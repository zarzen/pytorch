#include <torch/csrc/jit/codegen/cuda/arith.h>
#include <torch/csrc/jit/codegen/cuda/index_compute.h>
#include <torch/csrc/jit/codegen/cuda/ir_iostream.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/lower_utils.h>
#include <torch/csrc/jit/codegen/cuda/predicate_compute.h>

#include <torch/csrc/jit/codegen/cuda/lower_index.h>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

Val* IndexLowering::lowerSrcIndex(Val* src, Val* dst) const {
  if (auto tv = dynamic_cast<TensorView*>(src)) {
    TORCH_INTERNAL_ASSERT(dst->isA<TensorView>());
    return Index::getProducerIndex(tv, dst->as<TensorView>(), for_loops_);
  } else {
    return src;
  }
}

Val* IndexLowering::lowerDstIndex(Val* dst) const {
  if (auto tv = dynamic_cast<TensorView*>(dst)) {
    return Index::getConsumerIndex(tv, for_loops_);
  } else {
    return dst;
  }
}

void IndexLowering::pushBack(Expr* expr) {
  if (active_scope_ == nullptr) {
    lowered_exprs_.push_back(expr);
  } else {
    active_scope_->push_back(expr);
  }
}

Expr* IndexLowering::back() const {
  if (active_scope_ == nullptr) {
    TORCH_INTERNAL_ASSERT(
        !lowered_exprs_.empty(), "IndexLowering::back: empty scope.");
    return lowered_exprs_.back();
  }
  TORCH_INTERNAL_ASSERT(
      !active_scope_->empty(), "IndexLowering::back: empty scope.");
  return active_scope_->exprs().back();
}

void IndexLowering::insertAtTopLevel(Expr* expr) {
  TORCH_INTERNAL_ASSERT(!lowered_exprs_.empty());
  lowered_exprs_.insert(lowered_exprs_.end() - 1, expr);
}

void IndexLowering::handle(const kir::IfThenElse* ite) {
  const auto prev_scope = active_scope_;

  auto new_ite = IrBuilder::create<kir::IfThenElse>(ite->predicate());
  pushBack(new_ite);

  active_scope_ = &new_ite->thenBody();

  for (auto expr : ite->thenBody().exprs()) {
    OptOutConstDispatch::handle(expr);
  }

  active_scope_ = &new_ite->elseBody();

  for (auto expr : ite->elseBody().exprs()) {
    OptOutConstDispatch::handle(expr);
  }

  active_scope_ = prev_scope;
}

void IndexLowering::handle(const kir::ForLoop* for_loop) {
  const auto prev_scope = active_scope_;

  auto new_for_loop = IrBuilder::create<kir::ForLoop>(for_loop);
  pushBack(new_for_loop);

  active_scope_ = &new_for_loop->body();
  for_loops_.push_back(new_for_loop);

  for (auto expr : for_loop->body().exprs()) {
    OptOutConstDispatch::handle(expr);
  }

  for_loops_.pop_back();
  active_scope_ = prev_scope;
}

void IndexLowering::handle(const UnaryOp* uop) {
  const auto in = lowerSrcIndex(uop->in(), uop->out());
  const auto out = lowerDstIndex(uop->out());
  pushBack(IrBuilder::create<UnaryOp>(uop->getUnaryOpType(), out, in));
  GpuLower::current()->propagateExprInfo(uop, back());
}

void IndexLowering::handle(const BinaryOp* bop) {
  const auto lhs = lowerSrcIndex(bop->lhs(), bop->out());
  const auto rhs = lowerSrcIndex(bop->rhs(), bop->out());
  const auto out = lowerDstIndex(bop->out());
  pushBack(IrBuilder::create<BinaryOp>(bop->getBinaryOpType(), out, lhs, rhs));
  GpuLower::current()->propagateExprInfo(bop, back());
}

void IndexLowering::handle(const TernaryOp* top) {
  const auto in1 = lowerSrcIndex(top->in1(), top->out());
  const auto in2 = lowerSrcIndex(top->in2(), top->out());
  const auto in3 = lowerSrcIndex(top->in3(), top->out());
  const auto out = lowerDstIndex(top->out());
  pushBack(IrBuilder::create<TernaryOp>(
      top->getTernaryOpType(), out, in1, in2, in3));
  GpuLower::current()->propagateExprInfo(top, back());
}

