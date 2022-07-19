// Basic functions on sparse tensors
#define TORCH_ASSERT_ONLY_METHOD_OPERATORS

#include <ATen/core/Tensor.h>
#include <ATen/Dispatch.h>
#include <ATen/InitialTensorOptions.h>
#include <ATen/Layout.h>
#include <ATen/Parallel.h>
#include <ATen/SparseCsrTensorImpl.h>
#include <ATen/SparseCsrTensorUtils.h>
#include <ATen/SparseTensorImpl.h>
#include <ATen/native/LinearAlgebraUtils.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/Functions.h>
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/_convert_indices_from_csr_to_coo.h>
#include <ATen/ops/_nnz_native.h>
#include <ATen/ops/_sparse_compressed_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_csr_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_csc_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_bsr_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_bsc_tensor_unsafe_native.h>
#include <ATen/ops/_sparse_coo_tensor_unsafe_native.h>
#include <ATen/ops/_validate_sparse_compressed_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_csr_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_csc_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_bsr_tensor_args_native.h>
#include <ATen/ops/_validate_sparse_bsc_tensor_args_native.h>
#include <ATen/ops/ccol_indices_native.h>
#include <ATen/ops/clone_native.h>
#include <ATen/ops/col_indices_native.h>
#include <ATen/ops/copy_native.h>
#include <ATen/ops/crow_indices_native.h>
#include <ATen/ops/empty.h>
#include <ATen/ops/empty_like_native.h>
#include <ATen/ops/empty_native.h>
#include <ATen/ops/resize_as_sparse_native.h>
#include <ATen/ops/resize_native.h>
#include <ATen/ops/row_indices_native.h>
#include <ATen/ops/select_native.h>
#include <ATen/ops/sparse_compressed_tensor_native.h>
#include <ATen/ops/sparse_csr_tensor_native.h>
#include <ATen/ops/sparse_csc_tensor_native.h>
#include <ATen/ops/sparse_bsr_tensor_native.h>
#include <ATen/ops/sparse_bsc_tensor_native.h>
#include <ATen/ops/values_native.h>
#endif

namespace at {
namespace native {

using namespace at::sparse_csr;

namespace {


} // end anonymous namespace

void _validate_sparse_compressed_tensor_args_worker(const Tensor& compressed_indices, const Tensor& plain_indices, const Tensor& values, const IntArrayRef size, const Layout& layout) {

  // Layout must be Sparse Compressed
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout, "validate_sparse_compressed_tensor_args", [&]{});

  const std::string layout_name = layoutToString(layout, /*upper=*/ true);
  const std::string compressed_indices_name = compressedIndicesName(layout);
  const std::string plain_indices_name = plainIndicesName(layout);

  // Layout Invariants
  TORCH_CHECK(
      plain_indices.layout() == kStrided && plain_indices.is_contiguous(),
      "expected ", plain_indices_name, " to be a strided and contiguous tensor");

  TORCH_CHECK(
      compressed_indices.layout() == kStrided && compressed_indices.is_contiguous(),
      "expected ", compressed_indices_name ," to be a strided and contiguous tensor");

  TORCH_CHECK(
      values.layout() == kStrided && values.is_contiguous(),
      "expected values to be a strided and contiguous tensor");

  // Shape and Strides invariants
  TORCH_CHECK(
              size.size() >= 2,
              "size of a batched ", layout_name, " tensor must have length >= 2, but got: ",
              size.size());
  TORCH_CHECK(
              compressed_indices.dim() >= 1,
              compressed_indices_name, " must have dim >= 1 but got ", compressed_indices_name, ".dim() = ",
              compressed_indices.dim());
  TORCH_CHECK(
              plain_indices.dim() >= 1,
              plain_indices_name, " must have dim >= 1 but got ", plain_indices_name, ".dim() = ",
              plain_indices.dim());
  TORCH_CHECK(
              values.dim() >= 1,
              "values must have dim >= 1 but got values.dim() = ",
              values.dim());

