#include <type_traits>

#include <ATen/ATen.h>
#include <ATen/AccumulateType.h>
#include <ATen/Dispatch.h>
#include <ATen/NestedTensorImpl.h>
#include <ATen/Parallel.h>
#include <ATen/TensorIndexing.h>
#include <ATen/cpu/vec/vec256/vec256.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/cat.h>
#endif

#include <ATen/native/nested/NestedTensorTransformerFunctions.h>

namespace at {

namespace native {

namespace {

Tensor gemm_nt(const Tensor& self, const Tensor& other) {
  if (self.is_nested()) {
    return NestedTensor_matmul(self, other.t());
  } else {
    return at::native::matmul(self, other.t());
  }
}

template <typename scalar_t>
void transform_bias_rescale_qkv_inner_loop(
    int64_t B,
    int64_t T,
    int64_t _3D,
    int64_t D,
    int64_t num_head,
    int64_t dim_per_head,
    scalar_t* qkv_data,
    scalar_t* qkv_bias_data,
    scalar_t* q_k_v_data,
    scalar_t inv_sqrt_dim_per_head,
    int64_t begin,
    int64_t end) {
  for (auto i : c10::irange(begin, end)) {
    auto t = i % T;
    i /= T;
    auto nh = i % num_head;
    i /= num_head;
    auto b = i;
    using Vec = vec::Vectorized<scalar_t>;
    auto V = vec::Vectorized<scalar_t>::size();
    auto dh = 0;
    auto d = nh * dim_per_head;
    for (; dh + V <= dim_per_head; dh += V, d += V) {
      // load
      auto q_bias_data = Vec::loadu(&qkv_bias_data[d + 0 * D]);
      auto k_bias_data = Vec::loadu(&qkv_bias_data[d + 1 * D]);
      auto v_bias_data = Vec::loadu(&qkv_bias_data[d + 2 * D]);

      auto q_data = Vec::loadu(&qkv_data[b * _3D * T + t * _3D + d + 0 * D]) +
          q_bias_data;
      auto k_data = Vec::loadu(&qkv_data[b * _3D * T + t * _3D + d + 1 * D]) +
          k_bias_data;
      auto v_data = Vec::loadu(&qkv_data[b * _3D * T + t * _3D + d + 2 * D]) +
          v_bias_data;

      q_data = q_data * Vec(inv_sqrt_dim_per_head);

      q_data.store(&q_k_v_data
                       [0 * B * num_head * T * dim_per_head +
                        b * num_head * T * dim_per_head +
                        nh * T * dim_per_head + t * dim_per_head + dh]);
      k_data.store(&q_k_v_data
                       [1 * B * num_head * T * dim_per_head +
                        b * num_head * T * dim_per_head +
                        nh * T * dim_per_head + t * dim_per_head + dh]);
      v_data.store(&q_k_v_data
                       [2 * B * num_head * T * dim_per_head +
                        b * num_head * T * dim_per_head +
                        nh * T * dim_per_head + t * dim_per_head + dh]);
    }
    for (; dh < dim_per_head; dh++) {
      auto d = nh * dim_per_head + dh;
      auto q_bias = qkv_bias_data[d + 0 * D];
      auto k_bias = qkv_bias_data[d + 1 * D];
      auto v_bias = qkv_bias_data[d + 2 * D];
      auto q_data = qkv_data[b * _3D * T + t * _3D + d + 0 * D] + q_bias;
      auto k_data = qkv_data[b * _3D * T + t * _3D + d + 1 * D] + k_bias;
      auto v_data = qkv_data[b * _3D * T + t * _3D + d + 2 * D] + v_bias;
      q_data = q_data * inv_sqrt_dim_per_head;
      q_k_v_data
          [0 * B * num_head * T * dim_per_head +
           b * num_head * T * dim_per_head + nh * T * dim_per_head +
           t * dim_per_head + dh] = q_data;
      q_k_v_data
          [1 * B * num_head * T * dim_per_head +
           b * num_head * T * dim_per_head + nh * T * dim_per_head +
           t * dim_per_head + dh] = k_data;
      q_k_v_data
          [2 * B * num_head * T * dim_per_head +
           b * num_head * T * dim_per_head + nh * T * dim_per_head +
           t * dim_per_head + dh] = v_data;
    }
  }
}

Tensor bmm_nt(const Tensor& a, const Tensor& b) {
  auto a_ = a.view({a.size(0) * a.size(1), a.size(2), a.size(3)});
  auto b_ = b.view({b.size(0) * b.size(1), b.size(2), b.size(3)});
  auto bt_ = b_.transpose(2, 1);
  auto c_ = at::bmm(a_, bt_);
  return c_.view({a.size(0), a.size(1), a.size(2), b.size(2)});
}

Tensor masked_softmax(
    Tensor& attn_scores,
    c10::optional<Tensor> attn_mask,
    const Tensor& query) {
  if (query.is_nested() && !attn_mask) {
    // TODO: maybe we could do better than generating a mask every time?

    attn_mask = NestedTensor_to_mask(query, 2);
    // TODO: CPU path does not support transformer mask yet.
    if (attn_scores.is_cpu()) {
      attn_mask = attn_mask->view({-1, 1, 1, attn_scores.sizes()[3]});
      // 1 means skip, 0 means keep.
      // want:
      // 0,0 -> 0
      // 0,1 -> 1
      // 1,1 -> 1
      // so that's logical OR.
      *attn_mask = *attn_mask | attn_mask->transpose(2, 3);
      attn_mask = at::expand_inplace(attn_scores, *attn_mask)->contiguous();
    }
    attn_mask = attn_mask->to(query.device(), /*non-blocking=*/true);
  }
  if (attn_mask && attn_mask->dtype() != at::kBool) {
    TORCH_WARN(
        "Converting mask without torch.bool dtype to bool; this will "
        "negatively affect performance. Prefer to use a boolean mask directly.");
    attn_mask = attn_mask->to(at::kBool);
  }
  if (attn_scores.is_cpu() && attn_mask && attn_mask->dim() == 2) {
    // TODO: CPU path does not support transformer mask yet.
    const auto batch_size = attn_scores.sizes()[0];
    const auto seq_len = attn_scores.sizes()[3];
    TORCH_CHECK(attn_mask->sizes()[0] == batch_size);
    TORCH_CHECK(attn_mask->sizes()[1] == seq_len);
    attn_mask = attn_mask->view({batch_size, 1, 1, seq_len});
    attn_mask = at::expand_inplace(attn_scores, *attn_mask)->contiguous();
  }
  if (attn_mask) {
    return _masked_softmax(attn_scores, *attn_mask);
  } else {
    return _softmax_out(attn_scores, attn_scores, attn_scores.dim() - 1, false);
  }
}

Tensor bmm_nn(Tensor& out, const Tensor& a, const Tensor& b) {
  const std::array<int64_t, 3> newAShape = {
      a.sizes()[0] * a.sizes()[1], a.sizes()[2], a.sizes()[3]};
  auto a_ = a.view(newAShape);
  const std::array<int64_t, 3> newBShape = {
      b.sizes()[0] * b.sizes()[1], b.sizes()[2], b.sizes()[3]};
  auto b_ = b.view(newBShape);
  auto out_ = out.reshape({newAShape[0], newAShape[1], newBShape[2]});
  auto c_ = at::bmm_out(out_, a_, b_);
  return c_.view({a.size(0), a.size(1), a.size(2), b.size(3)});
}

Tensor transform_0213(const Tensor& a) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(a.size(1));
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(a.size(3));
  return a.permute({0, 2, 1, 3})
      .contiguous()
      .view({a.size(0), a.size(2), a.size(1) * a.size(3)});
}

Tensor transform0213_gemm_nt_bias(
    const Tensor& a,
    const Tensor& b,
    const Tensor& c,
    const Tensor& query) {
  if (query.is_nested()) {
    at::Tensor nested_a = _nested_from_padded(
        a, get_nested_tensor_impl(query)->get_nested_size_tensor(), true);
    return NestedTensor_times_Tensor_plus_Tensor_addmm(
        c, nested_a, b.t(), 1, 1);
  } else {
    const Tensor a_0213 = transform_0213(a);
    auto a_ = a_0213.view({a_0213.size(0) * a_0213.size(1), a_0213.size(2)});
    auto r_ = at::native::linear(a_, b, c);
    return r_.view({a_0213.size(0), a_0213.size(1), r_.size(1)});
  }
}

void debug_assert_shape(int line, const Tensor& t, c10::IntArrayRef shape) {
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
      (size_t)t.dim() == shape.size(),
      "(called from line ",
      line,
      ") ",
      "expected ",
      shape.size(),
      "-D tensor but got ",
      t.dim());
  if (t.is_nested()) {
    return;
  }
  for (auto idx : c10::irange(shape.size())) {
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
        shape[idx] == 0 || t.sizes()[idx] == shape[idx],
        "(called from line ",
        line,
        ") ",
        "expected dim ",
        idx,
        " to be ",
        shape[idx],
        " but got ",
        t.sizes()[idx]);
  }
}
} // namespace