void IndexLowering::handle(const ViewAsScalar* uop) {
  const auto in = lowerSrcIndex(uop->in(), uop->out());
  const auto out = lowerDstIndex(uop->out());
  for (auto loop : for_loops_) {
    if (GpuLower::current()->caMap()->areMapped(
            loop->iter_domain(),
            uop->vector_id()->as<IterDomain>(),
            IdMappingMode::LOOP)) {
      Val* index = loop->index();
      pushBack(
          IrBuilder::create<ViewAsScalar>(out, in, uop->vector_id(), index));
      GpuLower::current()->propagateExprInfo(uop, back());
      return;
    }
  }
  TORCH_INTERNAL_ASSERT(false, "Can not find index for vector dim");
}

namespace {

// Get the size of the temporary work buffer for grid communication, this can be
// grid reduction, broadcast, or grid welford.
// expansion_factor can be optionally passed to expand the allocation
// size. For example, FusedReduction should double the work buffer size.
Val* getGridCommWorkBufferSize(
    const TensorDomain* td,
    const std::vector<kir::ForLoop*>& for_loops = {},
    int expansion_factor = 1) {
  // The buffer size is the number of thread blocks multiplied by the
  // number of threads not used for reduction domains.
  // Note: Previously it was calculated based on the shape of the
  // tensor, but it makes more sense to compute the size based on the
  // shape of the thread block and grid since this buffer is used for
  // communications among them. Both methods should result in the same
  // size if the parallel dimensions are exact, but otherwise, just
  // computing the buffer size based on the tensor shape isn't
  // sufficient since there could be extra threads/blocks.
  TORCH_INTERNAL_ASSERT(
      expansion_factor >= 1, "Invalid expansion factor: ", expansion_factor);
  Val* buffer_size = expansion_factor == 1
      ? GpuLower::current()->kernel()->oneVal()
      : IrBuilder::create<Int>(expansion_factor);
  for (auto pt : kParallelTypeThreads) {
    auto pt_dim = GpuLower::current()->parallelDimensionMap().get(pt);
    if (pt_dim == nullptr || pt_dim->isOneInt()) {
      continue;
    }
    if (isParallelTypeThreadDim(pt) &&
        std::any_of(td->domain().begin(), td->domain().end(), [&](auto out_id) {
          return out_id->getParallelType() == pt &&
              (out_id->isReduction() || out_id->isBroadcast());
        })) {
      continue;
    }
    buffer_size = SimplifyingIrBuilder::mulExpr(buffer_size, pt_dim);
  }

  // All iteration domains require a separate entry in the buffer for re-entrant
  // grid reductions.
  for (auto fl : for_loops) {
    if (fl->isTrivial()) {
      continue;
    }
    if (fl->iter_domain()->isThread()) {
      // already accounted for.
      continue;
    }
    buffer_size =
        SimplifyingIrBuilder::mulExpr(buffer_size, fl->iter_domain()->extent());
  }

  return buffer_size;
}

Val* getGridSyncBufferSize(
    const TensorDomain* td,
    const std::vector<kir::ForLoop*>& for_loops = {}) {
  // See the comment above for getGridCommWorkBufferSize.
  Val* buffer_size = GpuLower::current()->kernel()->oneVal();
  for (auto pt : kParallelTypeBIDs) {
    auto pt_dim = GpuLower::current()->parallelDimensionMap().get(pt);
    if (pt_dim == nullptr || pt_dim->isOneInt()) {
      continue;
    }
    if (std::any_of(td->domain().begin(), td->domain().end(), [&](auto out_id) {
          return out_id->getParallelType() == pt &&
              (out_id->isReduction() || out_id->isBroadcast());
        })) {
      continue;
    }
    buffer_size = SimplifyingIrBuilder::mulExpr(buffer_size, pt_dim);
  }

  // All iteration domains require a separate semaphore for re-entrant grid
  // reductions
  for (auto fl : for_loops) {
    if (fl->isTrivial()) {
      continue;
    }
    if (fl->iter_domain()->isThread()) {
      // already accounted for.
      continue;
    }

    buffer_size =
        SimplifyingIrBuilder::mulExpr(buffer_size, fl->iter_domain()->extent());
  }

  return buffer_size;
}

Val* getEntranceCountGridReduce(std::vector<kir::ForLoop*>& for_loops) {
  Val* grid_reduction_entrances = GpuLower::current()->kernel()->oneVal();

  for (const auto loop : for_loops) {
    if (loop->isTrivial()) {
      continue;
    }
    if (loop->iter_domain()->isThread()) {
      // already accounted for.
      continue;
    }
    // TODO: Does this work for shift/gather?
    grid_reduction_entrances = SimplifyingIrBuilder::mulExpr(
        grid_reduction_entrances, loop->iter_domain()->extent());
  }
  return grid_reduction_entrances;
}

// Linear indexing of for loops for multiple entrances into grid reduce
// TODO: What happens if there's a broadcast that's resolved (not present in the
// grid reduce) but the global buffer isn't expanded?
Val* getEntranceLinIndGridReduce(std::vector<kir::ForLoop*>& for_loops) {
  Val* linear_index = GpuLower::current()->kernel()->zeroVal();

  for (const auto loop : for_loops) {
    if (loop->isTrivial()) {
      continue;
    }
    if (loop->iter_domain()->isThread()) {
      // already accounted for.
      continue;
    }
    // TODO: Does this work for shift/gather?
    linear_index = SimplifyingIrBuilder::addExpr(
        SimplifyingIrBuilder::mulExpr(
            linear_index, loop->iter_domain()->extent()),
        loop->index());
  }
  return linear_index;
}

} // namespace

