#include <torch/csrc/python_headers.h>

#include <torch/csrc/Dtype.h>
#include <torch/csrc/DynamicTypes.h>
#include <torch/csrc/Exceptions.h>
#include <torch/csrc/Layout.h>
#include <torch/csrc/PythonTypes.h>
#include <torch/csrc/autograd/generated/VariableType.h>
#include <torch/csrc/utils/cuda_enabled.h>
#include <torch/csrc/utils/cuda_lazy_init.h>
#include <torch/csrc/utils/object_ptr.h>
#include <torch/csrc/Storage.h>
#include <torch/csrc/Device.h>

#include <ATen/ATen.h>

#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace torch {
namespace {
std::array<THPDtype*, static_cast<int>(at::ScalarType::NumOptions)> dtype_registry = {};

std::array<THPLayout*, static_cast<int>(at::Layout::NumOptions)> layout_registry = {};

at::Backend get_backend(bool is_cuda, bool is_sparse) {
  if (is_cuda) {
    if (is_sparse){
      return at::Backend::SparseCUDA;
    } else {
      return at::Backend::CUDA;
    }
  } else {
    if (is_sparse){
      return at::Backend::SparseCPU;
    } else {
      return at::Backend::CPU;
    }
  }
}

at::DeprecatedTypeProperties* get_type(at::Backend backend, at::ScalarType scalarType) {
  if (isSparse(backend) && scalarType == at::kHalf) {
    return nullptr;
  }
  return &at::getDeprecatedTypeProperties(backend, scalarType);
}
} // namespace

void registerDtypeObject(THPDtype *dtype, at::ScalarType scalarType) {
  dtype_registry[static_cast<int>(scalarType)] = dtype;
}

void registerLayoutObject(THPLayout *thp_layout, at::Layout layout) {
  layout_registry[static_cast<int>(layout)] = thp_layout;
}

THPDtype* getTHPDtype(at::ScalarType scalarType) {
  auto dtype = dtype_registry[static_cast<int>(scalarType)];
  if (!dtype) {
    throw std::invalid_argument("unsupported scalarType");
  }
  return dtype;
}

THPLayout* getTHPLayout(at::Layout layout) {
  auto thp_layout = layout_registry[static_cast<int>(layout)];
  if (!thp_layout) {
    throw std::invalid_argument("unsupported at::Layout");
  }
  return thp_layout;
}

PyObject* createPyObject(const at::Storage& storage) {
  // TODO: https://github.com/pytorch/pytorch/issues/47442
  if (storage.device_type() == at::DeviceType::Meta) {
    TORCH_CHECK_NOT_IMPLEMENTED(false, "python bindings for meta storage objects not supported");
  }
  if (storage.data() == nullptr && storage.nbytes() != 0) {
    TORCH_CHECK_NOT_IMPLEMENTED(false, "python bindings to nullptr storage (e.g., from torch.Tensor._make_wrapper_subclass) are currently unsafe and thus disabled.  See https://github.com/pytorch/pytorch/issues/61669 for more details");
  }
  PyTypeObject* type = reinterpret_cast<PyTypeObject*>(THPStorageClass);
  auto obj = THPObjectPtr(type->tp_alloc(type, 0));
  if (!obj) throw python_error();
  ((THPVoidStorage*)obj.get())->cdata = at::Storage(/* copy */ storage).unsafeReleaseStorageImpl();
  return obj.release();
}

PyTypeObject* loadTypedStorageTypeObject() {
  PyObject* storage_module = PyImport_ImportModule("torch.storage");
  TORCH_INTERNAL_ASSERT(storage_module && PyModule_Check(storage_module));

  PyObject* typed_storage_obj = PyObject_GetAttrString(storage_module, "_TypedStorage");
  TORCH_INTERNAL_ASSERT(typed_storage_obj && PyType_Check(typed_storage_obj));
  return reinterpret_cast<PyTypeObject*>(
      PyObject_GetAttrString(storage_module, "_TypedStorage"));
}

PyTypeObject* getTypedStorageTypeObject() {
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static PyTypeObject* typed_storage_type_obj = loadTypedStorageTypeObject();
  return typed_storage_type_obj;
}

bool isStorage(PyObject* obj)
{
  if (PyObject_TypeCheck(obj, getTypedStorageTypeObject())) {
    return true;
  }
  auto obj_type = Py_TYPE(obj);

  return obj_type == reinterpret_cast<PyTypeObject*>(THPStorageClass);
}

at::Storage createStorageGetType(PyObject* obj, at::ScalarType& scalar_type, bool& is_typed_storage)
{
  is_typed_storage = PyObject_TypeCheck(obj, getTypedStorageTypeObject());
  PyObject* untyped_storage_obj;

  if (is_typed_storage) {
    // NOTE: `PyObject_GetAttrString` increments the refcounts to `dtype` and
    // `_storage`, so we must decrement them. The refcounts will still stay
    // nonzero since the `_TypedStorage` maintains a reference.
    PyObject* dtype_obj = PyObject_GetAttrString(obj, "dtype");
    TORCH_INTERNAL_ASSERT(dtype_obj);
    Py_DECREF(dtype_obj);

    TORCH_INTERNAL_ASSERT(THPDtype_Check(dtype_obj));
    scalar_type = reinterpret_cast<THPDtype*>(dtype_obj)->scalar_type;

    untyped_storage_obj = PyObject_GetAttrString(obj, "_storage");
    TORCH_INTERNAL_ASSERT(untyped_storage_obj);
    Py_DECREF(untyped_storage_obj);

  } else {
    scalar_type = at::kByte;
    untyped_storage_obj = obj;
  }

  if (Py_TYPE(untyped_storage_obj) != reinterpret_cast<PyTypeObject*>(THPStorageClass)) {
    throw TypeError("not a storage '%s'", Py_TYPE(obj)->tp_name);
  }

  c10::StorageImpl* impl = static_cast<c10::StorageImpl*>(((THPVoidStorage*)untyped_storage_obj)->cdata);
  c10::DeviceType device_type = impl->device().type();

  at::Backend backend;
  if (device_type == at::kCPU) {
    backend = at::Backend::CPU;
  } else if (device_type == at::kCUDA) {
    backend = at::Backend::CUDA;
  } else {
    TORCH_CHECK(false, "Invalid device for storage: ", device_type);
  }

  auto type_properties = get_type(backend, at::kByte);

  return type_properties->unsafeStorageFromTH(((THPVoidStorage*)untyped_storage_obj)->cdata, true);
}

at::Storage createStorage(PyObject* obj) {
  at::ScalarType scalar_type;
  bool is_typed_storage = false;
  return createStorageGetType(obj, scalar_type, is_typed_storage);
}

}  // namespace
