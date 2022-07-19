//  Copyright © 2022 Apple Inc.

#include <ATen/ATen.h>
#include <ATen/Tensor.h>
#include <ATen/Utils.h>
#include <ATen/mps/MPSStream.h>
#include <ATen/native/mps/OperationUtils.h>
#include <torch/library.h>

#ifdef __OBJC__
#include <MetalPerformanceShaders/MetalPerformanceShaders.h>
#endif

using namespace at::mps;

namespace at {
namespace native {

Tensor relu_mps(const Tensor& self) {
  using namespace mps;
  Tensor output = at::empty_like(self);
  resize_tensor(&output);
  TORCH_CHECK(output.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "relu" + getTensorsStringKey({self});
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);
          // passing selector of reLUWithTensor on the mpsGraph object
          MPSGraphTensor* outputTensor = [mpsGraph reLUWithTensor:inputTensor
                                                             name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder   = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }

  return output;
}

Tensor & relu_mps_(Tensor & self) {
  using namespace mps;
  // Inplace relu
  Tensor &output = self;
  TORCH_CHECK(output.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "relu_" + getTensorsStringKey({self});
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);
          // passing selector of reLUWithTensor on the mpsGraph object
          MPSGraphTensor* outputTensor = [mpsGraph reLUWithTensor:inputTensor
                                                             name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder   = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }

  return output;
}

TORCH_IMPL_FUNC(leaky_relu_out_mps) (
  const Tensor& self, const Scalar& negative_slope, const Tensor& output) {
  using namespace mps;
  TORCH_CHECK(output.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream *stream = getCurrentMPSStream();

  @autoreleasepool {

    string key = "leaky_relu" + getTensorsStringKey({self}) + ":" + to_string(negative_slope.to<double>());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);

          MPSGraphTensor* negSlopeTensor = [mpsGraph constantWithScalar:negative_slope.to<double>()
                                                                  shape:@[@1]
                                                               dataType:getMPSDataType(self.scalar_type())];
          MPSGraphTensor* negSlopeMulXTensor = [mpsGraph multiplicationWithPrimaryTensor:inputTensor
                                                                         secondaryTensor:negSlopeTensor
                                                                                    name:nil];
          MPSGraphTensor* outputTensor = [mpsGraph maximumWithPrimaryTensor:negSlopeMulXTensor
                                                            secondaryTensor:inputTensor
                                                                       name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }

}

TORCH_IMPL_FUNC(leaky_relu_backward_out_mps) (
  const Tensor& grad_output,
  const Tensor& self,
  const Scalar& negative_slope,
  bool self_is_result,
  const Tensor& output ) {

  using namespace mps;
  TORCH_CHECK(output.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor* inputTensor_ = nil;
    MPSGraphTensor* gradOutputTensor_ = nil;
    MPSGraphTensor* gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream *stream = getCurrentMPSStream();

  @autoreleasepool {

    string key = "leaky_relu_backward" + getTensorsStringKey({self, grad_output}) + ":" + to_string(negative_slope.to<double>());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);
          MPSGraphTensor* gradOutputTensor = mpsGraphRankedPlaceHolder(mpsGraph, grad_output);

          MPSGraphTensor* negSlopeTensor = [mpsGraph constantWithScalar:negative_slope.to<double>()
                                                                  shape:@[@1]
                                                               dataType:getMPSScalarType(self.scalar_type())];
          MPSGraphTensor* zeroTensor = [mpsGraph constantWithScalar:0.0f
                                                              shape:@[@1]
                                                           dataType:getMPSScalarType(self.scalar_type())];
          MPSGraphTensor* predicateTensor = [mpsGraph greaterThanWithPrimaryTensor:inputTensor
                                                                   secondaryTensor:zeroTensor
                                                                              name:nil];
          MPSGraphTensor* gradientsMulNegSlopeTensor = [mpsGraph multiplicationWithPrimaryTensor:gradOutputTensor
                                                                                 secondaryTensor:negSlopeTensor
                                                                                            name:nil];
          MPSGraphTensor* gradInputTensor = [mpsGraph selectWithPredicateTensor:predicateTensor
                                                            truePredicateTensor:gradOutputTensor
                                                           falsePredicateTensor:gradientsMulNegSlopeTensor
                                                                           name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->gradOutputTensor_ = gradOutputTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder gradOutputPlaceholder = Placeholder(cachedGraph->gradOutputTensor_, grad_output);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->gradInputTensor_, output);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData(),
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }
}


TORCH_IMPL_FUNC(log_softmax_mps_out) (
  const Tensor &self,
  const int64_t dim,
  const bool half_to_float,
  const Tensor &out) {
  using namespace mps;

  if (self.numel() == 0) {
    return;
  }

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor* inputTensor_ = nil;
    MPSGraphTensor* outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = at::mps::getCurrentMPSStream();

  @autoreleasepool {
    string key = "log_softmax_mps_out" + getTensorsStringKey({self}) + ":" + to_string(dim);
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph* newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);

          MPSGraphTensor* softmaxTensor = [mpsGraph softMaxWithTensor:inputTensor
                                                                 axis:dim
                                                                 name:nil];
          MPSGraphTensor* outputTensor = [mpsGraph logarithmWithTensor:softmaxTensor
                                                                  name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, out);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

}

TORCH_IMPL_FUNC(log_softmax_backward_mps_out) (
  const Tensor& grad_output,
  const Tensor& output,
  int64_t dim,
  ScalarType input_dtype,
  const Tensor& out) {
  using namespace mps;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor* outputTensor_ = nil;
    MPSGraphTensor* gradOutputTensor_ = nil;
    MPSGraphTensor* gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = at::mps::getCurrentMPSStream();

  @autoreleasepool {
    string key = "log_softmax_backward_mps_out:" + getMPSTypeString(grad_output.scalar_type()) + ":" + to_string(dim);
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph* newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* gradOutputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(grad_output.scalar_type()));
          MPSGraphTensor* outputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(output.scalar_type()));

          MPSGraphTensor* expTensor = [mpsGraph exponentWithTensor:outputTensor
                                                              name:nil];
          MPSGraphTensor* sumTensor = [mpsGraph reductionSumWithTensor:gradOutputTensor
                                                                  axis:dim
                                                                  name:nil];
          MPSGraphTensor* multiplicationTensor = [mpsGraph multiplicationWithPrimaryTensor:expTensor
                                                                           secondaryTensor:sumTensor
                                                                                      name:nil];
          MPSGraphTensor* resultTensor = [mpsGraph subtractionWithPrimaryTensor:gradOutputTensor
                                                                secondaryTensor:multiplicationTensor
                                                                           name:nil];

          newCachedGraph->gradOutputTensor_ = gradOutputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
          newCachedGraph->gradInputTensor_ = resultTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder gradPlaceholder   = Placeholder(cachedGraph->gradOutputTensor_, grad_output);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);
    Placeholder resultPlaceholder = Placeholder(cachedGraph->gradInputTensor_, out);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      gradPlaceholder.getMPSGraphTensor() : gradPlaceholder.getMPSGraphTensorData(),
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      resultPlaceholder.getMPSGraphTensor() : resultPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

}

TORCH_IMPL_FUNC(sigmoid_out_mps)(
  const Tensor& self,
  const Tensor& output) {
  using namespace mps;
  TORCH_CHECK(output.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "sigmoid_out_mps" + getTensorsStringKey({self});
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          // Initialize graph
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);

          MPSGraphTensor* outputTensor = [mpsGraph sigmoidWithTensor:inputTensor
                                                                name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder   = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }

}

TORCH_IMPL_FUNC(sigmoid_backward_out_mps)(
  const Tensor& grad_output,
  const Tensor& output,
  const Tensor& grad_input) {
  using namespace mps;
  TORCH_CHECK(grad_input.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *gradOutputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
    MPSGraphTensor *gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "sigmoid_backward_out_mps:" + getMPSTypeString(grad_output.scalar_type());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* gradOutputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(grad_output.scalar_type()));
          MPSGraphTensor* outputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(output.scalar_type()));