void IndexLowering::handle(const ReductionOp* rop) {
  TORCH_INTERNAL_ASSERT(ir_utils::isTvOp(rop));

  const auto out_tv = rop->out()->as<TensorView>();
  const auto out_domain = out_tv->domain();

  const bool has_block_reduce = out_domain->hasBlockReduction();
  const bool has_grid_reduce = out_domain->hasGridReduction();

  const auto out = lowerDstIndex(rop->out());
  const auto in = lowerSrcIndex(rop->in(), rop->out());

  if (has_grid_reduce) {
    handleGridReduction(rop, out, in);
  } else if (has_block_reduce) {
    handleBlockReduction(rop, out, in);
  } else {
    pushBack(
        IrBuilder::create<BinaryOp>(rop->getReductionOpType(), out, out, in));
    GpuLower::current()->propagateExprInfo(rop, back());
  }
}

void IndexLowering::handleBlockReduction(
    const ReductionOp* rop,
    Val* out,
    Val* in) {
  TORCH_INTERNAL_ASSERT(ir_utils::isTvOp(rop));

  ReductionOp* indexed_rop = IrBuilder::create<ReductionOp>(
      rop->getReductionOpType(), rop->init(), out, in, rop->isAllreduce());
  if (rop->predicate()) {
    indexed_rop->setPredicate(rop->predicate());
  }
  if (rop->writePredicate()) {
    indexed_rop->setWritePredicate(rop->writePredicate());
  }

  pushBack(indexed_rop);
  GpuLower::current()->propagateExprInfo(rop, back());
}

