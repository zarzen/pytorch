#pragma once

#ifdef USE_VULKAN_API

#include <ATen/native/vulkan/ops/Common.h>
#include <ATen/native/vulkan/ops/Mm.h>
#include <torch/library.h>

namespace at {
namespace native {
namespace vulkan {
namespace ops {

class GruOpContext final : public torch::jit::CustomClassHolder {
 public:
  static GruOpContext create(
      const std::vector<Tensor>& params_cpu, // weights/biases (cpu)
      bool has_biases,
      int64_t num_layers,
      double dropout,
      bool train,
      bool bidirectional,
      bool batch_first);

  using State = std::tuple<std::vector<Tensor>, bool, int64_t, double, bool, bool, bool>;

  std::tuple<Tensor, Tensor> run(
      const Tensor& input_vk,
      const Tensor & hx_vk) const;
  State unpack() const;

 private:
  GruOpContext(
      const std::vector<Tensor>& params_cpu, // weights/biases (cpu)
      bool has_biases,
      int64_t num_layers,
      double dropout,
      bool train,
      bool bidirectional,
      bool batch_first);

 private:
  struct {
    std::vector<LinearOpContext> linear_op_contexts;  // {{ op context for b_ir, w_ir, op context for b_hr, w_hr,
                                                      //    op context for b_iz, w_iz, op context for b_hz, w_hz,
                                                      //    op context for b_in, w_in, op context for b_hn, w_hn,}, ...}
    bool has_biases{};
    int64_t num_layers{};
    double dropout{};
    bool train{};
    bool bidirectional{};
    bool batch_first{};
  } packed_;

  struct {
    std::vector<Tensor> params_cpu;      // weights/biases (cpu)
    bool has_biases{};
    int64_t num_layers{};
    double dropout{};
    bool train{};
    bool bidirectional{};
    bool batch_first{};
  } unpacked_;
};

c10::intrusive_ptr<GruOpContext> gru_prepack(
    std::vector<Tensor>&& params_cpu,   // weights/biases (cpu)
    bool has_biases,
    int64_t num_layers,
    double dropout,
    bool train,
    bool bidirectional,
    bool batch_first);

std::tuple<Tensor, Tensor> gru_run(
    const Tensor& input_vk,
    const Tensor & hx_vk,
    const c10::intrusive_ptr<GruOpContext>& context);

} // namespace ops
} // namespace vulkan
} // namespace native
} // namespace at

#endif /* USE_VULKAN_API */
