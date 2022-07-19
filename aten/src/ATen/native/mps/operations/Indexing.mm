//  Copyright © 2022 Apple Inc.

#include <ATen/ATen.h>
#include <ATen/Tensor.h>
#include <ATen/Utils.h>

#include <ATen/ceil_div.h>
#include <ATen/NativeFunctions.h>
#include <ATen/ExpandUtils.h>
#include <ATen/MemoryOverlap.h>
#include <ATen/mps/MPSStream.h>
#include <ATen/native/LinearAlgebraUtils.h>
#include <ATen/native/mps/OperationUtils.h>
#include <ATen/native/Resize.h>
#include <ATen/AccumulateType.h>
#include <torch/library.h>
#include <ATen/native/TensorAdvancedIndexing.h>
#include <ATen/native/IndexingUtils.h>
#include <c10/util/irange.h>
#include <c10/core/QScheme.h>
#include <c10/util/SmallVector.h>

#ifdef __OBJC__
#include <MetalPerformanceShaders/MetalPerformanceShaders.h>
#endif

namespace at {
namespace native {

Tensor index_select_mps(const Tensor & self,
                         int64_t dim,
                         const Tensor & index) {
  IntArrayRef input_shape = self.sizes();
  auto num_input_dims = input_shape.size();

  auto num_indices = index.numel();
  TORCH_CHECK_INDEX(index.dim() <= 1, "index_select(): Index is supposed to be a vector");

  dim = maybe_wrap_dim(dim, self.dim());
  std::vector<int64_t> shape_data(num_input_dims);

  // Calculate new shape
  for(auto i : c10::irange(num_input_dims)) {
    if (i == dim) {
      shape_data[i] = num_indices;
    } else {
      shape_data[i] = input_shape[i];
    }
  }

  IntArrayRef output_shape = IntArrayRef(shape_data.data(), num_input_dims);

  Tensor result = at::native::empty_mps(
                      output_shape,
                      self.scalar_type(),
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);

  index_select_out_mps(self, dim, index, result);
  return result;
}

Tensor& index_select_out_mps(const Tensor & self,
                             int64_t dim,
                             const Tensor & index,
                             Tensor & output) {

  using namespace mps;
  MPSStream* stream = getCurrentMPSStream();
  dim = maybe_wrap_dim(dim, self.dim());
  // Checks
  TORCH_CHECK_INDEX(index.dim() <= 1, "index_select(): Index is supposed to be a vector");
  TORCH_CHECK(index.scalar_type() == ScalarType::Long || index.scalar_type() == ScalarType::Int, "index_select(): Expected dtype int32 or int64 for index");
  TORCH_CHECK(self.scalar_type() == output.scalar_type(),
              "index_select(): self and output must have the same scalar type");
  TORCH_CHECK(dim == 0 || dim < self.dim(),
              "index_select(): Indexing dim ", dim, " is out of bounds of tensor");

  // Derive from MPSCachedGraph
  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor* inputTensor_ = nil;
    MPSGraphTensor* indexTensor_ = nil;
    MPSGraphTensor* outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  @autoreleasepool {

    string key = "index_select_out_mps" + getTensorsStringKey({self, index}) + ":" + std::to_string(dim);
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {
        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);
          MPSGraphTensor* indexTensor = mpsGraphRankedPlaceHolder(mpsGraph, index);

          MPSGraphTensor* outputTensor = [mpsGraph gatherWithUpdatesTensor:inputTensor
                                                             indicesTensor:indexTensor
                                                                      axis:dim
                                                           batchDimensions:0
                                                                      name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->indexTensor_ = indexTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder indexPlaceholder = Placeholder(cachedGraph->indexTensor_, index);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData(),
      indexPlaceholder.getMPSGraphTensor() : indexPlaceholder.getMPSGraphTensorData()
    };
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

  return output;

}

Tensor & masked_fill__mps(Tensor& self, const Tensor & mask, const Scalar& value) {
  using namespace mps;
  TORCH_CHECK(self.device() == mask.device(), "expected self and mask to be on the same device, but got mask on ",
    mask.device(), " and self on ", self.device());
  TORCH_CHECK(mask.scalar_type() == kByte || mask.scalar_type() == kBool,
    "expected mask dtype to be Bool but got ", mask.scalar_type());
  auto maybe_outnames = namedinference::broadcast_to_outnames(self, mask, "masked_fill_");

  c10::MaybeOwned<Tensor> b_mask = expand_inplace(self, mask, "masked_fill_");

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *maskTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();
  @autoreleasepool {
    string key = "masked_fill" + getTensorsStringKey({self, mask}) + ":" + std::to_string(value.toDouble());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);
          MPSGraphTensor* maskTensor = mpsGraphRankedPlaceHolder(mpsGraph, mask);
          MPSDataType valueType = getMPSScalarType(value.type());

          // constantWithScalar doesn't like Bool constants getting created so
          // mapping them to int8
          if (valueType == MPSDataTypeBool) {
            valueType = MPSDataTypeInt8;
          }
          MPSGraphTensor* valueTensor =  [mpsGraph constantWithScalar:value.to<double>()
                                                            dataType:valueType];
          valueTensor = [mpsGraph castTensor:valueTensor
                                          toType:getMPSDataType(self.scalar_type())
                                           name : @"castTensorEq"];

          MPSGraphTensor* outputTensor = [mpsGraph selectWithPredicateTensor:maskTensor
                                                        truePredicateTensor:valueTensor
                                                        falsePredicateTensor:inputTensor
                                                             name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->maskTensor_ = maskTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder   = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder maskPlaceholder   = Placeholder(cachedGraph->maskTensor_, mask);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, self);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData(),
      maskPlaceholder.getMPSGraphTensor() : maskPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }
  namedinference::propagate_names_if_nonempty(self, maybe_outnames);
  return self;
}

Tensor embedding_dense_backward_mps(
    const Tensor & grad_, const Tensor & indices, int64_t num_weights,
    int64_t padding_idx, bool scale_grad_by_freq)
{
    // TODO: implement padding_idx & scale_grad_by_freq.
    namespace native_mps = at::native::mps;
    struct CachedGraph : public native_mps::MPSCachedGraph
    {
      CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
      MPSGraphTensor *incomingGradTensor_ = nil;
      MPSGraphTensor *indicesTensor_ = nil;
      MPSGraphTensor *outgoingGradTensor_ = nil;
    };

    native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

    IntArrayRef incoming_gradient_shape = grad_.sizes();
    int64_t num_incoming_gradient_dims = incoming_gradient_shape.size();

    IntArrayRef indices_shape = indices.sizes();
    int64_t num_indices_dims = indices_shape.size();

    int64_t D = incoming_gradient_shape[num_incoming_gradient_dims - 1];
    c10::SmallVector<int64_t, 2> outgoing_gradient_shape{num_weights, D};
    Tensor outgoing_gradient = at::native::empty_mps(
                                IntArrayRef(outgoing_gradient_shape.data(), outgoing_gradient_shape.size()),
                                grad_.scalar_type(),
                                c10::nullopt,
                                kMPS,
                                c10::nullopt,
                                c10::nullopt);

    if (outgoing_gradient.numel() == 0) {
      return outgoing_gradient;
    }

    auto stream = at::mps::getCurrentMPSStream();

    @autoreleasepool {
        string key = "edb_mps:" + native_mps::getMPSTypeString(grad_.scalar_type()) + ":indices" + std::to_string(num_indices_dims) + ":num_weights" + std::to_string(num_weights) + ":padding_idx" + std::to_string(padding_idx) + ":scaled" + std::to_string(scale_grad_by_freq);
      CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
      // Initialize once if configuration not found in cache
      if(!cachedGraph) {
        native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

          CachedGraph *newCachedGraph = nil;

          @autoreleasepool {
            MPSGraph* mpsGraph = native_mps::make_mps_graph();
            newCachedGraph = new CachedGraph(mpsGraph);

            MPSGraphTensor* incomingGradTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(grad_.scalar_type()));

            MPSGraphTensor* indicesTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(indices.scalar_type()));

            MPSGraphTensor *reshapedIndicesTensor = [mpsGraph  expandDimsOfTensor:indicesTensor
                             axes:@[@-1]
                             name:nil];

            MPSGraphTensor *outgoingGradTensor;
            outgoingGradTensor = [mpsGraph scatterNDWithUpdatesTensor:incomingGradTensor
                            indicesTensor:reshapedIndicesTensor
                                    shape:native_mps::getMPSShape(IntArrayRef(outgoing_gradient_shape.data(), outgoing_gradient_shape.size()))
                          batchDimensions:0
                                     mode:MPSGraphScatterModeAdd
                                     name:@"edb"];

            newCachedGraph->incomingGradTensor_ = incomingGradTensor;
            newCachedGraph->indicesTensor_ = indicesTensor;
            newCachedGraph->outgoingGradTensor_ = outgoingGradTensor;

          }
          return newCachedGraph;
        });
        cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
      }
      auto incomingGradPlaceholder = native_mps::Placeholder(cachedGraph->incomingGradTensor_, grad_);
      auto indicesPlaceholder = native_mps::Placeholder(cachedGraph->indicesTensor_, indices);
      auto outgoingGradPlaceholder = native_mps::Placeholder(cachedGraph->outgoingGradTensor_, outgoing_gradient);

      NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
          incomingGradPlaceholder.getMPSGraphTensor() : incomingGradPlaceholder.getMPSGraphTensorData(),
          indicesPlaceholder.getMPSGraphTensor() : indicesPlaceholder.getMPSGraphTensorData()
      };

      NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
          outgoingGradPlaceholder.getMPSGraphTensor() : outgoingGradPlaceholder.getMPSGraphTensorData()
      };
      native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);
    }
    return outgoing_gradient;
}

Tensor & masked_fill__mps(Tensor& self, const Tensor & mask, const Tensor & value) {
  TORCH_CHECK(value.dim() == 0, "masked_fill_ only supports a 0-dimensional value tensor, but got tensor "
      "with ", value.dim(), " dimension(s).");
  return masked_fill__mps(self, mask, value.item());
}

}
}