// compute q = (q + q_bias) / sqrt(dim_per_head), k = k + k_bias, v = v + v_bias
std::tuple<Tensor, Tensor, Tensor> transform_bias_rescale_qkv_cpu(
    const Tensor& qkv,
    const Tensor& qkv_bias,
    const int64_t num_head) {
  auto qkv_ = qkv.is_nested()
    ? c10::MaybeOwned<Tensor>::owned(qkv.to_padded_tensor(0))
    : c10::MaybeOwned<Tensor>::borrowed(qkv);
  auto B = qkv_->size(0);
  auto T = qkv_->size(1);
  auto _3D = qkv_->size(2);
  auto D = _3D / 3;
  TORCH_CHECK(D % num_head == 0);
  TORCH_CHECK(_3D % 3 == 0);
  const auto dim_per_head = D / num_head;
  auto q_k_v = at::empty({3, B, num_head, T, dim_per_head}, qkv_->options());

  const auto qkv_contig = qkv_->expect_contiguous();
  const auto qkv_bias_contig = qkv_bias.expect_contiguous();
  AT_DISPATCH_FLOATING_TYPES_AND2(
      ScalarType::Half,
      ScalarType::BFloat16,
      qkv_->scalar_type(),
      "transform_bias_rescale_qkv",
      [&] {
        scalar_t* qkv_data = qkv_contig->data_ptr<scalar_t>();
        scalar_t* qkv_bias_data = qkv_bias_contig->data_ptr<scalar_t>();
        scalar_t* q_k_v_data = q_k_v.data_ptr<scalar_t>();
        const scalar_t inv_sqrt_dim_per_head =
            1.0 / std::sqrt(static_cast<scalar_t>(dim_per_head));

        int64_t grain_size =
            std::max(internal::GRAIN_SIZE / (3 * dim_per_head), (int64_t)1);
        parallel_for(
            0, B * num_head * T, grain_size, [&](int64_t begin, int64_t end) {
              transform_bias_rescale_qkv_inner_loop(
                  B,
                  T,
                  _3D,
                  D,
                  num_head,
                  dim_per_head,
                  qkv_data,
                  qkv_bias_data,
                  q_k_v_data,
                  inv_sqrt_dim_per_head,
                  begin,
                  end);
            });
      });
  auto q_k_v_s =
      at::native::split(q_k_v.view({3 * B, num_head, T, dim_per_head}), B, 0);
  TORCH_INTERNAL_ASSERT_DEBUG_ONLY(q_k_v_s.size() == 3);
  return std::make_tuple(q_k_v_s[0], q_k_v_s[1], q_k_v_s[2]);
}