void IndexLowering::handleGridReduction(
    const ReductionOp* rop,
    Val* out,
    Val* in) {
  const auto out_tv = out->as<kir::TensorIndex>()->view();
  const auto out_domain = out_tv->domain();

  TORCH_INTERNAL_ASSERT(out_domain->hasGridReduction());

  // If we do a grid reduction we can't have a reduction axis that is not bound
  // to a grid or block dim.
  TORCH_INTERNAL_ASSERT(
      std::none_of(
          out_domain->domain().begin(),
          out_domain->domain().end(),
          [](IterDomain* id) {
            return !id->isThread() && id->isReduction() &&
                !id->extent()->isOneInt();
          }),
      "Found a reduction stage that has both a non-parallelized ",
      "reduction and a grid reduction. This is not supported, ",
      "please use rfactor to do the serialized reduction first, ",
      "then the grid reduction.");

  // When using the fused reduction in a loop, the global work buffer
  // is double buffered to save global synchronizations.
  auto is_within_a_loop = std::any_of(
      out_domain->domain().begin(),
      out_domain->domain().end(),
      [](IterDomain* id) { return !isTrivialIterDomain(id); });

  // Use a unique buffer for work and sync flag when called within a
  // loop unless it's persistent. Grid all reduce means persistence is
  // required. However, not being a grid all reduce does not mean
  // non-persistence. Currently, if a cooperative grid reduction is
  // required anywhere in the kernel, all grid reducitons are done in
  // a persistent manner, so all grid reductions should be consulted.
  // TODO: fix this
  const bool privatize_buffer = !rop->isAllreduce();

  const auto reduce_buffer = ir_utils::allocGlobalBufferForGridComm(
      getGridCommWorkBufferSize(
          out_domain,
          privatize_buffer ? for_loops_ : std::vector<kir::ForLoop*>(),
          rop->isAllreduce() && is_within_a_loop ? 2 : 1),
      out->dtype(),
      false);

  const auto sync_buffer = ir_utils::allocGlobalBufferForGridComm(
      getGridSyncBufferSize(
          out_domain,
          privatize_buffer ? for_loops_ : std::vector<kir::ForLoop*>()),
      DataType::Int,
      true);

  const auto entrance_ind = privatize_buffer
      ? getEntranceLinIndGridReduce(for_loops_)
      : GpuLower::current()->kernel()->zeroVal();
  const auto n_entrances = privatize_buffer
      ? getEntranceCountGridReduce(for_loops_)
      : GpuLower::current()->kernel()->oneVal();

  // The thread predicate for GridReduction needs to be set
  // separately from the main predicate. Do not combine them like
  // other expressions.
  const auto& thread_pred =
      GpuLower::current()->threadPredMap().getPredicatedParallelTypes(out_tv);

  auto grid_reduction = IrBuilder::create<kir::GridReduction>(
      rop->getReductionOpType(),
      rop->init(),
      out,
      in,
      reduce_buffer,
      sync_buffer,
      entrance_ind,
      n_entrances,
      rop->isAllreduce());

  grid_reduction->setThreadPredicate(thread_pred);

  if (rop->predicate()) {
    grid_reduction->setPredicate(rop->predicate());
  }
  if (rop->writePredicate()) {
    grid_reduction->setWritePredicate(rop->writePredicate());
  }

  pushBack(reduce_buffer);
  pushBack(sync_buffer);
  pushBack(grid_reduction);
  GpuLower::current()->propagateExprInfo(rop, back());

  if (rop->isAllreduce()) {
    // When using the fused reduction, allocate the reduction object at
    // the outer-most scope
    auto fused_reduction_alloc_reduction =
        IrBuilder::create<kir::AllocateFusedReduction>(grid_reduction);
    insertAtTopLevel(fused_reduction_alloc_reduction);
  }
}

void IndexLowering::handle(const GroupedReductionOp* grouped_rop) {
  TORCH_INTERNAL_ASSERT(ir_utils::isTvOp(grouped_rop));

  const auto out_tv = ir_utils::getTvOutput(grouped_rop);
  const auto out_domain = out_tv->domain();

  const bool has_block_reduce = out_domain->hasBlockReduction();
  const bool has_grid_reduce = out_domain->hasGridReduction();

  std::vector<Val*> indexed_outputs(grouped_rop->numReductions());
  std::vector<Val*> indexed_inputs(grouped_rop->numReductions());

  for (const auto i : c10::irange(grouped_rop->numReductions())) {
    indexed_outputs.at(i) = lowerDstIndex(grouped_rop->output(i));
    indexed_inputs.at(i) =
        lowerSrcIndex(grouped_rop->input(i), grouped_rop->output(i));
  }

  if (has_grid_reduce) {
    handleGridReduction(grouped_rop, indexed_outputs, indexed_inputs);
  } else if (has_block_reduce) {
    handleBlockReduction(grouped_rop, indexed_outputs, indexed_inputs);
  } else {
    for (const auto i : c10::irange(grouped_rop->numReductions())) {
      pushBack(IrBuilder::create<BinaryOp>(
          grouped_rop->getReductionOpType(i),
          indexed_outputs.at(i),
          indexed_outputs.at(i),
          indexed_inputs.at(i)));
    }
  }
}

void IndexLowering::handleBlockReduction(
    const GroupedReductionOp* grouped_rop,
    const std::vector<Val*>& outputs,
    const std::vector<Val*>& inputs) {
  TORCH_INTERNAL_ASSERT(ir_utils::isTvOp(grouped_rop));

  GroupedReductionOp* indexed_rop = IrBuilder::create<GroupedReductionOp>(
      grouped_rop->getReductionOpTypes(),
      grouped_rop->initVals(),
      outputs,
      inputs,
      grouped_rop->isAllreduce());
  if (grouped_rop->predicate()) {
    indexed_rop->setPredicate(grouped_rop->predicate());
  }
  if (grouped_rop->writePredicate()) {
    indexed_rop->setWritePredicate(grouped_rop->writePredicate());
  }

  pushBack(indexed_rop);
  GpuLower::current()->propagateExprInfo(grouped_rop, back());
}