          MPSGraphTensor* unitTensor = [mpsGraph constantWithScalar:1.0
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* oneMinusSigmoidTensor = [mpsGraph subtractionWithPrimaryTensor:unitTensor
                                                                         secondaryTensor:outputTensor
                                                                                    name:nil];
          MPSGraphTensor* timesTensor = [mpsGraph multiplicationWithPrimaryTensor:oneMinusSigmoidTensor
                                                               secondaryTensor:outputTensor
                                                                          name:nil];
          MPSGraphTensor* gradInputTensor = [mpsGraph multiplicationWithPrimaryTensor:gradOutputTensor
                                                                      secondaryTensor:timesTensor
                                                                                 name:nil];

          newCachedGraph->gradOutputTensor_ = gradOutputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder gradOutputPlaceholder   = Placeholder(cachedGraph->gradOutputTensor_, grad_output);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);
    Placeholder gradInputPlaceholder   = Placeholder(cachedGraph->gradInputTensor_, grad_input);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData(),
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      gradInputPlaceholder.getMPSGraphTensor() : gradInputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }

}

TORCH_IMPL_FUNC(tanh_backward_out_mps)(
  const Tensor& grad_output,
  const Tensor& output,
  const Tensor& grad_input) {
  using namespace mps;
  TORCH_CHECK(grad_input.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *gradOutputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
    MPSGraphTensor *gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "tanh_backward_out_mps:" + getMPSTypeString(grad_output.scalar_type());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* gradOutputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(grad_output.scalar_type()));
          MPSGraphTensor* outputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(output.scalar_type()));

          MPSGraphTensor* unitTensor = [mpsGraph constantWithScalar:1.0
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* tanh2Tensor = [mpsGraph squareWithTensor:outputTensor
                                                              name:nil];
          MPSGraphTensor* oneMinusTanh2Tensor = [mpsGraph subtractionWithPrimaryTensor:unitTensor
                                                                       secondaryTensor:tanh2Tensor
                                                                                  name:nil];
          MPSGraphTensor* gradInputTensor = [mpsGraph multiplicationWithPrimaryTensor:gradOutputTensor
                                                                      secondaryTensor:oneMinusTanh2Tensor
                                                                                 name:nil];

          newCachedGraph->gradOutputTensor_ = gradOutputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder gradOutputPlaceholder   = Placeholder(cachedGraph->gradOutputTensor_, grad_output);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);
    Placeholder gradInputPlaceholder   = Placeholder(cachedGraph->gradInputTensor_, grad_input);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData(),
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      gradInputPlaceholder.getMPSGraphTensor() : gradInputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }
}

