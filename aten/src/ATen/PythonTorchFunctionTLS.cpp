#include <ATen/PythonTorchFunctionTLS.h>
#include <c10/core/TensorImpl.h>

namespace at {
namespace impl {

static thread_local PythonTorchFunctionTLS pythonTorchFunctionState;

void PythonTorchFunctionTLS::set_mode(std::shared_ptr<c10::SafePyObject> mode) {
  pythonTorchFunctionState.mode_ = std::move(mode);
}

const std::shared_ptr<c10::SafePyObject>& PythonTorchFunctionTLS::get_mode() {
  return pythonTorchFunctionState.mode_;
}

void PythonTorchFunctionTLS::swap_mode(std::shared_ptr<c10::SafePyObject>& mode) {
  pythonTorchFunctionState.mode_.swap(mode);
}

void PythonTorchFunctionTLS::set_disabled(bool disabled) {
  pythonTorchFunctionState.disabled_ = disabled;
}

bool PythonTorchFunctionTLS::is_disabled() {
  return pythonTorchFunctionState.disabled_;
}

void PythonTorchFunctionTLS::set_state(const PythonTorchFunctionTLS& state) {
  pythonTorchFunctionState = state;
}

const PythonTorchFunctionTLS& PythonTorchFunctionTLS::get_state() {
  return pythonTorchFunctionState;
}

} // namespace impl
} // namespace at