std::tuple<Tensor, Tensor> native_multi_head_attention(
    const Tensor& query,
    const Tensor& key,
    const Tensor& value,
    const int64_t embed_dim,
    const int64_t num_head,
    const Tensor& qkv_weight,
    const Tensor& qkv_bias,
    const Tensor& proj_weight,
    const Tensor& proj_bias,
    const c10::optional<Tensor>& mask,
    bool need_weights,
    bool average_attn_weights) {
  // query shape: [B, T, D]
  // qkv_weight shape: [3 * D, D]

  TORCH_CHECK(
      !mask || !query.is_nested(),
      "NestedTensor with mask is not supported yet");
  const auto D = embed_dim;
  TORCH_CHECK(
      query.dim() == 3,
      "expected 3-D `query`, got ",
      query.dim(),
      "-D tensor");
  TORCH_CHECK(
      query.is_nested() || query.sizes()[2] == embed_dim,
      "passed-in embed_dim ",
      embed_dim,
      " didn't match last dim of query ",
      query.sizes()[2]);
  TORCH_CHECK(
      key.dim() == 3,
      "expected 3-D `key`, got ",
      key.dim(),
      "-D tensor");
  TORCH_CHECK(
      value.dim() == 3,
      "expected 3-D `value`, got ",
      value.dim(),
      "-D tensor");
  TORCH_CHECK(
      query.is_nested() || key.is_nested() || value.is_nested() ||
          (query.sizes() == key.sizes() && key.sizes() == value.sizes()),
      "expected `query`/`key`/`value` shapes to match");
  TORCH_CHECK(
      qkv_weight.dim() == 2,
      "expected 2-D `qkv_weight`, got ",
      qkv_weight.dim(),
      "-D tensor");
  TORCH_CHECK(
      D * 3 == qkv_weight.sizes()[0],
      "expected `qkv_weight` first dim to be 3x embed_dim");
  TORCH_CHECK(
      D == qkv_weight.sizes()[1],
      "expected `qkv_weight` second dim to be embed_Dim");
  TORCH_CHECK(
      qkv_bias.dim() == 1,
      "expected 2-D `qkv_bias`, got ",
      qkv_bias.dim(),
      "-D tensor");
  TORCH_CHECK(
      qkv_bias.sizes()[0] == 3 * D,
      "expected `qkv_bias` first dim and first dim of query to be equal");
  TORCH_CHECK(D % num_head == 0, "`embed_dim` must divide evenly by `num_heads`");

#ifndef NDEBUG
  const auto B = query.is_nested()
      ? get_nested_tensor_impl(query)->get_nested_size_tensor().size(0)
      : query.sizes()[0];
  auto T = query.is_nested() ? 0 : query.sizes()[1];
  const auto dim_per_head = D / num_head;
#endif

  // shape: [B, T, 3 x D]
  Tensor qkv;

  if (key.is_same(value)) {
    if (query.is_same(key)) {
      // self-attention
      qkv = gemm_nt(query, qkv_weight);
    } else {
      // encoder-decoder attention
      // TODO: is there a more efficient way to set this up?
      // TODO: can we stay nested insted of using cat? Probably just make a
      // NestedTensor out of the matmul results or something?
      auto q_kv_weight_s =
          at::native::split_with_sizes(qkv_weight, {D, D * 2}, 0);
      TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
          q_kv_weight_s.size() == 2,
          "expected split to produce 2 tensors but it produced ",
          q_kv_weight_s.size());
      auto q = gemm_nt(query, q_kv_weight_s[0]);
      auto kv = gemm_nt(key, q_kv_weight_s[1]);
      qkv = at::cat({q, kv}, 2);
    }
  } else {
    auto q_k_v_weight_s = at::native::chunk(qkv_weight, 3, 0);
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
        q_k_v_weight_s.size() == 3,
        "expected chunk to produce 3 tensors but it produced ",
        q_k_v_weight_s.size());
    // TODO: can we stay nested instead of using cat?
    auto q = gemm_nt(query, q_k_v_weight_s[0]);
    auto k = gemm_nt(key, q_k_v_weight_s[1]);
    auto v = gemm_nt(value, q_k_v_weight_s[2]);
    qkv = at::cat({q, k, v}, 2);
  }

  if (!qkv.is_nested() && qkv.numel() == 0) {
    if (query.is_nested()) {
      return std::make_tuple(Tensor(), Tensor());
    }
    return std::make_tuple(at::empty_like(query), Tensor());
  }