TORCH_IMPL_FUNC(threshold_out_mps)(
  const Tensor& self,
  const Scalar& threshold,
  const Scalar& value,
  const Tensor& result) {
  using namespace mps;
  TORCH_CHECK(self.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "threshold_out_mps" + getTensorsStringKey({self}) + ":" +
                                       to_string(threshold.to<double>()) + ":" +
                                       to_string(value.to<double>());

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);

          MPSGraphTensor *thresholdTensor = [mpsGraph constantWithScalar: threshold.to<double>()
                                                                   shape: @[@1]
                                                                dataType: getMPSDataType(self.scalar_type())];

          MPSGraphTensor *valueTensor = [mpsGraph constantWithScalar: value.to<double>()
                                                               shape: @[@1]
                                                            dataType: getMPSDataType(self.scalar_type())];

          // x > threshold
          MPSGraphTensor *predicateTensor = [mpsGraph greaterThanWithPrimaryTensor: inputTensor
                                                                   secondaryTensor: thresholdTensor
                                                                              name: nil];

          // result = (self > threshold) ? self : value
          MPSGraphTensor *outputTensor = [mpsGraph selectWithPredicateTensor: predicateTensor
                                                         truePredicateTensor: inputTensor
                                                        falsePredicateTensor: valueTensor
                                                                        name: nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, result);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }
}

TORCH_IMPL_FUNC(threshold_backward_out_mps)(
  const Tensor& grad,
  const Tensor& self,
  const Scalar& threshold,
  const Tensor& gradInput) {
  using namespace mps;
  TORCH_CHECK(self.is_mps());
  TORCH_CHECK(grad.is_mps());

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *gradTensor_ = nil;
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "threshold_backward_out_mps" + getTensorsStringKey({self, grad}) + ":" +
                                                 to_string(threshold.to<double>());

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);
          MPSGraphTensor *gradTensor = mpsGraphRankedPlaceHolder(mpsGraph, grad);

          MPSGraphTensor *thresholdTensor = [mpsGraph constantWithScalar: threshold.to<double>()
                                                                   shape: @[@1]
                                                                dataType: getMPSDataType(self.scalar_type())];

          MPSGraphTensor *zeroTensor = [mpsGraph constantWithScalar: 0.0
                                                           dataType: inputTensor.dataType];

          // x > threshold
          MPSGraphTensor *predicateTensor = [mpsGraph greaterThanWithPrimaryTensor: inputTensor
                                                                   secondaryTensor: thresholdTensor
                                                                              name: nil];

          // result = (self > threshold) ? grad : zeroTensor
          MPSGraphTensor *gradInputTensor = [mpsGraph selectWithPredicateTensor: predicateTensor
                                                         truePredicateTensor: gradTensor
                                                        falsePredicateTensor: zeroTensor
                                                                        name: nil];

          newCachedGraph->gradTensor_ = gradTensor;
          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder gradPlaceholder = Placeholder(cachedGraph->gradTensor_, grad);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->gradInputTensor_, gradInput);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      gradPlaceholder.getMPSGraphTensor() : gradPlaceholder.getMPSGraphTensorData(),
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }
}

