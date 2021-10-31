#include <torch/csrc/jit/codegen/cuda/shape_inference.h>

#include <c10/core/ScalarType.h>
#include <torch/csrc/jit/codegen/cuda/instrumentation.h>
#include <torch/csrc/jit/ir/constants.h>
#include <torch/csrc/jit/runtime/operator.h>

#include <ATen/ExpandUtils.h>
#include <ATen/core/jit_type.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

namespace {

bool hasTypeAndDevice(const TensorTypePtr& op) {
  return op->device().has_value() && op->scalarType().has_value();
}

/* NaiveTypePropagator
 *   Populate type/device tag on tensor, this is a transition module to
 *   cover the absence of type inference in codegen cuda fuser.
 *
 * We only cover operations supported in codegen. We focus on propagate concrete
 * types.
 * It does NOT handle aliases (not supported in codegen anyway); Type promotion
 * is not guaranteed to be consistent with PyTorch (we need to serve the need of
 * codegen instead).
 */
class NaiveTypePropagator {
 public:
  NaiveTypePropagator(std::shared_ptr<Graph> graph)
      : graph_(std::move(graph)) {}

  void PropagateOnBlock(Block* block) {
    for (Node* node : block->nodes()) {
      PropagateOnNode(node);
    }
  }

  void PropagateOnNode(Node* node) {
    switch (node->kind()) {
      // Constant:
      case prim::Constant: {
        if (node->output()->type()->isSubtypeOf(*TensorType::get())) {
          node->output()->inferTypeFrom(node->t(attr::value));
        }
        break;
      }
      // unary operations that forward meta info:
      case aten::neg:
      case aten::bitwise_not:
      case aten::abs:
      case aten::log:
      case aten::log10:
      case aten::log1p:
      case aten::log2:
      case aten::lgamma:
      case aten::exp:
      case aten::expm1:
      case aten::erf:
      case aten::erfc:
      case aten::cos:
      case aten::acos:
      case aten::cosh:
      case aten::sin:
      case aten::asin:
      case aten::sinh:
      case aten::tan:
      case aten::atan:
      case aten::sqrt:
      case aten::rsqrt:
      case aten::ceil:
      case aten::floor:
      case aten::round:
      case aten::trunc:
      case aten::frac:
      case aten::reciprocal:
      case aten::relu:
      case aten::sigmoid:
      case aten::threshold:
      case aten::softplus:
      case aten::clamp:
      case aten::gelu:
      case aten::gelu_backward:
      case aten::silu:
      case aten::tanh: {
        TORCH_CHECK(
            hasTypeAndDevice(node->input(0)->type()->cast<TensorType>()),
            "Type and device propagation has failed, or was not provided enough information.");
        node->output()->setType(node->input(0)->type()->cast<TensorType>());
        break;
      }
      // TODO: rand_like should support cast.
      case aten::rand_like: {
        TORCH_CHECK(
            hasTypeAndDevice(node->input(0)->type()->cast<TensorType>()),
            "Type and device propagation has failed, or was not provided enough information.");
        node->output()->setType(node->input(0)->type()->cast<TensorType>());
        break;
      }
      // binary operations that forward meta info and broadcast shape:
      case aten::mul:
      case aten::div:
      case aten::atan2:
      // TODO: double check type casting logic for min/max/pow
      case aten::min:
      case aten::max:
      case aten::pow:
      case aten::remainder:
      case aten::fmod:
      case aten::lerp:
      // add/sub could be ternary op and the third argument does not contribute
      // to neither type promoteion nor shape.
      case aten::add:
      case aten::sub: {
        const auto promoted_type = binary_broadcast_type(
            node->input(0)->type()->cast<TensorType>(),
            node->input(1)->type()->cast<TensorType>());
        node->output()->setType(promoted_type);
        break;
      }
      // Type can be int or bool for "and" and "or", if both are bool should be
      // bool, if both int should be int, otherwise would have errored
      case aten::__and__:
      case aten::__or__: {
        const auto promoted_type = binary_broadcast_type(
            node->input(0)->type()->cast<TensorType>(),
            node->input(1)->type()->cast<TensorType>(),
            node->input(0)->type()->cast<TensorType>()->scalarType() ==
                    at::ScalarType::Bool
                ? at::ScalarType::Bool
                : at::ScalarType::Int);
        break;
      }
      // Real int ops
      case aten::__xor__:
      case aten::__lshift__:
      case aten::__rshift__: {
        const auto promoted_type = binary_broadcast_type(
            node->input(0)->type()->cast<TensorType>(),
            node->input(1)->type()->cast<TensorType>(),
            at::ScalarType::Int);
        node->output()->setType(promoted_type);
        break;
      }
      case aten::lt:
      case aten::le:
      case aten::gt:
      case aten::ge:
      case aten::ne:
      case aten::eq: {
        const auto promoted_type = binary_broadcast_type(
            node->input(0)->type()->cast<TensorType>(),
            node->input(1)->type()->cast<TensorType>(),
            at::ScalarType::Bool);
        node->output()->setType(promoted_type);
        break;
      }
      case aten::where: {
        const auto promoted_type = binary_broadcast_type(
            node->input(1)->type()->cast<TensorType>(),
            node->input(2)->type()->cast<TensorType>());
        node->output()->setType(promoted_type);
        break;
      }
      case aten::addcmul: {
        auto promoted_type = binary_broadcast_type(
            node->input(1)->type()->cast<TensorType>(),
            node->input(2)->type()->cast<TensorType>());
        promoted_type = binary_broadcast_type(
            promoted_type, node->input(0)->type()->cast<TensorType>());
        node->output()->setType(promoted_type);
        break;
      }
      case aten::dropout: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        node->output()->setType(out_type);
        break;
      }
      case aten::instance_norm:
      case aten::batch_norm: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        node->output()->setType(out_type);
        break;
      }
      case aten::_batch_norm_impl_index_backward: {
        auto grad_input_type = node->input(1)->type()->cast<TensorType>();
        TORCH_CHECK(
            hasTypeAndDevice(grad_input_type),
            "Type and device propagation has failed, or was not provided enough information.");
        node->output(0)->setType(grad_input_type);

        // TODO: double check with type promotion
        auto mean_rstd_type = TensorType::create(
            *grad_input_type->scalarType(),
            *grad_input_type->device(),
            c10::nullopt,
            c10::nullopt);

        node->output(1)->setType(mean_rstd_type);
        node->output(2)->setType(mean_rstd_type);

        break;
      }
      case aten::_batch_norm_impl_index: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        TORCH_CHECK(
            hasTypeAndDevice(out_type),
            "Type and device propagation has failed, or was not provided enough information.");
        node->output(0)->setType(out_type);