  TORCH_CHECK(
      compressed_indices.dim() == plain_indices.dim(),
      "number of dimensions of ", compressed_indices_name, " and ", plain_indices_name, " must be the same but got ",
      compressed_indices.dim(), " and ", plain_indices.dim(), ", respectively");

  AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(
      layout, "validate_sparse_compressed_tensor_args",
      [&] {
        TORCH_CHECK(
                    compressed_indices.dim() == values.dim(),
                    "number of dimensions of indices and values must be the same but got ",
                    compressed_indices.dim(), " and ", values.dim(), ", respectively");
      },
      [&] {
        TORCH_CHECK(
                    compressed_indices.dim() + 2 == values.dim(),
                    "number of dimensions of indices must be two less than the number of dimensions of the values but got ",
                    compressed_indices.dim(), " + 2 not equal to ", values.dim());
      });

  TORCH_CHECK(
      static_cast<size_t>(compressed_indices.dim()) == size.size() - 1,
      "number of dimensions of indices must be one less than the number of dimensions of the provided size but got ",
      compressed_indices.dim(), " not equal to ", size.size(), " - 1");

  int block_ndim = AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(layout, "validate_sparse_compressed_tensor_args", [&]{ return 0; }, [&]{ return 2; });
  IntArrayRef block_size = values.sizes().slice(values.dim() - block_ndim, block_ndim);
  int64_t numel_per_block = AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(layout, "validate_sparse_compressed_tensor_args",
                                [&]() -> int64_t { return 1; }, [&]() -> int64_t { return block_size[0] * block_size[1]; });
  int compressed_dim = compressedDimension(layout, size);
  int plain_dim = plainDimension(layout, size);

  // All batch sizes must be the same
  auto batch_size = size.slice(0, size.size() - 2);
  auto compressed_indices_batch_size = compressed_indices.sizes().slice(0, compressed_indices.dim() - 1);
  auto plain_indices_batch_size = plain_indices.sizes().slice(0, plain_indices.dim() - 1);
  auto values_batch_size = values.sizes().slice(0, values.dim() - 1 - block_ndim);
  TORCH_CHECK(
      batch_size == compressed_indices_batch_size &&
      batch_size == plain_indices_batch_size &&
      batch_size == values_batch_size,
      "all batch dimensions of the provided size (", batch_size, "), indices (",
      compressed_indices_batch_size,", ", plain_indices_batch_size, "), and values (",
      values_batch_size,") must be the same.");

  // Note, this check also enforces `compressed_indices.size(-1) >= 1`
  TORCH_CHECK(
              compressed_indices.size(-1) == (size[compressed_dim] + 1),
              compressed_indices_name, ".size(-1) must be equal to size[-", (size.size() - compressed_dim), "] + 1 (that is ",
              size[compressed_dim] + 1, "), but got: ", compressed_indices.size(-1));

  AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(layout, "validate_sparse_compressed_tensor_args",
      [&] {
        TORCH_CHECK(
                    plain_indices.numel() == values.numel(),
                    plain_indices_name, " and values must have the same number of elements, but got ", plain_indices_name, ".numel(): ",
                    plain_indices.numel(), ", values.numel(): ", values.numel());
      },
      [&] {
        TORCH_CHECK(
                    plain_indices.numel() * numel_per_block == values.numel(),
                    "number of ", plain_indices_name, " elements must be the same as the number of blocks in values, but got ",
                    plain_indices_name, ".numel() * numel_per_block: ", plain_indices.numel() * numel_per_block,
                    ", values.numel(): ", values.numel(),", numel_per_block: ", numel_per_block);
      });