MPSGraphTensor* normcdf (MPSGraph* mpsGraph, MPSGraphTensor *inputTensor) {
    // (1.0f + erf(x*SQRT1_2)) * 0.5f * x;
    const float SQRT1_2 = 0.707106781186547524400844362104849039f;
    MPSGraphTensor *sqrt1_2 = [mpsGraph constantWithScalar:SQRT1_2
                                                        shape:@[@1]
                                                     dataType:MPSDataTypeFloat32];
    MPSGraphTensor *onef = [mpsGraph constantWithScalar:1.0f
                                                  shape:@[@1]
                                              dataType:MPSDataTypeFloat32];
    MPSGraphTensor *halff = [mpsGraph constantWithScalar:0.5f
                                                    shape:@[@1]
                                                dataType:MPSDataTypeFloat32];

    MPSGraphTensor *erfTensor = [mpsGraph multiplicationWithPrimaryTensor: inputTensor
                                                          secondaryTensor: sqrt1_2
                                                                  name : nil];
    erfTensor = [mpsGraph erfWithTensor: erfTensor name : nil];
    erfTensor = [mpsGraph additionWithPrimaryTensor: erfTensor
                                      secondaryTensor: onef
                                                  name : nil];
    erfTensor = [mpsGraph multiplicationWithPrimaryTensor: erfTensor
                                        secondaryTensor: halff
                                                    name : nil];

    return  erfTensor;
}

TORCH_IMPL_FUNC(gelu_out_mps) (
    const Tensor& self, c10::string_view approximate, const Tensor& output
  ) {
  using namespace mps;
  TORCH_CHECK(output.is_mps());

  // Empty output
  if(output.numel() == 0)
    return;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "gelu_out_mps" + getTensorsStringKey({self});
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph,
                                                                  getMPSDataType(self.scalar_type()),
                                                                  getMPSShape(self));

          MPSGraphTensor* outputTensor =  normcdf(mpsGraph, inputTensor);
          outputTensor = [mpsGraph multiplicationWithPrimaryTensor:outputTensor
                                                   secondaryTensor:inputTensor
                                                              name:nil];
          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder   = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };
    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }

}

TORCH_IMPL_FUNC(gelu_backward_out_mps) (
    const Tensor& grad, const Tensor& self, c10::string_view approximate, const Tensor& grad_input
  ) {
  using namespace mps;
  constexpr float kBeta = M_2_SQRTPI * M_SQRT1_2 * (0.5);

  // Empty output
  if(grad_input.numel() == 0)
    return;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *gradTensor_ = nil;
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "gelu_backward_out_mps" + getTensorsStringKey({self, grad});
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* gradTensor = mpsGraphRankedPlaceHolder(mpsGraph,
                                                                  getMPSDataType(grad.scalar_type()),
                                                                  getMPSShape(grad));
          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph,
                                                                  getMPSDataType(self.scalar_type()),
                                                                  getMPSShape(self));
          MPSGraphTensor* cdf = normcdf(mpsGraph, inputTensor);
          MPSGraphTensor *halff = [mpsGraph constantWithScalar:-0.5f
                                                    shape:@[@1]
                                                dataType:MPSDataTypeFloat32];
          MPSGraphTensor *betaf = [mpsGraph constantWithScalar:kBeta
                                                    shape:@[@1]
                                                dataType:MPSDataTypeFloat32];
          MPSGraphTensor *pdfMul = [mpsGraph squareWithTensor : inputTensor
                                                    name : nil];
          pdfMul = [mpsGraph multiplicationWithPrimaryTensor : pdfMul
                                          secondaryTensor : halff
                                                    name : nil];
          pdfMul = [mpsGraph exponentWithTensor : pdfMul
                                        name  : nil];
          MPSGraphTensor* pdf = [mpsGraph multiplicationWithPrimaryTensor : pdfMul
                                                        secondaryTensor  : betaf
                                                                  name : nil];
          pdf = [mpsGraph multiplicationWithPrimaryTensor : inputTensor
                                          secondaryTensor : pdf
                                            name : nil];
          pdf = [mpsGraph additionWithPrimaryTensor : pdf
                                  secondaryTensor : cdf
                                      name : nil];
          MPSGraphTensor* outputTensor = [mpsGraph multiplicationWithPrimaryTensor : gradTensor
                                                                   secondaryTensor : pdf
                                                                              name : nil];

          newCachedGraph->gradTensor_ = gradTensor;
          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder gradPlaceholder   = Placeholder(cachedGraph->gradTensor_, grad);
    Placeholder selfPlaceholder   = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, grad_input);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      gradPlaceholder.getMPSGraphTensor() : gradPlaceholder.getMPSGraphTensorData(),
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };
    runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }


}