#ifndef NDEBUG
  if (!query.is_nested() || !qkv.is_nested()) {
    if (query.is_nested()) {
      T = qkv.size(1);
    }
    debug_assert_shape(__LINE__, qkv, {B, T, 3 * D});
  }
#endif

#ifdef DEBUG_PRINT_EACH_STEP
  if (!qkv.is_nested()) {
    std::cerr << "qkv: " << qkv << std::endl;
  }
#endif
  // shape: 3 x [B, num_head, T, dim_per_head]
  auto q_k_v = _transform_bias_rescale_qkv(qkv, qkv_bias, num_head);
  qkv = Tensor(); // Not used any more, allow free
  auto& q = std::get<0>(q_k_v);
  const auto& k = std::get<1>(q_k_v);
  const auto& v = std::get<2>(q_k_v);
#ifndef NDEBUG
  debug_assert_shape(__LINE__, q, {B, num_head, T, dim_per_head});
  debug_assert_shape(__LINE__, k, {B, num_head, T, dim_per_head});
  debug_assert_shape(__LINE__, v, {B, num_head, T, dim_per_head});
#endif
#ifdef DEBUG_PRINT_EACH_STEP
  std::cerr << "q: " << q << std::endl;
  std::cerr << "k: " << k << std::endl;
  std::cerr << "v: " << v << std::endl;
