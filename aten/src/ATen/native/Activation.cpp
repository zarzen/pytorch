#include <ATen/native/Activation.h>

#include <ATen/ATen.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/Dispatch.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/Parallel.h>
#if defined(C10_MOBILE) && defined(USE_XNNPACK)
#include <ATen/native/xnnpack/Engine.h>
#endif
#include <ATen/core/DistributionsHelper.h>

#include <c10/util/irange.h>
#if AT_MKLDNN_ENABLED()
#include <ATen/native/mkldnn/MKLDNNCommon.h>
#include <ATen/native/mkldnn/Utils.h>
#endif

namespace at {
namespace meta {
// computes `result = self <= threshold ? value : other`
// other is `self` in threshold() and `grad` in threshold_backward()
TORCH_META_FUNC(threshold)(const Tensor& self, const Scalar& threshold, const Scalar& value) {
  const Tensor& result = maybe_get_output();
  build(TensorIteratorConfig()
    .set_check_mem_overlap(false)  // threshold is idempotent, so overlap is okay
    .add_output(result)
    .add_input(self)
    .add_input(self) // other
    .allow_cpu_scalars(true)
    .promote_inputs_to_common_dtype(true)
    .cast_common_dtype_to_outputs(true)
    .enforce_safe_casting_to_output(true));
}
// computes `result = self <= threshold ? value : other`
// other is `self` in threshold() and `grad` in threshold_backward()
TORCH_META_FUNC(threshold_backward)(const Tensor& grad, const Tensor& self, const Scalar& threshold) {
  const Tensor& gradInput = maybe_get_output();
  build(TensorIteratorConfig()
    .set_check_mem_overlap(false)  // threshold is idempotent, so overlap is okay
    .add_output(gradInput)
    .add_input(self)
    .add_input(grad)  // other
    .allow_cpu_scalars(true)
    .promote_inputs_to_common_dtype(true)
    .cast_common_dtype_to_outputs(true)
    .enforce_safe_casting_to_output(true));
}

TORCH_META_FUNC(elu) (
  const Tensor& self, const Scalar& alpha, const Scalar& scale, const Scalar& input_scale
) {
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(elu_backward) (
  const Tensor& grad_output,
  const Scalar& alpha,
  const Scalar& scale,
  const Scalar& input_scale,
  bool is_result,
  const Tensor& self_or_result
) {
  TORCH_CHECK(
    !is_result || alpha.to<double>() >= 0.0,
    "In-place elu backward calculation is triggered with a negative slope which is not supported. "
    "This is caused by calling in-place forward function with a negative slope, "
    "please call out-of-place version instead.");

  build_borrowing_binary_op(maybe_get_output(), grad_output, self_or_result);
}

TORCH_META_FUNC(silu) (const Tensor& self) {
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(silu_backward) (
  const Tensor& grad_output, const Tensor& input
) {
  build_borrowing_binary_op(maybe_get_output(), grad_output, input);
}

TORCH_META_FUNC(mish) (const Tensor& self) {
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(softplus) (
  const Tensor& self, const Scalar& beta, const Scalar& threshold
) {
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(softplus_backward) (
  const Tensor& grad_output,
  const Tensor& self,
  const Scalar& beta,
  const Scalar& threshold
) {
  build_borrowing_binary_op(maybe_get_output(), grad_output, self);
}

TORCH_META_FUNC(leaky_relu) (
  const Tensor& self, const Scalar& negval
) {
  build_unary_op(maybe_get_output(), self);
}

// Note: leakyReLu backward calculation doesn't support in-place call with negative slope.
// The reason is that for in-place forward call, the forward result will be saved into autograd
// node instead of the input itself, when calculating backward gradient, there is no way to know
// whether the original input for current node is positive or not if the input slope is
// negative. eg. forward is 2, slope is -0.2, the original input for this node could be
// either 2, or -10, so no way to get a correct backward gradient in this case.
TORCH_META_FUNC(leaky_relu_backward) (
  const Tensor& grad_output,
  const Tensor& self_or_result,
  const Scalar& negval,
  bool is_result
) {
  TORCH_CHECK(
    !is_result || negval.to<double>() >= 0.0,
    "In-place leakyReLu backward calculation is triggered with a negative slope which is not supported. "
    "This is caused by calling in-place forward function with a negative slope, "
    "please call out-of-place version instead. File an issue at https://github.com/pytorch/pytorch if you do "
    "require supporting in-place leakRelu backward calculation with negative slope");

  build_borrowing_binary_op(maybe_get_output(), self_or_result, grad_output);
}

TORCH_META_FUNC(hardsigmoid) (const Tensor& self) {
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(hardsigmoid_backward) (const Tensor& grad_output, const Tensor& self) {
  build_borrowing_binary_op(maybe_get_output(), grad_output, self);
}

TORCH_META_FUNC(hardshrink) (const Tensor & self, const Scalar& lambd) {
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(hardshrink_backward) (
  const Tensor & grad, const Tensor & self, const Scalar& lambd
) {
  build_borrowing_binary_op(maybe_get_output(), grad, self);
}

static inline void softshrink_check(const Scalar& lambd) {
  double lamb = lambd.to<double>();
  TORCH_CHECK(lamb >= 0, "lambda must be greater or equal to 0, but found to be ", lamb, ".");
}

TORCH_META_FUNC(softshrink) (
  const Tensor & self, const Scalar& lambd
) {
  softshrink_check(lambd);
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(softshrink_backward) (
  const Tensor & grad, const Tensor & self, const Scalar& lambd
) {
  build_borrowing_binary_op(maybe_get_output(), grad, self);
}

TORCH_META_FUNC(gelu) (const Tensor & self, c10::string_view approximate) {
  build_unary_op(maybe_get_output(), self);
}

TORCH_META_FUNC(gelu_backward) (
  const Tensor& grad, const Tensor& self, c10::string_view approximate
) {
  build_borrowing_binary_op(maybe_get_output(), grad, self);
}

} // namespace meta

namespace native {

static const double SELU_ALPHA = 1.6732632423543772848170429916717;
static const double SELU_SCALE = 1.0507009873554804934193349852946;

DEFINE_DISPATCH(elu_stub);
DEFINE_DISPATCH(elu_backward_stub);
DEFINE_DISPATCH(softplus_stub);
DEFINE_DISPATCH(softplus_backward_stub);
DEFINE_DISPATCH(log_sigmoid_cpu_stub);
DEFINE_DISPATCH(log_sigmoid_backward_stub);
DEFINE_DISPATCH(threshold_stub);
DEFINE_DISPATCH(hardtanh_backward_stub);
DEFINE_DISPATCH(hardsigmoid_stub);
DEFINE_DISPATCH(hardsigmoid_backward_stub);
DEFINE_DISPATCH(hardswish_stub);
DEFINE_DISPATCH(hardswish_backward_stub);
DEFINE_DISPATCH(hardshrink_stub);
DEFINE_DISPATCH(softshrink_stub);
DEFINE_DISPATCH(shrink_backward_stub);
DEFINE_DISPATCH(leaky_relu_stub);
DEFINE_DISPATCH(leaky_relu_backward_stub);
DEFINE_DISPATCH(silu_stub);
DEFINE_DISPATCH(silu_backward_stub);
DEFINE_DISPATCH(mish_stub);
DEFINE_DISPATCH(mish_backward_stub);
DEFINE_DISPATCH(prelu_cpu_stub);
DEFINE_DISPATCH(prelu_backward_cpu_stub);

TORCH_IMPL_FUNC(elu_out) (
  const Tensor& self, const Scalar& alpha, const Scalar& scale, const Scalar& input_scale, const Tensor& result
) {
  elu_stub(device_type(), *this, alpha, scale, input_scale);
}

TORCH_IMPL_FUNC(elu_backward_out) (
  const Tensor& grad_output,
  const Scalar& alpha,
  const Scalar& scale,
  const Scalar& input_scale,
  bool is_result,
  const Tensor& self_or_result,
  const Tensor& grad_input
) {
  elu_backward_stub(device_type(), *this, alpha, scale, input_scale, is_result);
}

TORCH_IMPL_FUNC(silu_out) (
  const Tensor& self, const Tensor& result
) {
  silu_stub(device_type(), *this);
}

TORCH_IMPL_FUNC(silu_backward_out) (
  const Tensor& grad_output, const Tensor& input, const Tensor& grad_input
) {
  silu_backward_stub(device_type(), *this);
}

TORCH_IMPL_FUNC(mish_out) (
  const Tensor& self, const Tensor& result
) {
  mish_stub(device_type(), *this);
}

TORCH_IMPL_FUNC(softplus_out) (
  const Tensor& self, const Scalar& beta, const Scalar& threshold, const Tensor& result
) {
  softplus_stub(device_type(), *this, beta, threshold);
}

TORCH_IMPL_FUNC(softplus_backward_out) (
  const Tensor& grad_output,
  const Tensor& self,
  const Scalar& beta,
  const Scalar& threshold,
  const Tensor& grad_input
) {
  softplus_backward_stub(device_type(), *this, beta, threshold);
}

TORCH_IMPL_FUNC(leaky_relu_out) (
  const Tensor& self, const Scalar& negval, const Tensor& result
) {
  leaky_relu_stub(device_type(), *this, negval);
}

TORCH_IMPL_FUNC(leaky_relu_backward_out) (
  const Tensor& grad_output,
  const Tensor& self_or_result,
  const Scalar& negval,
  bool is_result,
  const Tensor& grad_input
) {
  leaky_relu_backward_stub(device_type(), *this, negval);
}

TORCH_IMPL_FUNC(hardsigmoid_out) (
  const Tensor& self, const Tensor& result
) {
  hardsigmoid_stub(device_type(), *this);
}

TORCH_IMPL_FUNC(hardsigmoid_backward_out) (
  const Tensor& grad_output, const Tensor& self, const Tensor& grad_input
) {
  hardsigmoid_backward_stub(device_type(), *this);
}

TORCH_IMPL_FUNC(hardshrink_out) (
  const Tensor & self, const Scalar& lambd, const Tensor& result
) {
  hardshrink_stub(device_type(), *this, lambd);
}

TORCH_IMPL_FUNC(hardshrink_backward_out) (
  const Tensor & grad, const Tensor & self, const Scalar& lambd, const Tensor& grad_input
) {
  shrink_backward_stub(device_type(), *this, lambd);
}

TORCH_IMPL_FUNC(softshrink_out) (
  const Tensor & self, const Scalar& lambd, const Tensor& result
) {
  softshrink_stub(device_type(), *this, lambd);
}

TORCH_IMPL_FUNC(softshrink_backward_out) (
  const Tensor & grad, const Tensor & self, const Scalar& lambd, const Tensor& grad_input
) {
  shrink_backward_stub(device_type(), *this, lambd);
}

bool use_mkldnn(const Tensor& input) {
#if AT_MKLDNN_ENABLED()
  if (!at::globalContext().userEnabledMkldnn()) {
    return false;
  }
  if (!input.is_contiguous() || input.numel() == 1) {
    return false;
  }
  return (input.is_mkldnn()) || // input is mkldnn Tensor
    (input.device().is_cpu() &&
    (((input.scalar_type() == kBFloat16) && mkldnn_bf16_device_check()) ||
    (input.scalar_type() == kFloat))); // input is dense layout and bfloat16/float32
#endif
  return false;
}

TORCH_IMPL_FUNC(gelu_out_cpu) (
  const Tensor& self, c10::string_view approximate, const Tensor& result
) {
auto approximate_type = get_gelutype_enum(approximate);
#if AT_MKLDNN_ENABLED()
  if (use_mkldnn(self) && (approximate_type == GeluType::None)) {
    const ideep::tensor& x = itensor_from_tensor(self);
    ideep::tensor y = itensor_from_tensor(result);
    ideep::eltwise_forward::compute(
      x, y, ideep::algorithm::eltwise_gelu_erf, ideep::prop_kind::forward_training, /*alpha*/ 0.0);
  } else {
    GeluKernel(kCPU, *this, approximate_type);
  }
#else
  GeluKernel(kCPU, *this, approximate_type);
#endif
}

TORCH_IMPL_FUNC(gelu_backward_out_cpu) (
  const Tensor& grad, const Tensor& self, c10::string_view approximate, const Tensor& grad_input
) {
auto approximate_type = get_gelutype_enum(approximate);
#if AT_MKLDNN_ENABLED()
  if (use_mkldnn(self) && (approximate_type == GeluType::None)) {
    const ideep::tensor& x = itensor_from_tensor(self);
    ideep::tensor grady = itensor_from_tensor(grad);
    ideep::tensor gradx = itensor_from_tensor(grad_input);
    ideep::eltwise_backward::compute(x, grady, gradx,
      ideep::algorithm::eltwise_gelu_erf, /*alpha*/ 0.0);
  } else {
    GeluBackwardKernel(kCPU, *this, approximate_type);
  }
#else
  GeluBackwardKernel(kCPU, *this, approximate_type);
#endif
}

Tensor hardtanh(const Tensor& self, const Scalar& min, const Scalar& max) {
  Tensor result = at::empty_like(self);
  return at::hardtanh_out(result, self, min, max);
}

Tensor& hardtanh_out(const Tensor& self, const Scalar& min, const Scalar& max, Tensor& result) {
  TORCH_CHECK(self.scalar_type() != at::kBool,
  "Bool inputs not supported for hardtanh");
  //preserve legacy behavior of boundaries not causing type promotion
  Scalar min_, max_;
  if (at::isIntegralType(self.scalar_type(), /*include_bool*/false)) {
    int64_t minval = min.toLong();
    int64_t maxval = max.toLong();
    TORCH_CHECK(self.dtype() != at::kByte || (minval >= 0 &&
       maxval >=0), "cannot do hardtanh on an unsigned type with negative limits");
    min_ = minval;
    max_ = maxval;
  } else {
    min_ = min;
    max_ = max;
  }
  return at::clamp_out(result, self, min_, max_);
}

Tensor& hardtanh_(Tensor& self, const Scalar& min, const Scalar& max) {
  return at::hardtanh_out(self, self, min, max);
}

Tensor& hardtanh_backward_out(const Tensor& grad_output, const Tensor& self, const Scalar& min, const Scalar& max, Tensor& grad_input) {
  auto iter = TensorIterator::borrowing_binary_op(grad_input, grad_output, self);
  hardtanh_backward_stub(iter.device_type(), iter, min, max);
  return grad_input;
}

Tensor hardtanh_backward(const Tensor& grad_output, const Tensor& self, const Scalar& min, const Scalar& max) {
  Tensor result;
  auto iter = TensorIterator::borrowing_binary_op(result, grad_output, self);
  hardtanh_backward_stub(iter.device_type(), iter, min, max);
  return iter.output();
}

Tensor hardswish(const Tensor& self) {
  #if defined(C10_MOBILE) && defined(USE_XNNPACK)
  if (xnnpack::use_hardswish(self)) {
    return xnnpack::hardswish(self);
  }
  #endif
  Tensor result;
  auto iter = TensorIterator::unary_op(result, self);
  hardswish_stub(iter.device_type(), iter);
  return iter.output();
}

Tensor& hardswish_out(const Tensor& self, Tensor& result) {
  auto iter = TensorIterator::unary_op(result, self);
  hardswish_stub(iter.device_type(), iter);
  return result;
}

Tensor& hardswish_(Tensor& self) {
  #if defined(C10_MOBILE) && defined(USE_XNNPACK)
  if (xnnpack::use_hardswish(self)) {
    xnnpack::hardswish_(self);
    return self;
  }
  #endif
  auto iter = TensorIterator::unary_op(self, self);
  hardswish_stub(iter.device_type(), iter);
  return self;
}

Tensor hardswish_backward(const Tensor& grad_output, const Tensor& self) {
  Tensor grad_input;
  auto iter = TensorIterator::borrowing_binary_op(grad_input, grad_output, self);
  hardswish_backward_stub(iter.device_type(), iter);
  return iter.output();
}

Tensor relu(const Tensor & self) {
  TORCH_CHECK(self.scalar_type() != at::kBool, "Boolean inputs not supported for relu");
  return at::clamp_min(self, 0);
}

Tensor & relu_(Tensor & self) {
  TORCH_CHECK(self.scalar_type() != at::kBool, "Boolean inputs not supported for relu");
  return at::clamp_min_(self, 0);
}

Tensor selu(const Tensor & self) {
  return at::elu(self, SELU_ALPHA, SELU_SCALE);
}

Tensor relu6(const Tensor & self) {
  return at::hardtanh(self, /*min_val=*/0, /*max_val=*/6);
}

Tensor & selu_(Tensor & self) {
  return at::elu_(self, SELU_ALPHA, SELU_SCALE);
}

Tensor & relu6_(Tensor & self) {
  return at::hardtanh_(self, /*min_val=*/0, /*max_val=*/6);
}

Tensor celu(const Tensor & self, const Scalar& alpha) {
  TORCH_CHECK(alpha.to<double>() != 0,
      "ZeroDivisionError: alpha cannot be 0 for CELU");
  double inv_alpha = 1. / alpha.to<double>();
  return at::elu(self, alpha, Scalar(1.0), Scalar(inv_alpha));
}

Tensor & celu_(Tensor & self, const Scalar& alpha) {
  TORCH_CHECK(alpha.to<double>() != 0,
      "ZeroDivisionError: alpha cannot be 0 for CELU");
  double inv_alpha = 1. / alpha.to<double>();
  return at::elu_(self, alpha, Scalar(1.0), Scalar(inv_alpha));
}

Tensor math_silu_backward(
    const Tensor& grad_output,
    const Tensor& input) {
  auto input_sigmoid = at::sigmoid(input);
  return grad_output * (input_sigmoid * (1 + input * (1 - input_sigmoid)));
}

Tensor mish_backward(
    const Tensor& grad_output,
    const Tensor& input) {
  Tensor grad_input = at::empty({0}, input.options());
  auto iter = TensorIterator::binary_op(grad_input, grad_output, input);
  mish_backward_stub(iter.device_type(), iter);
  return grad_input;
}

Tensor math_mish_backward(
    const Tensor& grad_output,
    const Tensor& input) {
  auto input_tanh_softplus = at::tanh(at::softplus(input));
  auto input_sigmoid = at::sigmoid(input);
  return grad_output * (input_tanh_softplus + (input * input_sigmoid * (1 - input_tanh_softplus * input_tanh_softplus)));
}

template <typename scalar_t>
inline void _rrelu_with_noise_train(
    Tensor& output,
    const Tensor& input,
    const Tensor& noise,
    const Scalar& lower_,
    const Scalar& upper_,
    c10::optional<Generator> generator) {
  scalar_t lower = lower_.to<scalar_t>();
  scalar_t upper = upper_.to<scalar_t>();
  Tensor tmp_tensor = output.contiguous();
  scalar_t* output_data = tmp_tensor.data_ptr<scalar_t>();
  scalar_t* input_data = input.data_ptr<scalar_t>();
  scalar_t* noise_data = noise.data_ptr<scalar_t>();
  auto gen  = at::get_generator_or_default<CPUGeneratorImpl>(generator, detail::getDefaultCPUGenerator());
  std::lock_guard<std::mutex> lock(gen->mutex_);
  for (const auto i : c10::irange(input.numel())) {
    if (input_data[i] <= 0) {
      at::uniform_real_distribution<double> uniform(lower, upper);
      const scalar_t r = (scalar_t)uniform(gen);
      output_data[i] = input_data[i] * r;
      noise_data[i] = r;
    } else {
      noise_data[i] = 1;
      output_data[i] = input_data[i];
    }
  }
  if (!output.is_contiguous()) {
    output.copy_(tmp_tensor);
  }
}

Tensor& rrelu_with_noise_out_cpu(const Tensor& self,
    const Tensor& noise,
    const Scalar& lower,
    const Scalar& upper,
    bool training,
    c10::optional<Generator> generator,
    Tensor& output) {
  if (training) {
    AT_DISPATCH_FLOATING_TYPES_AND(ScalarType::BFloat16, self.scalar_type(), "rrelu_with_noise_out_cpu", [&] {
      _rrelu_with_noise_train<scalar_t>(output, self.contiguous(), noise, lower, upper, generator);
    });
    return output;
  } else {
    auto lower_tensor = scalar_to_tensor(lower);
    auto upper_tensor = scalar_to_tensor(upper);
    auto negative = (lower_tensor + upper_tensor) / 2;
    Scalar negative_slope = negative.item();
    return at::leaky_relu_out(output, self, negative_slope);
  }
}

Tensor rrelu_with_noise_cpu(
    const Tensor& self,
    const Tensor& noise,
    const Scalar& lower,
    const Scalar& upper,
    bool training,
    c10::optional<Generator> generator) {
  auto output = at::empty_like(self, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
  return at::native::rrelu_with_noise_out_cpu(
      self, noise, lower, upper, training, generator, output);
}

Tensor& rrelu_with_noise_cpu_(
    Tensor& self,
    const Tensor& noise,
    const Scalar& lower,
    const Scalar& upper,
    bool training,
    c10::optional<Generator> generator) {
  return at::native::rrelu_with_noise_out_cpu(
      self, noise, lower, upper, training, generator, self);
}

Tensor rrelu_with_noise_backward(
    const Tensor& grad_output,
    const Tensor& self_or_result,
    const Tensor& noise,
    const Scalar& lower,
    const Scalar& upper,
    bool training,
    bool is_result) {
  if (training) {
    return noise * grad_output;
  } else {
    auto l = lower.toDouble();
    auto u = upper.toDouble();
    auto mid = (l + u) / 2.;
    return at::leaky_relu_backward(grad_output, self_or_result, mid, is_result);
  }
}

Tensor rrelu(const Tensor & self, const Scalar& lower, const Scalar& upper, bool training, c10::optional<Generator> generator) {
  return at::rrelu_with_noise(self, at::empty_like(self, LEGACY_CONTIGUOUS_MEMORY_FORMAT), lower, upper, training, generator);
}

Tensor & rrelu_(Tensor & self, const Scalar& lower, const Scalar& upper, bool training, c10::optional<Generator> generator) {
  return at::rrelu_with_noise_(self, at::empty_like(self, LEGACY_CONTIGUOUS_MEMORY_FORMAT), lower, upper, training, generator);
}

TORCH_IMPL_FUNC(threshold_out)(const Tensor& self, const Scalar& threshold, const Scalar& value, const Tensor& result) {
  threshold_stub(device_type(), *this, threshold, value);
}

TORCH_IMPL_FUNC(threshold_backward_out)(const Tensor& grad, const Tensor& self, const Scalar& threshold, const Tensor& gradInput) {
  threshold_stub(device_type(), *this, threshold, 0);
}

Tensor prelu_cpu(const Tensor& self, const Tensor& weight_) {
  int64_t weight_num = weight_.numel();
  Tensor result = at::empty_like(self, self.suggest_memory_format());

  if (weight_num != 1) {
    int64_t input_ndim = self.dim();
    TORCH_CHECK(input_ndim > 0, "Not allow zero-dim input tensor.");

    int64_t channel_size = 1; // channel_size default to 1
    if (input_ndim > 1) {
      channel_size = self.size(1); // channel is the 2nd dim of input
    }
    TORCH_CHECK(channel_size == weight_num,
      "Mismatch of parameter numbers and input channel size. Found parameter numbers = ", weight_num,
      " and channel size = ", channel_size, ".");
  }

  const int64_t ndim = self.dim();
  // Helper to convert 1d tensors or scalar tensor to an nd tensor that broadcasts with input
  // All elements go into the channel dimension
  DimVector sizes(ndim, 1), strides(ndim, 0);
  auto as_nd = [&](const Tensor& t) {
    TORCH_INTERNAL_ASSERT(t.defined() && (t.dim() == 1 || t.dim() == 0));
    if (ndim >= 2) {
      sizes[1] = t.dim() == 1 ? t.sizes()[0] : 1;
      strides[1] = t.dim() == 1 ? t.strides()[0] : 0;
      return t.as_strided(sizes, strides);
    }
    return t.as_strided(sizes, strides);
  };
  Tensor w;
  if (self.scalar_type() == ScalarType::BFloat16) {
    auto w_bf16 = at::empty(weight_.sizes(), weight_.options().dtype(ScalarType::BFloat16));
    w_bf16.copy_(weight_);
    w = weight_.defined() ? as_nd(w_bf16) :
        at::detail::scalar_tensor_static(1, self.scalar_type(), kCPU);
  } else {
    w = weight_.defined() ? as_nd(weight_) :
        at::detail::scalar_tensor_static(1, self.scalar_type(), kCPU);
  }

  auto iter = TensorIteratorConfig()
    .add_output(result)
    .add_input(self)
    .add_input(w)
    .build();
  prelu_cpu_stub(iter.device_type(), iter);
  return result;
}

std::tuple<Tensor, Tensor> prelu_backward_cpu(const Tensor& grad_out_, const Tensor& self, const Tensor& weight_) {
  int64_t weight_num = weight_.numel();

  Tensor input_grad = at::empty_like(self, self.suggest_memory_format());
  Tensor weight_grad = at::empty_like(weight_, at::MemoryFormat::Contiguous);
  Tensor weight_grad_collector = at::empty_like(self, at::MemoryFormat::Contiguous);

  if (weight_num != 1) {
    int64_t input_ndim = self.dim();
    TORCH_CHECK(input_ndim > 0, "Not allow zero-dim input tensor.");

    int64_t channel_size = 1; // channel_size default to 1
    if (input_ndim > 1) {
      channel_size = self.size(1); // channel is the 2nd dim of input
    }
    TORCH_CHECK(channel_size == weight_num,
      "Mismatch of parameter numbers and input channel size. Found parameter numbers = ", weight_num,
      " and channel size = ", channel_size, ".");
  }

  const int64_t ndim = self.dim();
  // Helper to convert 1d tensor or scalar tensor to an nd tensor that broadcasts with input
  // All elements go into the channel dimension
  DimVector sizes(ndim, 1), strides(ndim, 0);
  auto as_nd = [&](const Tensor& t) {
    TORCH_INTERNAL_ASSERT(t.defined() && (t.dim() == 1 || t.dim() == 0));
    if (ndim >= 2) {
      sizes[1] = t.dim() == 1 ? t.sizes()[0] : 1;
      strides[1] = t.dim() == 1 ? t.strides()[0] : 0;
      return t.as_strided(sizes, strides);
    }
    return t.as_strided(sizes, strides);
  };
  Tensor w;
  if (self.scalar_type() == ScalarType::BFloat16) {
    auto w_bf16 = at::empty(weight_.sizes(), weight_.options().dtype(ScalarType::BFloat16));
    w_bf16.copy_(weight_);
    w = weight_.defined() ? as_nd(w_bf16) :
        at::detail::scalar_tensor_static(1, self.scalar_type(), kCPU);
  } else {
    w = weight_.defined() ? as_nd(weight_) :
        at::detail::scalar_tensor_static(1, self.scalar_type(), kCPU);
  }

  auto iter = TensorIteratorConfig()
    .add_output(input_grad)
    .add_output(weight_grad_collector)
    .add_input(self)
    .add_input(grad_out_)
    .add_input(w)
    .build();

  prelu_backward_cpu_stub(iter.device_type(), iter);

  if (weight_num == 1) {
    weight_grad.fill_(weight_grad_collector.sum());
  } else {
    // update weight_grad
    std::vector<int64_t> reduce_dims;
    int64_t input_ndim = self.dim();
    reduce_dims.push_back(0);
    if (input_ndim > 2) {
      for(int64_t i = 2; i < input_ndim; i++) reduce_dims.push_back(i);
    }
    weight_grad = weight_grad_collector.sum(reduce_dims);
  }
  return std::tuple<Tensor, Tensor>{input_grad, weight_grad};
}

Tensor infinitely_differentiable_gelu_backward(
    const Tensor& grad,
    const Tensor& self) {
  constexpr double kAlpha = M_2_SQRTPI * M_SQRT1_2 * 0.5;
  Tensor cdf = (1.0 + (self * M_SQRT1_2).erf_()).mul_(0.5);
  Tensor pdf = (-0.5 * self * self).exp_();
  return cdf.addcmul_(self, pdf, kAlpha).mul_(grad);
}

std::tuple<Tensor, Tensor> log_sigmoid_forward_cpu(const Tensor& input) {
  // FIXME: do these actually need to be zeros_like or can they be empty_like?
  auto result = at::zeros_like(input, at::MemoryFormat::Contiguous);
  auto buffer = at::zeros_like(input, at::MemoryFormat::Contiguous);
  log_sigmoid_cpu_stub(kCPU, result, buffer, input.contiguous());
  return std::make_tuple(result, buffer);
}

std::tuple<Tensor&, Tensor&> log_sigmoid_forward_out_cpu(const Tensor& input, Tensor& result, Tensor& buffer) {
  result.resize_as_(input);
  buffer.resize_as_(input, at::MemoryFormat::Contiguous);
  TORCH_CHECK(buffer.is_contiguous(), "Contiguous buffer required for log_sigmoid with out parameter");
  Tensor result_tmp = result.is_contiguous() ? result : at::empty_like(result, at::MemoryFormat::Contiguous);
  log_sigmoid_cpu_stub(kCPU, result_tmp, buffer, input.contiguous());
  if (!result.is_contiguous()) {
    result.copy_(result_tmp);
  }
  return std::forward_as_tuple(result, buffer);
}

Tensor & log_sigmoid_out(const Tensor & self, Tensor & output) {
  Tensor buffer = at::empty({0}, self.options());
  return std::get<0>(at::log_sigmoid_forward_out(output, buffer, self));
}

Tensor log_sigmoid(const Tensor & self) {
  return std::get<0>(at::log_sigmoid_forward(self));
}

Tensor log_sigmoid_backward_cuda(const Tensor& grad_output, const Tensor& input, const Tensor& buffer) {
  auto grad_input = at::empty_like(grad_output);
  // NOTE: buffer is only used by CPU dispatch, we just ignore it here
  auto iter = at::TensorIteratorConfig()
      .add_output(grad_input)
      .add_input(input)
      .add_input(grad_output)
      .build();
  log_sigmoid_backward_stub(kCUDA, iter);
  return iter.output();
}

Tensor log_sigmoid_backward_cpu(const Tensor& grad_output, const Tensor& input, const Tensor& buffer) {
  auto grad_input = at::empty_like(grad_output);
  auto iter = at::TensorIteratorConfig()
      .add_output(grad_input)
      .add_input(input)
      .add_input(buffer)
      .add_input(grad_output)
      .build();
  log_sigmoid_backward_stub(kCPU, iter);
  return iter.output();
}

Tensor& log_sigmoid_backward_cuda_out(const Tensor& grad_output, const Tensor& input,
                                      const Tensor& buffer, Tensor& grad_input) {
  auto iter = TensorIteratorConfig()
      .add_output(grad_input)
      .add_input(input)
      .add_input(grad_output)
      .build();
  log_sigmoid_backward_stub(kCUDA, iter);
  return grad_input;
}

Tensor& log_sigmoid_backward_cpu_out(const Tensor& grad_output,
    const Tensor& input,
    const Tensor& buffer,
    Tensor& grad_input) {
  auto iter = TensorIteratorConfig()
      .add_output(grad_input)
      .add_input(input)
      .add_input(buffer)
      .add_input(grad_output)
      .build();
  log_sigmoid_backward_stub(kCPU, iter);
  return grad_input;
}

DEFINE_DISPATCH(GeluKernel);
DEFINE_DISPATCH(GeluBackwardKernel);

}}  // namespace at::native