void elu_variants_out_mps (
  const Tensor& self,
  const Scalar& alpha,
  const Scalar& scale,
  const Scalar& input_scale,
  const Tensor& result,
  string func_name) {

  using namespace mps;
  TORCH_CHECK(self.is_mps());

  // Empty output
  if(result.numel() == 0)
    return;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = func_name + ":" + getTensorsStringKey({self}) + ":" +
                                       to_string(alpha.to<double>()) + ":" +
                                       to_string(scale.to<double>()) + ":" +
                                       to_string(input_scale.to<double>());

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);

          // scale * (max(0, x) + min(0, alpha * (exp(input_scale * x) - 1) ))

          MPSGraphTensor* alphaTensor = [mpsGraph constantWithScalar:alpha.to<double>()
                                                               shape:@[@1]
                                                            dataType:getMPSDataType(self.scalar_type())];

          MPSGraphTensor* inputScaleTensor = [mpsGraph constantWithScalar:input_scale.to<double>()
                                                                    shape:@[@1]
                                                                 dataType:getMPSDataType(self.scalar_type())];

          MPSGraphTensor* scaleTensor = [mpsGraph constantWithScalar:scale.to<double>()
                                                               shape:@[@1]
                                                            dataType:getMPSDataType(self.scalar_type())];
          MPSGraphTensor* unitTensor = [mpsGraph constantWithScalar:1.0f
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(self.scalar_type())];
          MPSGraphTensor* zeroTensor = [mpsGraph constantWithScalar:0.0f
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(self.scalar_type())];

          MPSGraphTensor* scaledInputTensor = [mpsGraph multiplicationWithPrimaryTensor:inputTensor
                                                                        secondaryTensor:inputScaleTensor
                                                                                   name:nil];
          MPSGraphTensor* exponentTensor = [mpsGraph exponentWithTensor:scaledInputTensor
                                                                   name:nil];
          MPSGraphTensor* exponentMinusOneTensor = [mpsGraph subtractionWithPrimaryTensor:exponentTensor
                                                                          secondaryTensor:unitTensor
                                                                                     name:nil];
          MPSGraphTensor* alphaTimesTensor = [mpsGraph multiplicationWithPrimaryTensor:exponentMinusOneTensor
                                                                       secondaryTensor:alphaTensor
                                                                                  name:nil];
          MPSGraphTensor* predicateTensor = [mpsGraph greaterThanWithPrimaryTensor:inputTensor
                                                                   secondaryTensor:zeroTensor
                                                                              name:nil];
          MPSGraphTensor* fusedOutput = [mpsGraph selectWithPredicateTensor:predicateTensor
                                                        truePredicateTensor:inputTensor
                                                       falsePredicateTensor:alphaTimesTensor
                                                                       name:nil];
          MPSGraphTensor* outputTensor = [mpsGraph multiplicationWithPrimaryTensor:fusedOutput
                                                                   secondaryTensor:scaleTensor
                                                                              name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, result);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

}

// scale * (max(0, x) + min(0, alpha * (exp(input_scale * x) - 1) ))
TORCH_IMPL_FUNC(elu_out_mps) (
   const Tensor& self,
   const Scalar& alpha,
   const Scalar& scale,
   const Scalar& input_scale,
   const Tensor& result) {

  elu_variants_out_mps(self, alpha, scale, input_scale, result, "elu_out_mps");
}