void IndexLowering::handleGridReduction(
    const GroupedReductionOp* grouped_rop,
    const std::vector<Val*>& outputs,
    const std::vector<Val*>& inputs) {
  const auto out_tv = ir_utils::getTvOutput(grouped_rop);
  const auto out_domain = out_tv->domain();

  TORCH_INTERNAL_ASSERT(out_domain->hasGridReduction());

  // If we do a grid reduction we can't have a reduction axis that is not bound
  // to a grid or block dim.
  TORCH_INTERNAL_ASSERT(
      std::none_of(
          out_domain->domain().begin(),
          out_domain->domain().end(),
          [](IterDomain* id) {
            return !id->isThread() && id->isReduction() &&
                !id->extent()->isOneInt();
          }),
      "Found a reduction stage that has both a non-parallelized ",
      "reduction and a grid reduction. This is not supported, ",
      "please use rfactor to do the serialized reduction first, ",
      "then the grid reduction.");

  // When using the fused reduction in a loop, the global work buffer
  // is double buffered to save global synchronizations.
  auto is_within_a_loop = std::any_of(
      out_domain->domain().begin(),
      out_domain->domain().end(),
      [](IterDomain* id) { return !isTrivialIterDomain(id); });

  std::vector<kir::Allocate*> reduce_buffers;
  std::transform(
      outputs.begin(),
      outputs.end(),
      std::back_inserter(reduce_buffers),
      [&](Val* output) {
        return ir_utils::allocGlobalBufferForGridComm(
            getGridCommWorkBufferSize(
                out_domain,
                for_loops_,
                (grouped_rop->isAllreduce() && is_within_a_loop ? 2 : 1)),
            output->dtype(),
            false);
      });

  const auto sync_buffer = ir_utils::allocGlobalBufferForGridComm(
      getGridSyncBufferSize(out_domain, for_loops_), DataType::Int, true);

  // The thread predicate for GridReduction needs to be set
  // separately from the main predicate. Do not combine them like
  // other expressions.
  const auto& thread_pred =
      GpuLower::current()->threadPredMap().getPredicatedParallelTypes(out_tv);

  auto grid_reduction = IrBuilder::create<kir::GroupedGridReduction>(
      grouped_rop->getReductionOpTypes(),
      grouped_rop->initVals(),
      outputs,
      inputs,
      reduce_buffers,
      sync_buffer,
      grouped_rop->isAllreduce());

  grid_reduction->setThreadPredicate(thread_pred);

  if (grouped_rop->predicate()) {
    grid_reduction->setPredicate(grouped_rop->predicate());
  }
  if (grouped_rop->writePredicate()) {
    grid_reduction->setWritePredicate(grouped_rop->writePredicate());
  }

  for (auto reduce_buffer : reduce_buffers) {
    pushBack(reduce_buffer);
  }
  pushBack(sync_buffer);
  pushBack(grid_reduction);
  GpuLower::current()->propagateExprInfo(grouped_rop, back());

  if (grouped_rop->isAllreduce()) {
    auto fused_reduction_alloc_reduction =
        IrBuilder::create<kir::AllocateFusedReduction>(grid_reduction);
    insertAtTopLevel(fused_reduction_alloc_reduction);
  }
}