  // Indices invariants
  AT_DISPATCH_INDEX_TYPES(compressed_indices.scalar_type(), "validate_sparse_compressed_tensor_args",
      [&] {
        Tensor compressed_indices_cpu = compressed_indices.to(kCPU);
        auto compressed_indices_data_ptr = compressed_indices_cpu.data_ptr<index_t>();
        auto batch_stride = compressed_indices_cpu.dim() >= 2 ? compressed_indices_cpu.stride(-2) : 0;
        auto compressed_dims = size[compressedDimension(layout, size)];
        for (const auto batch_id : c10::irange(batchCount(compressed_indices_cpu))) {
          TORCH_CHECK(
                      compressed_indices_data_ptr[batch_id*batch_stride] == 0,
                      "(Batch element ", batch_id, ") ",
                      ": 0th value of ", compressed_indices_name, " must be 0, but it is ", compressed_indices_data_ptr[batch_id*batch_stride]);
          TORCH_CHECK(
                      compressed_indices_data_ptr[batch_id*batch_stride + compressed_indices.size(-1) - 1] == plain_indices.size(-1),
                      "(Batch element ", batch_id, ") ",
                      "last value of ", compressed_indices_name, " should be equal to the length of ", plain_indices_name, ".");
          for (int i =  1; i <= compressed_dims; i++) {
            TORCH_CHECK(
                        compressed_indices_data_ptr[batch_id*batch_stride + i - 1] <= compressed_indices_data_ptr[batch_id*batch_stride + i],
                        "(Batch element ", batch_id, ") ",
                        "at position i = ", i, ", the condition ", compressed_indices_name, "[i - 1] <= ", compressed_indices_name, "[i] fails");
          }
        }
        if (plain_indices.numel() > 0) {
          TORCH_CHECK(0 <= plain_indices.min().item<index_t>(), plain_indices_name, ".min() should be greater or equal to zero");
          TORCH_CHECK(size[plain_dim] > plain_indices.max().item<index_t>(), "size[-", (size.size() - plain_dim),"] should be greater than ", plain_indices_name, ".max()");
        }
      });

  // Type Invariants
  auto compressed_indices_type = compressed_indices.scalar_type();
  auto plain_indices_type = plain_indices.scalar_type();
  TORCH_CHECK(
      compressed_indices_type == plain_indices_type,
      "both ", compressed_indices_name, " and ", plain_indices_name, " should have the same type, bot got ",
      compressed_indices_type, " and ", plain_indices_type, ", respectively");
  TORCH_CHECK(
      compressed_indices_type == kInt || compressed_indices_type == kLong,
      compressed_indices_name, " and ", plain_indices_name, " must be an int32 or int64 type, but got: ",
      compressed_indices_type);