TORCH_IMPL_FUNC(elu_backward_out_mps) (
  const Tensor& grad_output,
  const Scalar& alpha,
  const Scalar& scale,
  const Scalar& input_scale,
  bool is_result,
  const Tensor& self_or_result,
  const Tensor& grad_input
) {

  using namespace mps;
  TORCH_CHECK(grad_output.is_mps());

  // Empty output
  if(grad_input.numel() == 0)
    return;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *gradOutputTensor_ = nil;
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *resultTensor_ = nil;
    MPSGraphTensor *gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "elu_backward_out_mps:" + getTensorsStringKey({grad_output}) + ":" +
                                                 to_string(alpha.to<double>()) + ":" +
                                                 to_string(scale.to<double>()) + ":" +
                                                 to_string(input_scale.to<double>()) + ":" +
                                                 to_string(is_result);

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* gradOutputTensor = mpsGraphRankedPlaceHolder(mpsGraph, grad_output);

          MPSGraphTensor* inputTensor = nil;
          MPSGraphTensor* resultTensor = nil;

          MPSGraphTensor* lessThanZeroGradTensor = nil;

          if(is_result) {
            resultTensor = mpsGraphRankedPlaceHolder(mpsGraph, self_or_result);
            MPSGraphTensor* alphaTensor = [mpsGraph constantWithScalar:alpha.to<double>()
                                                               shape:@[@1]
                                                            dataType:getMPSDataType(grad_output.scalar_type())];
            MPSGraphTensor* resultPlusAlphaTensor = [mpsGraph additionWithPrimaryTensor:resultTensor
                                                                        secondaryTensor:alphaTensor
                                                                                   name:nil];
            auto constMul = scale.to<double>() * input_scale.to<double>();
            MPSGraphTensor* constMulTensor = [mpsGraph constantWithScalar:constMul
                                                                    shape:@[@1]
                                                                 dataType:getMPSDataType(grad_output.scalar_type())];
            lessThanZeroGradTensor = [mpsGraph multiplicationWithPrimaryTensor:resultPlusAlphaTensor
                                                               secondaryTensor:constMulTensor
                                                                          name:nil];
          }
          else {
            inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self_or_result);
            MPSGraphTensor* inputScaleTensor = [mpsGraph constantWithScalar:input_scale.to<double>()
                                                                    shape:@[@1]
                                                                 dataType:getMPSDataType(grad_output.scalar_type())];
            MPSGraphTensor* scaledInputTensor = [mpsGraph multiplicationWithPrimaryTensor:inputTensor
                                                                          secondaryTensor:inputScaleTensor
                                                                                     name:nil];
            MPSGraphTensor* expTensor = [mpsGraph exponentWithTensor:scaledInputTensor
                                                                name:nil];
            auto constMul = scale.to<double>() * input_scale.to<double>() * alpha.to<double>();
            MPSGraphTensor* constMulTensor = [mpsGraph constantWithScalar:constMul
                                                                    shape:@[@1]
                                                                 dataType:getMPSDataType(grad_output.scalar_type())];
            lessThanZeroGradTensor = [mpsGraph multiplicationWithPrimaryTensor:expTensor
                                                               secondaryTensor:constMulTensor
                                                                          name:nil];
          }

          MPSGraphTensor* scaleTensor = [mpsGraph constantWithScalar:scale.to<double>()
                                                               shape:@[@1]
                                                            dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* zeroTensor = [mpsGraph constantWithScalar:0.0f
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* predicateTensor = [mpsGraph greaterThanWithPrimaryTensor:inputTensor
                                                                   secondaryTensor:zeroTensor
                                                                              name:nil];
          MPSGraphTensor* gradTensor = [mpsGraph selectWithPredicateTensor:predicateTensor
                                                       truePredicateTensor:scaleTensor
                                                      falsePredicateTensor:lessThanZeroGradTensor
                                                                      name:nil];
          MPSGraphTensor* gradInputTensor = [mpsGraph multiplicationWithPrimaryTensor:gradTensor
                                                                      secondaryTensor:gradOutputTensor
                                                                                 name:nil];

          newCachedGraph->gradOutputTensor_ = gradOutputTensor;
          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->resultTensor_ = resultTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder gradOutputPlaceholder = Placeholder(cachedGraph->gradOutputTensor_, grad_output);
    Placeholder selfPlaceholder = Placeholder();
    Placeholder resultPlaceholder = Placeholder();
    if(is_result)
      resultPlaceholder = Placeholder(cachedGraph->resultTensor_, self_or_result);
    else
      selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self_or_result);
    Placeholder gradInputPlaceholder = Placeholder(cachedGraph->gradInputTensor_, grad_input);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = nil;

    if(is_result)
      feeds = @{
        gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData(),
        resultPlaceholder.getMPSGraphTensor() : resultPlaceholder.getMPSGraphTensorData()
      };
    else
      feeds = @{
        gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData(),
        selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
      };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      gradInputPlaceholder.getMPSGraphTensor() : gradInputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

}