void IndexLowering::handle(const WelfordOp* wop) {
  TORCH_INTERNAL_ASSERT(ir_utils::isTvOp(wop));

  const auto out_tv = wop->outAvg()->as<TensorView>();
  const auto out_domain = out_tv->domain();

  const bool has_block_reduce = out_domain->hasBlockReduction();
  const bool has_grid_reduce = out_domain->hasGridReduction();

  // If we do a grid reduction we can't have a reduction axis that is not bound
  // to a grid or block dim ()
  if (has_grid_reduce) {
    TORCH_INTERNAL_ASSERT(
        std::none_of(
            out_domain->domain().begin(),
            out_domain->domain().end(),
            [](IterDomain* id) {
              return !id->isThread() && id->isReduction();
            }),
        "Found a reduction stage that has both a non-parallelized ",
        "reduction and a grid reduction.  This is not supported, ",
        "please use rfactor to do the serialized reduction first, ",
        "then the grid reduction.");
  }

  // lower IO tensors
  const auto in_var =
      wop->inVar() ? lowerSrcIndex(wop->inVar(), wop->outAvg()) : nullptr;
  const auto in_avg = lowerSrcIndex(wop->inAvg(), wop->outAvg());
  auto in_N = wop->inN();

  // in Rfactor-ed case, the input N is actually a TV
  if (!in_N->isScalar()) {
    in_N = lowerSrcIndex(in_N, wop->outN());
  }

  auto out_avg = lowerDstIndex(wop->outAvg());
  auto out_var = lowerDstIndex(wop->outVar());
  auto out_N = lowerDstIndex(wop->outN());

  WelfordOp* indexed_wop = IrBuilder::create<WelfordOp>(
      out_avg,
      out_var,
      out_N,
      wop->initAvg(),
      wop->initVar(),
      wop->initN(),
      in_avg,
      in_var,
      in_N,
      wop->isAllreduce());

  if (wop->predicate()) {
    indexed_wop->setPredicate(wop->predicate());
  }
  if (wop->writePredicate()) {
    indexed_wop->setWritePredicate(wop->writePredicate());
  }

  // Serial welford
  if (!has_block_reduce && !has_grid_reduce) {
    pushBack(indexed_wop);
    GpuLower::current()->propagateExprInfo(wop, back());
    return;
  }

  // Block-only welford
  if (!has_grid_reduce) {
    pushBack(indexed_wop);
    GpuLower::current()->propagateExprInfo(wop, back());
    return;
  }

  handleGridWelford(indexed_wop);
}

void IndexLowering::handleGridWelford(WelfordOp* indexed_wop) {
  const auto out_tv = indexed_wop->out()->as<kir::TensorIndex>()->view();
  const auto out_domain = out_tv->domain();

  // Buffer allocation
  // When using the fused reduction in a loop, the global work buffer
  // is double buffered to save global synchronizations.
  auto is_within_a_loop = std::any_of(
      out_domain->domain().begin(),
      out_domain->domain().end(),
      [](IterDomain* id) { return !isTrivialIterDomain(id); });

  // TODO: See the comment on the same variable in handleGridReduction
  const bool privatize_buffer = !indexed_wop->isAllreduce();

  const auto work_buffer_size = getGridCommWorkBufferSize(
      out_domain,
      privatize_buffer ? for_loops_ : std::vector<kir::ForLoop*>(),
      indexed_wop->isAllreduce() && is_within_a_loop ? 2 : 1);

  const auto out_var_buffer = ir_utils::allocGlobalBufferForGridComm(
      work_buffer_size, indexed_wop->outVar()->dtype(), false);
  const auto out_avg_buffer = ir_utils::allocGlobalBufferForGridComm(
      work_buffer_size, indexed_wop->outAvg()->dtype(), false);
  const auto out_N_buffer = ir_utils::allocGlobalBufferForGridComm(
      work_buffer_size, indexed_wop->outN()->dtype(), false);

  const auto sync_buffer = ir_utils::allocGlobalBufferForGridComm(
      getGridSyncBufferSize(
          out_domain,
          privatize_buffer ? for_loops_ : std::vector<kir::ForLoop*>()),
      DataType::Int,
      true);

  const auto entrance_ind = privatize_buffer
      ? getEntranceLinIndGridReduce(for_loops_)
      : GpuLower::current()->kernel()->zeroVal();
  const auto n_entrances = privatize_buffer
      ? getEntranceCountGridReduce(for_loops_)
      : GpuLower::current()->kernel()->oneVal();

  // The thread predicate for GridReduction needs to be set
  // separately from the main predicate. Do not combine them like
  // other expressions.
  const auto& thread_pred =
      GpuLower::current()->threadPredMap().getPredicatedParallelTypes(out_tv);

  auto grid_welford = IrBuilder::create<kir::GridWelford>(
      indexed_wop,
      out_var_buffer,
      out_avg_buffer,
      out_N_buffer,
      sync_buffer,
      entrance_ind,
      n_entrances);

  grid_welford->setThreadPredicate(thread_pred);

  const bool block_reduce_separated =
      out_domain->hasBlockReduction() && !indexed_wop->isAllreduce();

  if (indexed_wop->predicate()) {
    if (block_reduce_separated) {
      grid_welford->setPredicate(IrBuilder::create<kir::Predicate>(
          GpuLower::current()->kernel()->trueVal()));
    } else {
      grid_welford->setPredicate(indexed_wop->predicate());
    }
  }

  if (indexed_wop->writePredicate()) {
    grid_welford->setWritePredicate(indexed_wop->writePredicate());
  }

  if (block_reduce_separated) {
    pushBack(indexed_wop);
    GpuLower::current()->propagateExprInfo(indexed_wop, back());
  }

  pushBack(out_var_buffer);
  pushBack(out_avg_buffer);
  pushBack(out_N_buffer);
  pushBack(sync_buffer);
  pushBack(grid_welford);
  GpuLower::current()->propagateExprInfo(indexed_wop, back());

  if (indexed_wop->isAllreduce()) {
    // When using the fused reduction, allocate the reduction object at
    // the outer-most scope
    auto fused_reduction_alloc_reduction =
        IrBuilder::create<kir::AllocateFusedReduction>(grid_welford);
    insertAtTopLevel(fused_reduction_alloc_reduction);
  }
}