  // Device Invariants
  TORCH_CHECK(
      plain_indices.get_device() == compressed_indices.get_device(),
      compressed_indices_name, " and ", plain_indices_name, " devices (",
      compressed_indices.get_device(),
      ", ",
      plain_indices.get_device(),
      ") must match");
  TORCH_CHECK(
      compressed_indices.get_device() == values.get_device(),
      "device of ", compressed_indices_name, " (",
      compressed_indices.get_device(),
      ") must match device of values (",
      values.get_device(),
      ")");
  TORCH_CHECK(
      values.device().type() == kCPU || values.device().type() == kCUDA,
      "device type of values (",
      values.device().type(),
      ") must be CPU or CUDA");

}

void _validate_sparse_compressed_tensor_args(const Tensor& compressed_indices, const Tensor& plain_indices, const Tensor& values, IntArrayRef size, Layout layout) {
  _validate_sparse_compressed_tensor_args_worker(compressed_indices, plain_indices, values, size, layout);
}

void _validate_sparse_csr_tensor_args(const Tensor& crow_indices, const Tensor& col_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(crow_indices, col_indices, values, size, kSparseCsr);
}

void _validate_sparse_csc_tensor_args(const Tensor& ccol_indices, const Tensor& row_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(ccol_indices, row_indices, values, size, kSparseCsc);
}

void _validate_sparse_bsr_tensor_args(const Tensor& crow_indices, const Tensor& col_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(crow_indices, col_indices, values, size, kSparseBsr);
}

void _validate_sparse_bsc_tensor_args(const Tensor& ccol_indices, const Tensor& row_indices, const Tensor& values, IntArrayRef size) {
  _validate_sparse_compressed_tensor_args_worker(ccol_indices, row_indices, values, size, kSparseBsc);
}

// Construction of CSR, CSC, BSR, and BSC tensors.

// Note: The usage of "Csr" in names like SparseCsrTensor,
// SparseCsrCPU, SparseCsrCUDA, and SparseCsrTensorImpl exists because
// of historical reasons (that ought to be removed in future) and does
// not mean that the corresponding functionality would be CSR layout
// only specific.
SparseCsrTensor new_compressed_tensor(const TensorOptions& options) {
  // TODO: remove this comment after enabling autograd support for CSR tensor
  // constructor.
  // TORCH_INTERNAL_ASSERT(impl::variable_excluded_from_dispatch());
  Layout layout = AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(options.layout(), "new_compressed_tensor", [&] { return the_layout; });
  DispatchKey dispatch_key;

  TORCH_CHECK_NOT_IMPLEMENTED(
    options.device().type() == kCPU || options.device().type() == kCUDA,
     "Could not run 'new_compressed_tensor' from the '", options.device(), "' device.)");

  if (options.device().is_cuda()) {
    dispatch_key = DispatchKey::SparseCsrCUDA;
  } else {
    dispatch_key = DispatchKey::SparseCsrCPU;
  }

  return detail::make_tensor<SparseCsrTensorImpl>(
      DispatchKeySet(dispatch_key), layout, options.dtype());
}


Tensor _sparse_compressed_tensor_unsafe(const Tensor& compressed_indices,
                                        const Tensor& plain_indices,
                                        const Tensor& values,
                                        IntArrayRef size,
                                        c10::optional<ScalarType> dtype,
                                        c10::optional<Layout> layout,
                                        c10::optional<Device> device,
                                        c10::optional<bool> pin_memory) {
  if (!layout) {
    AT_ERROR("sparse_compressed_tensor_unsafe expected sparse compressed tensor layout but got none");
  }
  Layout layout_ = layout.value();
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout_, "sparse_compressed_tensor_unsafe", [&]{});
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);
  SparseCsrTensor self = new_compressed_tensor(options);
  get_sparse_csr_impl(self)->set_member_tensors(compressed_indices, plain_indices, values, size);
  return self;
}

template <Layout required_layout>
Tensor _sparse_compressed_tensor_unsafe_template(const Tensor& compressed_indices,
                                                 const Tensor& plain_indices,
                                                 const Tensor& values,
                                                 IntArrayRef size,
                                                 c10::optional<ScalarType> dtype,
                                                 c10::optional<Layout> layout,
                                                 c10::optional<Device> device,
                                                 c10::optional<bool> pin_memory) {
  Layout layout_ = layout.value_or(required_layout);
  TORCH_CHECK(layout_ == required_layout, "sparse compressed layout must be ",required_layout, " but got ", layout_);
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);
  SparseCsrTensor self = new_compressed_tensor(options);
  get_sparse_csr_impl(self)->set_member_tensors(compressed_indices, plain_indices, values, size);
  return self;
}

#define SPARSE_COMPRESSED_TENSOR_UNSAFE(KIND, REQUIRED_LAYOUT)          \
  Tensor _sparse_##KIND##_tensor_unsafe(const Tensor& compressed_indices, \
                                        const Tensor& plain_indices,    \
                                        const Tensor& values,           \
                                        IntArrayRef size,               \
                                        c10::optional<ScalarType> dtype, \
                                        c10::optional<Layout> layout,   \
                                        c10::optional<Device> device,   \
                                        c10::optional<bool> pin_memory) { \
    return _sparse_compressed_tensor_unsafe_template<REQUIRED_LAYOUT>(compressed_indices, plain_indices, values, size, dtype, layout, device, pin_memory); \
  }

SPARSE_COMPRESSED_TENSOR_UNSAFE(csr, kSparseCsr);
SPARSE_COMPRESSED_TENSOR_UNSAFE(csc, kSparseCsc);
SPARSE_COMPRESSED_TENSOR_UNSAFE(bsr, kSparseBsr);
SPARSE_COMPRESSED_TENSOR_UNSAFE(bsc, kSparseBsc);