TORCH_IMPL_FUNC(silu_out_mps) (
  const Tensor& self,
  const Tensor& result) {

  using namespace mps;
  TORCH_CHECK(self.is_mps());

  // Empty output
  if(result.numel() == 0)
    return;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "silu_out_mps:" + getTensorsStringKey({self});

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);

          MPSGraphTensor* unitTensor = [mpsGraph constantWithScalar:1.0
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(self.scalar_type())];
          MPSGraphTensor* negativeInput = [mpsGraph negativeWithTensor:inputTensor
                                                                  name:nil];
          MPSGraphTensor* expNegativeTensor = [mpsGraph exponentWithTensor:negativeInput
                                                                      name:nil];
          MPSGraphTensor* expPlusOneTensor = [mpsGraph additionWithPrimaryTensor:expNegativeTensor
                                                                 secondaryTensor:unitTensor
                                                                            name:nil];
          MPSGraphTensor* outputTensor = [mpsGraph divisionWithPrimaryTensor:inputTensor
                                                             secondaryTensor:expPlusOneTensor
                                                                        name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, result);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

}

TORCH_IMPL_FUNC(silu_backward_out_mps) (
  const Tensor& grad_output,
  const Tensor& self,
  const Tensor& grad_input) {

  using namespace mps;
  TORCH_CHECK(grad_output.is_mps());

  // Empty output
  if(grad_input.numel() == 0)
    return;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *gradOutputTensor_ = nil;
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "silu_out_backward_mps:" + getTensorsStringKey({grad_output});

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);
          MPSGraphTensor *gradOutputTensor = mpsGraphRankedPlaceHolder(mpsGraph, grad_output);

          MPSGraphTensor* unitTensor = [mpsGraph constantWithScalar:1.0
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* negativeInput = [mpsGraph negativeWithTensor:inputTensor
                                                                  name:nil];
          MPSGraphTensor* expNegativeTensor = [mpsGraph exponentWithTensor:negativeInput
                                                                      name:nil];
          MPSGraphTensor* expPlusOneTensor = [mpsGraph additionWithPrimaryTensor:expNegativeTensor
                                                                 secondaryTensor:unitTensor
                                                                            name:nil];
          MPSGraphTensor* sigmoidTensor = [mpsGraph reciprocalWithTensor:expPlusOneTensor
                                                                    name:nil];
          MPSGraphTensor* oneMinusSigmoid = [mpsGraph subtractionWithPrimaryTensor:unitTensor
                                                                   secondaryTensor:sigmoidTensor
                                                                              name:nil];
          MPSGraphTensor* inputTimesDiff = [mpsGraph multiplicationWithPrimaryTensor:inputTensor
                                                                     secondaryTensor:oneMinusSigmoid
                                                                                name:nil];
          MPSGraphTensor* onePlusTensor = [mpsGraph additionWithPrimaryTensor:unitTensor
                                                              secondaryTensor:inputTimesDiff
                                                                         name:nil];
          MPSGraphTensor* gradTensor = [mpsGraph multiplicationWithPrimaryTensor:sigmoidTensor
                                                                 secondaryTensor:onePlusTensor
                                                                            name:nil];
          MPSGraphTensor* gradInputTensor = [mpsGraph multiplicationWithPrimaryTensor:gradTensor
                                                                      secondaryTensor:gradOutputTensor
                                                                                 name:nil];

          newCachedGraph->gradOutputTensor_ = gradOutputTensor;
          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder gradOutputPlaceholder = Placeholder(cachedGraph->gradOutputTensor_, grad_output);
    Placeholder gradInputPlaceholder = Placeholder(cachedGraph->gradInputTensor_, grad_input);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData(),
      gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      gradInputPlaceholder.getMPSGraphTensor() : gradInputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

}