        auto mean_rstd_type = TensorType::create(
            *out_type->scalarType(),
            *out_type->device(),
            c10::nullopt,
            c10::nullopt);

        node->output(1)->setType(mean_rstd_type);
        node->output(2)->setType(mean_rstd_type);
        // TODO: not that it matters, but mark the right type here;
        // node->output(3)->setType(out_type->withScalarType());
        node->output(3)->setType(out_type);
        node->output(4)->setType(IntType::get());

        break;
      }
      case aten::native_batch_norm: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        TORCH_CHECK(
            hasTypeAndDevice(out_type),
            "Type and device propagation has failed, or was not provided enough information.");
        node->output(0)->setType(out_type);

        auto mean_rstd_type = TensorType::create(
            *out_type->scalarType(),
            *out_type->device(),
            c10::nullopt,
            c10::nullopt);

        node->output(1)->setType(mean_rstd_type);
        node->output(2)->setType(mean_rstd_type);

        break;
      }
      case aten::native_batch_norm_backward: {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
        auto out_mask_list = constant_as<c10::List<bool>>(node->input(9));
        TORCH_INTERNAL_ASSERT(
            out_mask_list.has_value(), "output mask for batch_norm_backward");
        std::vector<int> output_mask;
        for (const auto value : out_mask_list->vec()) {
          output_mask.emplace_back(static_cast<int>(value));
        }

        if (output_mask[0]) {
          auto in_type = node->input(1)->type()->cast<TensorType>();
          node->output(0)->setType(in_type);
        }

        if (output_mask[1]) {
          auto weight_type = node->input(2)->type()->cast<TensorType>();
          node->output(1)->setType(weight_type);
        }

        if (output_mask[2]) {
          auto weight_type = node->input(2)->type()->cast<TensorType>();
          auto bias_type = TensorType::create(
              *weight_type->scalarType(),
              *weight_type->device(),
              *weight_type->dim(),
              output_mask[2]);
          node->output(2)->setType(bias_type);
        }
        break;
      }
      case aten::layer_norm: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        node->output()->setType(out_type);
        break;
      }
      case aten::native_layer_norm: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        TORCH_CHECK(
            hasTypeAndDevice(out_type),
            "Type and device propagation has failed, or was not provided enough information.");
        node->output(0)->setType(out_type);

        auto mean_rstd_type = TensorType::create(
            *out_type->scalarType(), *out_type->device(), c10::nullopt, false);

        node->output(1)->setType(mean_rstd_type);
        node->output(2)->setType(mean_rstd_type);

        break;
      }
      case aten::native_layer_norm_backward: {
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
        auto out_mask_list = constant_as<c10::List<bool>>(node->input(7));
        TORCH_INTERNAL_ASSERT(
            out_mask_list.has_value(), "output mask for layer_norm_backward");
        std::vector<int> output_mask;
        for (const auto value : out_mask_list->vec()) {
          output_mask.emplace_back(static_cast<int>(value));
        }

        if (output_mask[0]) {
          auto out_type = node->input(0)->type()->cast<TensorType>();
          node->output(0)->setType(out_type);
        }

        if (output_mask[1] &&
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
            !node->input(5)->type()->isSubtypeOf(
                static_cast<c10::TypePtr>(NoneType::get()))) {
          // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
          auto weight_type = node->input(5)->type()->cast<TensorType>();
          node->output(1)->setType(weight_type);
        }

        if (output_mask[2] &&
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
            !node->input(6)->type()->isSubtypeOf(
                static_cast<c10::TypePtr>(NoneType::get()))) {
          // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
          auto bias_type = node->input(6)->type()->cast<TensorType>();
          node->output(2)->setType(bias_type);
        }
        break;
      }
      case aten::softmax: {
        auto out_type = node->input(0)->type()->cast<TensorType>();

        // accept dtype input to `aten::softmax` node
        if (!node->input(2)->type()->isSubtypeOf(
                static_cast<c10::TypePtr>(NoneType::get()))) {
          if (auto opt_ivalue = toIValue(node->input(2))) {
            out_type = out_type->withScalarType(opt_ivalue->toScalarType());
          }
        }
        node->output()->setType(out_type);
        break;
      }
      case aten::_softmax_backward_data: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        node->output()->setType(out_type);
        break;
      }
      case aten::mean:
      case aten::sum: {
        auto out_type = node->input(0)->type()->cast<TensorType>();

        // accept dtype input to `aten::sum` node
        if (!node->input(3)->type()->isSubtypeOf(
                static_cast<c10::TypePtr>(NoneType::get()))) {
          if (auto opt_ivalue = toIValue(node->input(3))) {
            out_type = out_type->withScalarType(opt_ivalue->toScalarType());
          }
        }
        const auto dims = constant_as<c10::List<int64_t>>(node->input(1));
        const auto keepdim = constant_as<bool>(node->input(2));
        TORCH_CHECK(
            dims.has_value() && keepdim.has_value(),
            "Shape inference cannot handle options.");
        node->output()->setType(
            unary_reduce_type(out_type, dims->vec(), keepdim.value()));
        break;
      }
      case aten::sum_to_size:
      case aten::_grad_sum_to_size: {
        auto out_type = node->input(0)->type()->cast<TensorType>();
        node->output()->setType(out_type->withDim(c10::nullopt));
        break;
      }
      case aten::type_as: {
        const auto type0 = node->input(0)->type()->cast<TensorType>();
        const auto type1 = node->input(1)->type()->cast<TensorType>();
        TORCH_CHECK(
            type0 != nullptr && type1 != nullptr &&
                type1->scalarType().has_value(),
            "input to type_as needs to be a tensor");
        node->output()->setType(type0->withScalarType(type1->scalarType()));
        break;
      }
      case aten::to: {
        const auto type0 = node->input(0)->type()->cast<TensorType>();
        const auto out_dtype = toIValue(node->input(1));
        TORCH_CHECK(out_dtype, "No output type specified");
        node->output()->setType(
            type0->withScalarType(out_dtype->toScalarType()));
        break;
      }
      case prim::add_optional: {
        const auto type0 = node->input(0)->type()->cast<TensorType>();
        const auto type1 = node->input(1)->type()->cast<TensorType>();
        TORCH_CHECK(type0 != nullptr);
        if (type1 != nullptr) {
          node->output()->setType(type0);
        } else {
          const auto promoted_type = binary_broadcast_type(type0, type1);
          node->output()->setType(promoted_type);
        }
        break;
      }
      default:
        TORCH_CHECK(
            false,
            "type inference failed, unrecognized operation encountered:",
            node->kind().toDisplayString());
        // TODO: generate a proper error log, as this probably means something
        //       went unexpected.
        break;
    }
  }

  void run() {
    PropagateOnBlock(graph_->block());
  }

 protected:
  TensorTypePtr unary_reduce_type(
      const TensorTypePtr& op,
      const std::vector<int64_t>& dims,
      bool keepdim) {
    TORCH_CHECK(
        hasTypeAndDevice(op),
        "Type and device propagation has failed, or was not provided enough information.");
    return TensorType::create(
        *op->scalarType(), *op->device(), c10::nullopt, c10::nullopt);
  }

  // TODO: we should comply to codegen type promotion.
  TensorTypePtr binary_broadcast_type(
      TensorTypePtr const& op0,
      TensorTypePtr const& op1,
      c10::optional<at::ScalarType> scalar_type = c10::nullopt) {
    TORCH_CHECK(
        op0 != nullptr || op1 != nullptr,
        "Scalar operations on binary broadcast type, not supported yet.");

    if (op0 != nullptr && op1 != nullptr) {
      TORCH_CHECK(
          hasTypeAndDevice(op0) && hasTypeAndDevice(op1),
          "Type and device propagation has failed, or was not provided enough information.");
      auto promoted_scalar_type = scalar_type.has_value()
          ? *scalar_type
          : c10::promoteTypes(*op0->scalarType(), *op1->scalarType());

      return TensorType::create(
          promoted_scalar_type, *op0->device(), c10::nullopt, c10::nullopt);
    } else {
      auto ptr = (op0 != nullptr) ? op0 : op1;
      TORCH_CHECK(
          hasTypeAndDevice(ptr),
          "Type and device propagation has failed, or was not provided enough information.");
      return TensorType::create(
          scalar_type.has_value() ? *scalar_type : *ptr->scalarType(),
          *ptr->device(),
          c10::nullopt,
          c10::nullopt);
    }
  }

 private:
  std::shared_ptr<Graph> graph_;
};

} // namespace

void TypePropagate(std::shared_ptr<Graph>& graph) {
  FUSER_PERF_SCOPE("TypePropagate");
  NaiveTypePropagator(graph).run();
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