#endif

  // shape: [B, num_head, T, T]
  auto qkt = bmm_nt(q, k);
  // q & k are dead but cannot be freed because they were packed with v
#ifndef NDEBUG
  debug_assert_shape(__LINE__, qkt, {B, num_head, T, T});
#endif
#ifdef DEBUG_PRINT_EACH_STEP
  std::cerr << "qkt: " << qkt << std::endl;
#endif

  // shape: [B, num_head, T, T]
  // TODO: long-term, have a kernel that works with
  // NestedTensor directly if there is no mask passed
  qkt = masked_softmax(qkt, mask, query);
#ifdef DEBUG_PRINT_EACH_STEP
  std::cerr << "qkt after softmax: " << qkt << std::endl;
#endif

  // shape: [B, num_head, T, dim_per_head]
  // reuse storage for q; we're done with it
  auto attn_ctx = bmm_nn(q, qkt, v);
  // qkv is not dead; we just reused storage for q!
  if (!need_weights) {
    qkt = Tensor();
  }
#ifndef NDEBUG
  debug_assert_shape(__LINE__, attn_ctx, {B, num_head, T, dim_per_head});
#endif
#ifdef DEBUG_PRINT_EACH_STEP
  std::cerr << "attn_ctx: " << attn_ctx << std::endl;
#endif

  // shape: [B, T, D]
  // Fuse transform_0213 inside
  auto proj = transform0213_gemm_nt_bias(
      attn_ctx, proj_weight, proj_bias, query);
#ifndef NDEBUG
  debug_assert_shape(__LINE__, proj, {B, T, D});
#endif
  if (need_weights && average_attn_weights) {
    // weights are not needed for full transformer, so don't worry too
    // much about performance -- we implement this just to make use
    // cases that don't disable need_weights still get some speedup.
    qkt = qkt.sum(1);
    qkt /= num_head;
  }
  return std::make_tuple(std::move(proj), std::move(qkt));
}

} // namespace native
} // namespace at