// -------------------------------------------------
// Hardtanh backward

Tensor hardtanh_backward_mps
  (const Tensor& grad_output,
   const Tensor& self,
   const Scalar& min,
   const Scalar& max) {

  Tensor grad_input = at::native::empty_mps(
                      grad_output.sizes(),
                      grad_output.scalar_type(),
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);
  grad_input = hardtanh_backward_out_mps(grad_output, self, min, max, grad_input);
  return grad_input;
}

// Hardtanh backward
Tensor& hardtanh_backward_out_mps
  (const Tensor& grad_output,
   const Tensor& self,
   const Scalar& min,
   const Scalar& max,
   Tensor& grad_input) {

  using namespace mps;
  TORCH_CHECK(grad_output.is_mps());

  // Empty output
  if(grad_input.numel() == 0)
    return grad_input;

  struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *gradOutputTensor_ = nil;
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *gradInputTensor_ = nil;
  };

  MPSGraphCache* cache_ = MPSGraphCache::getInstance();

  MPSStream* stream = getCurrentMPSStream();

  @autoreleasepool {
    string key = "hardtanh_backward_out_mps:" + getTensorsStringKey({grad_output}) + ":" +
                                                 to_string(min.to<double>()) + ":" +
                                                 to_string(max.to<double>());

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* gradOutputTensor = mpsGraphRankedPlaceHolder(mpsGraph, grad_output);
          MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, self);

          // TODO: Compute gradient
          MPSGraphTensor* unitTensor = [mpsGraph constantWithScalar:1.0f
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* zeroTensor = [mpsGraph constantWithScalar:0.0f
                                                              shape:@[@1]
                                                           dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* minTensor = [mpsGraph constantWithScalar:min.to<double>()
                                                             shape:@[@1]
                                                          dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* maxTensor = [mpsGraph constantWithScalar:max.to<double>()
                                                             shape:@[@1]
                                                          dataType:getMPSDataType(grad_output.scalar_type())];
          MPSGraphTensor* greaterThanMaxPredicateTensor = [mpsGraph greaterThanWithPrimaryTensor:inputTensor
                                                                                 secondaryTensor:maxTensor
                                                                                            name:nil];
          MPSGraphTensor* lesserThanMinPredicateTensor = [mpsGraph lessThanWithPrimaryTensor:inputTensor
                                                                               secondaryTensor:minTensor
                                                                                          name:nil];
          MPSGraphTensor* greaterThanMaxGradTensor = [mpsGraph selectWithPredicateTensor:greaterThanMaxPredicateTensor
                                                                     truePredicateTensor:zeroTensor
                                                                    falsePredicateTensor:unitTensor
                                                                                    name:nil];
          MPSGraphTensor* lesserThanMinGradTensor = [mpsGraph selectWithPredicateTensor:lesserThanMinPredicateTensor
                                                                    truePredicateTensor:zeroTensor
                                                                   falsePredicateTensor:unitTensor
                                                                                   name:nil];
          MPSGraphTensor* gradTensor = [mpsGraph multiplicationWithPrimaryTensor:greaterThanMaxGradTensor
                                                                 secondaryTensor:lesserThanMinGradTensor
                                                                            name:nil];
          MPSGraphTensor* gradInputTensor = [mpsGraph multiplicationWithPrimaryTensor:gradTensor
                                                                      secondaryTensor:gradOutputTensor
                                                                                 name:nil];

          newCachedGraph->gradOutputTensor_ = gradOutputTensor;
          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder gradOutputPlaceholder = Placeholder(cachedGraph->gradOutputTensor_, grad_output);
    Placeholder selfPlaceholder = Placeholder(cachedGraph->inputTensor_, self);
    Placeholder gradInputPlaceholder = Placeholder(cachedGraph->gradInputTensor_, grad_input);

    // Create dictionary of inputs and outputs
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData(),
      selfPlaceholder.getMPSGraphTensor() : selfPlaceholder.getMPSGraphTensorData()
    };

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      gradInputPlaceholder.getMPSGraphTensor() : gradInputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

  return grad_input;
}

} // namespace native
} // namespace at