void IndexLowering::handle(const MmaOp* mma) {
  const auto a = lowerSrcIndex(mma->inA(), mma->out());
  const auto b = lowerSrcIndex(mma->inB(), mma->out());
  const auto out = lowerDstIndex(mma->out());
  auto mma_indexed =
      IrBuilder::create<MmaOp>(out, a, b, mma->init(), mma->options());
  pushBack(mma_indexed);
  GpuLower::current()->propagateExprInfo(mma, back());
}

void IndexLowering::handle(const BroadcastOp* bop) {
  TORCH_INTERNAL_ASSERT(ir_utils::isTvOp(bop));

  const auto out_tv = bop->out()->as<TensorView>();

  const auto out = lowerDstIndex(bop->out());
  const auto in = lowerSrcIndex(bop->in(), bop->out());
  auto indexed_expr =
      IrBuilder::create<BroadcastOp>(out, in, bop->getBroadcastDimFlags());

  const ParallelTypeBitmap parallel_bitmap =
      GpuLower::current()->threadPredMap().getParallelBroadcastDomains(out_tv);

  const bool block_x = parallel_bitmap.get(ParallelType::BIDx);
  const bool block_y = parallel_bitmap.get(ParallelType::BIDy);
  const bool block_z = parallel_bitmap.get(ParallelType::BIDz);

  if (bop->predicate()) {
    indexed_expr->setPredicate(bop->predicate());
  }

  const bool grid_broadcast_needed = block_x || block_y || block_z;
  if (!grid_broadcast_needed) {
    pushBack(indexed_expr);
    GpuLower::current()->propagateExprInfo(bop, back());
    return;
  }

  // Grid broadcast
  const auto out_domain = out_tv->domain();
  const auto broadcast_buffer = ir_utils::allocGlobalBufferForGridComm(
      getGridCommWorkBufferSize(out_domain), out->dtype(), false);

  const auto sync_buffer = ir_utils::allocGlobalBufferForGridComm(
      getGridSyncBufferSize(out_domain), DataType::Int, true);

  auto grid_broadcast = IrBuilder::create<kir::GridBroadcast>(
      indexed_expr, broadcast_buffer, sync_buffer);

  if (bop->predicate()) {
    grid_broadcast->setPredicate(bop->predicate());
  }

  pushBack(broadcast_buffer);
  pushBack(sync_buffer);
  pushBack(grid_broadcast);
  GpuLower::current()->propagateExprInfo(bop, back());
}

void IndexLowering::handle(const kir::Allocate* allocate) {
  // TODO(kir): remove the need for const_cast
  pushBack(const_cast<kir::Allocate*>(allocate)); // NOLINT
}

void IndexLowering::handle(const kir::BlockSync* sync) {
  // TODO(kir): remove the need for const_cast
  pushBack(const_cast<kir::BlockSync*>(sync)); // NOLINT
}

void IndexLowering::handle(const kir::GridSync* sync) {
  // TODO(kir): remove the need for const_cast
  pushBack(const_cast<kir::GridSync*>(sync)); // NOLINT
}

void IndexLowering::generate(const std::vector<Expr*>& exprs) {
  for (auto expr : exprs) {
    OptOutConstDispatch::handle(expr);
  }
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