DimVector _estimate_sparse_compressed_tensor_size(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    Layout layout) {
  DimVector size = DimVector(IntArrayRef(plain_indices.sizes().data(), plain_indices.dim() - 1));
  int64_t compressed_dim = (plain_indices.size(-1) > 0 ? compressed_indices.size(-1) - 1 : 0);
  int64_t plain_dim = AT_DISPATCH_INTEGRAL_TYPES(plain_indices.scalar_type(), "estimate_sparse_compressed_tensor_size",
                                                 [&]() -> int64_t {
                                                   if (plain_indices.numel() > 0) {
                                                     return plain_indices.max().item<scalar_t>() + 1;
                                                   } else {
                                                     return 0;
                                                   }
                                                   });
  AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(layout, "estimate_sparse_compressed_tensor_size",
      [&]{
        size.push_back(compressed_dim);
        size.push_back(plain_dim);
      },
      [&]{
        size.push_back(plain_dim);
        size.push_back(compressed_dim);
      });
  return size;
}

// TODO: This constructor should probably use an ATen abstract method in order
// to make autograd dispatch available for the CSR constructor. See the relevant
// note in native_functions.yaml.
Tensor sparse_compressed_tensor(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {

  if (!layout) {
    AT_ERROR("sparse_compressed_tensor expected sparse compressed tensor layout but got none");
  }
  Layout layout_ = layout.value();
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout_, "sparse_compressed_tensor", [&]{});

  // See [Note: hacky wrapper removal for TensorOptions]
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);

  _validate_sparse_compressed_tensor_args_worker(compressed_indices, plain_indices, values, size, layout_);

  return at::native::_sparse_compressed_tensor_unsafe(
      compressed_indices,
      plain_indices,
      values,
      size,
      optTypeMetaToScalarType(options.dtype_opt()),
      options.layout_opt(),
      options.device_opt(),
      options.pinned_memory_opt());
}

Tensor sparse_compressed_tensor(
    const Tensor& compressed_indices,
    const Tensor& plain_indices,
    const Tensor& values,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory) {

  if (!layout) {
    AT_ERROR("sparse_compressed_tensor expected sparse compressed tensor layout but got none");
  }
  Layout layout_ = layout.value();
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(layout_, "sparse_compressed_tensor", [&]{});

  DimVector size = _estimate_sparse_compressed_tensor_size(compressed_indices, plain_indices, values, layout_);

  // See [Note: hacky wrapper removal for TensorOptions]
  TensorOptions options = TensorOptions().dtype(dtype).layout(layout_).device(device).pinned_memory(pin_memory);

  _validate_sparse_compressed_tensor_args_worker(compressed_indices, plain_indices, values, size, layout_);

  return at::native::_sparse_compressed_tensor_unsafe(
      compressed_indices,
      plain_indices,
      values,
      size,
      optTypeMetaToScalarType(options.dtype_opt()),
      options.layout_opt(),
      options.device_opt(),
      options.pinned_memory_opt());
}

#define SPARSE_COMPRESSED_TENSOR(KIND, REQUIRED_LAYOUT)                 \
  Tensor sparse_##KIND##_tensor(const Tensor& compressed_indices,       \
                                const Tensor& plain_indices,            \
                                const Tensor& values,                   \
                                c10::optional<ScalarType> dtype,        \
                                c10::optional<Layout> layout,           \
                                c10::optional<Device> device,           \
                                c10::optional<bool> pin_memory) {       \
    if (layout) {                                                       \
      TORCH_CHECK(layout.value() == REQUIRED_LAYOUT, "sparse " # KIND " layout must be ", REQUIRED_LAYOUT, " but got ", layout.value()); \
    }                                                                   \
    c10::optional<Layout> layout_(REQUIRED_LAYOUT);                     \
    return at::native::sparse_compressed_tensor(compressed_indices, plain_indices, values, dtype, layout_, device, pin_memory); \
  }                                                                     \
  Tensor sparse_##KIND##_tensor(const Tensor& compressed_indices,       \
                                const Tensor& plain_indices,            \
                                const Tensor& values,                   \
                                IntArrayRef size,                       \
                                c10::optional<ScalarType> dtype,        \
                                c10::optional<Layout> layout,           \
                                c10::optional<Device> device,           \
                                c10::optional<bool> pin_memory) {       \
    if (layout) {                                                       \
      TORCH_CHECK(layout.value() == REQUIRED_LAYOUT, "sparse " # KIND " layout must be ", REQUIRED_LAYOUT, " but got ", layout.value()); \
    }                                                                   \
    c10::optional<Layout> layout_(REQUIRED_LAYOUT);                     \
    return at::native::sparse_compressed_tensor(compressed_indices, plain_indices, values, size, dtype, layout_, device, pin_memory); \
  }

SPARSE_COMPRESSED_TENSOR(csr, kSparseCsr)
SPARSE_COMPRESSED_TENSOR(csc, kSparseCsc)
SPARSE_COMPRESSED_TENSOR(bsr, kSparseBsr)
SPARSE_COMPRESSED_TENSOR(bsc, kSparseBsc)

Tensor empty_sparse_compressed(
    IntArrayRef size,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory,
    c10::optional<MemoryFormat> optional_memory_format) {
  check_size_nonnegative(size);
  TORCH_CHECK(size.size() >= 2, "torch.empty: Only batched sparse compressed (non-block) tensors are supported, but got size ", size);

  // Strided is the default layout for torch.empty.
  Layout layout_ = layout.value_or(Layout::Strided);

  // torch.empty cannot be used to create blocked tensors because its
  // API lacks a method to specify the block size.
  AT_DISPATCH_SPARSE_COMPRESSED_NONBLOCK_LAYOUTS(layout_, "empty_sparse_compressed", [&]{});

  int64_t nnz = 0;
  auto compressed_indices_size = DimVector(size.slice(0, size.size() - 2));
  auto plain_indices_and_values_size = DimVector(size.slice(0, size.size() - 2));
  compressed_indices_size.push_back(size[compressedDimension(layout_, size)] + 1);
  plain_indices_and_values_size.push_back(nnz);

  TensorOptions options = TensorOptions().dtype(ScalarType::Long).layout(Layout::Strided).device(device).pinned_memory(pin_memory);
  auto compressed_indices = at::empty(compressed_indices_size, options);
  auto plain_indices = at::empty(plain_indices_and_values_size, options);
  auto values = at::empty(plain_indices_and_values_size, options.dtype(dtype));

  return at::native::_sparse_compressed_tensor_unsafe(compressed_indices,
                                                      plain_indices,
                                                      values,
                                                      size,
                                                      dtype,
                                                      layout,
                                                      device,
                                                      pin_memory);
}

const Tensor& resize_sparse_csr_(
    const Tensor& self,
    IntArrayRef size,
    c10::optional<MemoryFormat> optional_memory_format) {
  check_size_nonnegative(size);
  TORCH_CHECK(size.size() >= 2, "torch.resize_: Only batched sparse CSR matrices are supported, but got size ", size);
  TORCH_CHECK(
      self.size(-1) <= size[size.size() - 1],
      "torch.resize_: Resizing columns of sparse CSR tensors to a smaller value is not supported. ",
      "The original number of columns is ",
      self.size(-1),
      " while the requested new number of columns is ", size[size.size() - 1], ".");
  get_sparse_csr_impl(self)->resize_(self._nnz(), size);
  return self;
}

Tensor& copy_sparse_compressed_(Tensor& self, const Tensor& src, bool non_blocking) {
  AT_DISPATCH_ALL_SPARSE_COMPRESSED_LAYOUTS(self.layout(), "copy_sparse_compressed_", [&]{});
  TORCH_CHECK(
      self.layout() == src.layout(),
      "torch.copy_: copy of sparse compressed tensors having different layouts is not supported.",
      " self layout is ", self.layout(), " and src layout is ", src.layout());
  TORCH_CHECK(
      self._nnz() == src._nnz(),  // actually, values copy allows different shapes as long as operands are broadcastable
      "torch.copy_: only sparse compressed tensors with the same number of specified elements are supported.");
  auto self_compressed_dim = compressedDimension(self.layout(), self.sizes());
  auto src_compressed_dim = compressedDimension(src.layout(), src.sizes());
  auto self_compressed_dims = self.size(self_compressed_dim);
  auto src_compressed_dims = src.size(compressedDimension(src.layout(), src.sizes()));
  if (self_compressed_dim == src_compressed_dim) {
    TORCH_CHECK(self_compressed_dims == src_compressed_dims,
                "torch.copy_: expected shapes of self and src to match along dimension ",
                self_compressed_dim, " for ",
                self.layout(), " layout but the corresponding dimensions of self and src are ",
                self_compressed_dims, " and ", src_compressed_dims, ", respecitvely.");
  } else {
    TORCH_CHECK(self_compressed_dims == src_compressed_dims,
                "torch.copy_: expected shapes of self and src to match along dimensions ",
                self_compressed_dim, " and ", src_compressed_dim, ", respectively, for ",
                self.layout(), " layout but the corresponding dimensions of self and src are ",
                self_compressed_dims, " and ", src_compressed_dims, ", respecitvely.");
  }
  AT_DISPATCH_PLAIN_SPARSE_COMPRESSED_LAYOUTS(self.layout(), "copy_sparse_compressed_",
                                              [&]{},
                                              [&]{
                                                auto self_values = self.values();
                                                auto src_values = src.values();
                                                auto self_block_size = DimVector(self_values.sizes().slice(self_values.dim()-2, 2));
                                                auto src_block_size = DimVector(src_values.sizes().slice(src_values.dim()-2, 2));
                                                TORCH_CHECK(self_block_size == src_block_size,
                                                            "torch.copy_: copy of sparse compressed tensors having different block sizes is not supported.",
                                                            " self and src block sizes are ", self_block_size, " and ", src_block_size, ", respectivly.");
                                              });
  AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(self.layout(), "copy_sparse_compressed_",
                                            [&]{
                                              self.crow_indices().copy_(src.crow_indices(), non_blocking);
                                              self.col_indices().copy_(src.col_indices(), non_blocking);
                                            },
                                            [&]{
                                              self.ccol_indices().copy_(src.ccol_indices(), non_blocking);
                                              self.row_indices().copy_(src.row_indices(), non_blocking);
                                            });
  self.values().copy_(src.values(), non_blocking);
  return self;
}

// Access members of CSR tensors.
int64_t _nnz_sparse_csr(const SparseCsrTensor& self) {
  return get_sparse_csr_impl(self)->nnz();
}

Tensor values_sparse_csr(const Tensor& self) {
  return get_sparse_csr_impl(self)->values().alias();
}

Tensor crow_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_ROW_COMPRESSED_LAYOUTS(self.layout(),
                                                   "crow_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->compressed_indices().alias(); });
}

Tensor col_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_ROW_COMPRESSED_LAYOUTS(self.layout(),
                                                   "col_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->plain_indices().alias(); });
}

Tensor ccol_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_COL_COMPRESSED_LAYOUTS(self.layout(),
                                                   "ccol_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->compressed_indices().alias(); });
}

Tensor row_indices_sparse_csr(const Tensor& self) {
  return AT_DISPATCH_SPARSE_COL_COMPRESSED_LAYOUTS(self.layout(),
                                                   "row_indices",
                                                   [&]{ return get_sparse_csr_impl(self)->plain_indices().alias(); });
}

bool _is_same_size_as_sparse_csr(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  return self.sizes().equals(src.sizes());
}

const SparseCsrTensor& resize_as_sparse_csr_(
    const SparseCsrTensor& self,
    const SparseCsrTensor& src) {
  TORCH_CHECK(
      src.is_sparse_csr() && self.is_sparse_csr(),
      "resize_as_sparse_csr_: layout for self and src must be sparse_csr but got ",
      self.layout(),
      " for self, and ",
      src.layout(),
      " for src");
  if (!_is_same_size_as_sparse_csr(self, src)) {
    get_sparse_csr_impl(self)->resize_as_sparse_csr_tensor_(src);
  }
  return self;
}

SparseCsrTensor clone_sparse_compressed(
                                        const SparseCsrTensor& self,
                                        c10::optional<c10::MemoryFormat> optional_memory_format) {
  TORCH_CHECK(
      !optional_memory_format.has_value(),
      "unsupported memory format option ",
      optional_memory_format.value());
  TensorOptions options = self.options();
  auto compressed_indices = AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(self.layout(),
                                                                      "clone_sparse_compressed",
                                                                      [&]{ return self.crow_indices(); },
                                                                      [&]{ return self.ccol_indices(); });
  auto plain_indices = AT_DISPATCH_ROW_SPARSE_COMPRESSED_LAYOUTS(self.layout(),
                                                                 "clone_sparse_compressed",
                                                                 [&]{ return self.col_indices(); },
                                                                 [&]{ return self.row_indices(); });
  return at::native::_sparse_compressed_tensor_unsafe(
                                                      compressed_indices.clone(),
                                                      plain_indices.clone(),
                                                      self.values().clone(),
                                                      self.sizes(),
                                                      optTypeMetaToScalarType(options.dtype_opt()),
                                                      options.layout_opt(),
                                                      options.device_opt(),
                                                      options.pinned_memory_opt());
}

Tensor empty_like_sparse_csr(
    const Tensor& self,
    c10::optional<ScalarType> dtype,
    c10::optional<Layout> layout,
    c10::optional<Device> device,
    c10::optional<bool> pin_memory,
    c10::optional<c10::MemoryFormat> optional_memory_format) {
  TensorOptions options_ = TensorOptions().dtype(dtype).layout(layout).device(device).pinned_memory(pin_memory);
  TensorOptions options =
      self.options()
          .merge_in(options_)
          .merge_memory_format(optional_memory_format);

  if (options.layout() == kSparseCsr) {
    auto result = at::native::_sparse_csr_tensor_unsafe(
        self.crow_indices().clone(),
        self.col_indices().clone(),
        at::empty(self.values().sizes(), options.layout(kStrided)),
        self.sizes(),
        optTypeMetaToScalarType(options.dtype()),
        self.layout(),
        options.device());
    return result;
  } else if (options.layout() == kStrided) {
    return at::native::empty_like(self, dtype, layout, device, pin_memory, optional_memory_format);
  } else {
    TORCH_CHECK(false, "Layout ", options.layout(), " is not supported");
  }
}

Tensor select_sparse_csr(const Tensor& self, int64_t dim, int64_t index) {
  TORCH_INTERNAL_ASSERT(self.is_sparse_csr());
  TORCH_CHECK_INDEX(self.dim() != 0, "select() cannot be applied to a 0-dim tensor.");
  dim = maybe_wrap_dim(dim, self.dim());
  auto size = self.size(dim);
  if (index < -size || index >= size) {
    TORCH_CHECK_INDEX(false, "select(): index ", index, " out of range for tensor of size ",
                   self.sizes(), " at dimension ", dim);
  }
  if (index < 0) {
    index += size;
  }

  TORCH_INTERNAL_ASSERT(dim >= 0 && dim < self.dim());

  auto new_sizes = DimVector(self.sizes());
  new_sizes.erase(new_sizes.begin() + dim);
  auto options = self.options();

  // Selecting batch dimension
  if (dim < self.dim() - 2) {
    return at::native::_sparse_csr_tensor_unsafe(
        self.crow_indices().select(dim, index),
        self.col_indices().select(dim, index),
        self.values().select(dim, index),
        new_sizes,
        optTypeMetaToScalarType(options.dtype_opt()),
        options.layout_opt(),
        options.device_opt(),
        options.pinned_memory_opt());
  } else {
    TORCH_CHECK(self.dim() == 2, "select(): selecting rows or columns is not implemented for batched sparse CSR tensors.")
    // Converting to COO and calling select is slighly slower than operating on the CSR indices directly
    // for constructing a COO vector, however current version is more readable and easier to understand.
    return self.to_sparse().select(dim, index);
  }
}

} // namespace native
} // namespace at
