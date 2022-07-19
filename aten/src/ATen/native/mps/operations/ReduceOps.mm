//  Copyright © 2022 Apple Inc.

#include <ATen/ATen.h>
#include <ATen/Tensor.h>
#include <ATen/Utils.h>
#include <ATen/TensorUtils.h>
#include <ATen/mps/MPSStream.h>
#include <ATen/native/mps/OperationUtils.h>
#include <ATen/native/ReduceOpsUtils.h>
#include <ATen/native/Pool.h>
#include <torch/library.h>

namespace at {
namespace native {

using namespace std;

enum StdVarType {
  STANDARD_VARIANCE,
  STANDARD_DEVIATION
};

void set_apparent_shapes(NSMutableArray<NSNumber*> * &apparent_out_shape,
                         NSMutableArray<NSNumber*> * &apparent_in_shape,
                         int64_t num_reduce_dims,
                         int64_t num_input_dims,
                         int64_t num_output_dims,
                         IntArrayRef& input_shape,
                         NSMutableArray<NSNumber*> * &axes) {

  if(num_reduce_dims == 0) {
    /* Output shape becomes a one
     * Input shape becomes flattened
     * Because 0 reduce dims means all dims are reduced
     */
    apparent_in_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
    int64_t num_in_elements = 1;
    for(int i = 0; i < num_input_dims; i++) {
        num_in_elements *= input_shape[i];
    }
    apparent_in_shape[0] = [NSNumber numberWithInt:num_in_elements];

    apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
    apparent_out_shape[0] = @1;
  }

  else {
    // num_output_dims in this case is number of input dims
    apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:num_output_dims];
    for(int i = 0; i < num_output_dims; i++) {
      int64_t current_input_dim = input_shape[i];

      // If the current dim is to be reduced
      bool is_reduce_dim = false;

      for(int j = 0; j < num_reduce_dims; j++) {
        if(i == [axes[j] intValue]) {
          is_reduce_dim = true;
          break;
        }
      }

      if(is_reduce_dim) {
        apparent_out_shape[i] = @1;
      }
      else {
        apparent_out_shape[i] = [NSNumber numberWithInt:current_input_dim];
      }
    }
  }

}

// Helper function to set the axes of reduction
void set_axes(NSMutableArray<NSNumber *> * &axes,
              int64_t num_reduce_dims,
              IntArrayRef& dim,
              int64_t num_input_dims) {
    if(num_reduce_dims == 0) {
      axes = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
      axes[0] = @0;
    }
    else {
      axes = [NSMutableArray<NSNumber*> arrayWithCapacity:num_reduce_dims];
      for(int i = 0; i < num_reduce_dims; i++) {
        axes[i] = [NSNumber numberWithInt:maybe_wrap_dim(dim[i], num_input_dims)];
      }
    }
}

// Helper function to prepare axes and tensor shapes
void set_axes_and_shapes(const Tensor& input_t,
                         IntArrayRef dims,
                         NSMutableArray<NSNumber*> * &axes,
                         NSMutableArray<NSNumber*> * &apparent_input_shape,
                         NSMutableArray<NSNumber*> * &apparent_output_shape,
                         NSMutableArray<NSNumber*> * &output_shape) {

  IntArrayRef input_shape = input_t.sizes();

  int64_t num_input_dims = input_shape.size();
  int64_t num_reduce_dims = dims.size();
  int64_t num_output_dims;

  num_output_dims = num_reduce_dims == 0 ? 1 : num_input_dims;

  // Reduction axes
  set_axes(axes, num_reduce_dims, dims, input_shape.size());

  // Shapes
  set_apparent_shapes(apparent_output_shape,
                      apparent_input_shape,
                      num_reduce_dims,
                      num_input_dims,
                      num_output_dims,
                      input_shape,
                      axes);

  // Squeeze dims for output shape
  output_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:0];
  for(int i=0; i < num_output_dims; i++) {
    if([apparent_output_shape[i] longValue] != 1) {
      [output_shape addObject:apparent_output_shape[i]];
    }
  }
}

void reduction_out_mps
   (const Tensor& input_t,
    IntArrayRef dim,
    bool keepdim,
    c10::optional<ScalarType> dtype,
    const Tensor& output_t,
    string reduction_type,
    string func_name) {

  IntArrayRef input_shape = input_t.sizes();

  for(int i = 0; i < dim.size(); i++) {
    auto wrap_dim = maybe_wrap_dim(dim[i], input_shape.size());
    TORCH_CHECK(wrap_dim < input_shape.size(),
    func_name+": reduction dim must be in the range of input shape")
  }

  namespace native_mps = at::native::mps;

  NSMutableArray<NSNumber*> *axes = nil;
  NSMutableArray<NSNumber*> *apparent_input_shape = nil;
  NSMutableArray<NSNumber*> *apparent_output_shape = nil;
  NSMutableArray<NSNumber*> *output_shape = nil;

  set_axes_and_shapes(input_t, dim, axes, apparent_input_shape, apparent_output_shape, output_shape);

  // Derive from MPSCachedGraph
  struct CachedGraph : public native_mps::MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

  if (output_t.numel() == 0 || input_t.numel() == 0) {
    return;
  }

  auto stream = at::mps::getCurrentMPSStream();

  @autoreleasepool {

    // TODO: Make this key proper
    NSString* ns_key = [[axes valueForKey:@"description"] componentsJoinedByString:@","];
    string key =  func_name+":" + string([ns_key UTF8String]) + ":" + native_mps::getMPSTypeString(input_t.scalar_type()) + ":" + native_mps::getMPSTypeString(output_t.scalar_type());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

    if(!cachedGraph) {
      native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = native_mps::make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(input_t.scalar_type()));

          MPSGraphTensor* castInputTensor = nil;

          if(input_t.scalar_type() != ScalarType::Float && input_t.scalar_type() != ScalarType::Int)
            castInputTensor =  [mpsGraph castTensor:inputTensor
                                             toType:MPSDataTypeFloat32
                                               name:@"castInputTensor"];
          else
            castInputTensor = inputTensor;

          MPSGraphTensor* castOutputTensor = nil;

          if(reduction_type == "sum") {
            castOutputTensor = [mpsGraph reductionSumWithTensor:castInputTensor
                                                           axes:axes
                                                           name:nil];
          } else if(reduction_type == "prod") {
            castOutputTensor = [mpsGraph reductionProductWithTensor:castInputTensor
                                                               axes:axes
                                                               name:nil];
          } else if(reduction_type == "mean") {
            castOutputTensor = [mpsGraph meanOfTensor:inputTensor
                                                 axes:axes
                                                 name:nil];
          } else if(reduction_type == "count_nonzero") {
            MPSGraphTensor* zeros = [mpsGraph constantWithScalar:0
                                                        dataType:castInputTensor.dataType];

            MPSGraphTensor* nonZeros = [mpsGraph notEqualWithPrimaryTensor:castInputTensor
                                                           secondaryTensor:zeros
                                                                      name:nil];

            castOutputTensor = [mpsGraph reductionSumWithTensor:nonZeros
                                                           axes:axes
                                                           name:nil];
          }

          MPSGraphTensor* outputTensor = nil;

          if(output_t.scalar_type() != ScalarType::Float)
            outputTensor = [mpsGraph castTensor:castOutputTensor
                                         toType:(native_mps::getMPSDataType(output_t.scalar_type()))
                                           name:@"outputTensor"];
          else
            outputTensor = castOutputTensor;

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    auto inputPlaceholder = native_mps::Placeholder();

    if(apparent_input_shape)
      inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t, apparent_input_shape);
    else
      inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);
    auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, apparent_output_shape);
    NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
      inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
    };

    NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };
    native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }

}

TORCH_IMPL_FUNC(sum_out_mps)
   (const Tensor& input_t,
    IntArrayRef dim,
    bool keepdim,
    c10::optional<ScalarType> dtype,
    const Tensor& output_t) {

    reduction_out_mps(input_t, dim, keepdim, dtype, output_t, "sum", "sum_out_mps");
}

TORCH_IMPL_FUNC(prod_out_mps)
   (const Tensor& input_t,
    int64_t dim,
    bool keepdim,
    c10::optional<ScalarType> dtype,
    const Tensor& output_t) {

    int64_t dims[1] = {dim};

    reduction_out_mps(input_t, IntArrayRef(dims, 1), keepdim, dtype, output_t, "prod", "prod_out_mps");
}

// Taken from ReduceOps.cpp
inline ScalarType get_dtype_from_self(
    const Tensor& self,
    const optional<ScalarType>& dtype,
    bool promote_integers) {
  if (dtype.has_value()) {
    return dtype.value();
  }
  ScalarType src_type = self.scalar_type();
  if (promote_integers && at::isIntegralType(src_type, /*includeBool=*/true)) {
    return kLong;
  }
  return src_type;
}

Tensor prod_mps(const Tensor &self, c10::optional<ScalarType> opt_dtype) {

  auto num_dims = self.dim();

  int64_t dims[num_dims];

  for(int i = 0; i < num_dims; i++)
    dims[i] = i;

  Tensor output_t = at::native::empty_mps(
                      {},
                      get_dtype_from_self(self, opt_dtype, true),
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);

  reduction_out_mps(self, IntArrayRef(dims, num_dims), false, opt_dtype, const_cast<Tensor&>(output_t), "prod", "prod_mps");

  return output_t;
}


Tensor count_nonzero_mps(const Tensor& self, IntArrayRef dims){
  NSMutableArray<NSNumber*> *axes = nil;
  NSMutableArray<NSNumber*> *apparent_input_shape = nil;
  NSMutableArray<NSNumber*> *apparent_output_shape = nil;
  NSMutableArray<NSNumber*> *output_shape = nil;

  set_axes_and_shapes(self, dims, axes, apparent_input_shape, apparent_output_shape, output_shape);

  int64_t* raw_output_shape = (int64_t *)malloc([output_shape count] * sizeof(int64_t));
  for(int i=0; i < [output_shape count]; i++) {
    raw_output_shape[i] = [output_shape[i] longValue];
  }

  Tensor output_t = at::native::empty_mps(
                      IntArrayRef(raw_output_shape, [output_shape count]),
                      ScalarType::Long,
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);

  reduction_out_mps(self, dims, false, self.scalar_type(), const_cast<Tensor&>(output_t), "count_nonzero", "count_nonzero_mps");

  free(raw_output_shape);

  return output_t;
}

TORCH_IMPL_FUNC(mean_out_mps)
   (const Tensor& input_t,
    IntArrayRef dim,
    bool keepdim,
    c10::optional<ScalarType> dtype,
    const Tensor& output_t) {

    reduction_out_mps(input_t, dim, keepdim, dtype, output_t, "mean", "mean_out_mps");
}

TORCH_IMPL_FUNC(argmax_out_mps)
   (const Tensor& input_t,
    c10::optional<int64_t> dim,
    bool keepdim,
    const Tensor& output_t) {

    namespace native_mps = at::native::mps;

    // Derive from MPSCachedGraph
    struct CachedGraph : public native_mps::MPSCachedGraph
    {
      CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
      MPSGraphTensor *inputTensor_ = nil;
      MPSGraphTensor *outputTensor_ = nil;
    };

    native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

    int64_t dim_;

    if (dim.has_value()) {
        dim_ = maybe_wrap_dim(dim.value(), input_t.dim());
        native::zero_numel_check_dims(input_t, dim_, "argmax()");
    } else {
        TORCH_CHECK_INDEX(
        input_t.numel() != 0,
        "argmax()", ": Expected reduction dim to be specified for input.numel() == 0.");
        // Since input will be flattened, take argmax along 0'th dimension
        dim_ = 0;
    }

    // Calculate the output shape according to keepdim=True
    // If there is no dim argument, the input shape is flattened
    IntArrayRef input_shape = input_t.sizes();
    int64_t num_input_dims = input_shape.size();
    NSMutableArray<NSNumber*> *apparent_in_shape = nil;
    NSMutableArray<NSNumber*> *apparent_out_shape = nil;

    if(dim.has_value()) {
        apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:num_input_dims];
        for(int i = 0; i < num_input_dims; i++) {
            if(dim_ == i)
                apparent_out_shape[i] = @1;
            else
                apparent_out_shape[i] = [NSNumber numberWithInt:input_shape[i]];
        }
    }
    else {
        apparent_in_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
        int64_t num_in_elements = 1;
        for(int i = 0; i < num_input_dims; i++) {
            num_in_elements *= input_shape[i];
        }
        apparent_in_shape[0] = [NSNumber numberWithInt:num_in_elements];

        apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
        apparent_out_shape[0] = @1;
    }

    if (output_t.numel() == 0) {
        return;
    }

    auto stream = at::mps::getCurrentMPSStream();

    @autoreleasepool {
        string key = "argmax_out_mps:" + to_string(dim_) + ":" + native_mps::getMPSTypeString(input_t.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
          native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

            CachedGraph *newCachedGraph = nil;

            @autoreleasepool {
              MPSGraph* mpsGraph = native_mps::make_mps_graph();
              newCachedGraph = new CachedGraph(mpsGraph);

              MPSGraphTensor* inputTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(input_t.scalar_type()));

              MPSGraphTensor* castInputTensor = nil;

              if(input_t.scalar_type() != ScalarType::Float &&
                 input_t.scalar_type() != ScalarType::Int   &&
                 input_t.scalar_type() != ScalarType::Half)
                castInputTensor =  [mpsGraph castTensor:inputTensor
                                                 toType:MPSDataTypeFloat32
                                                   name:@"castInputTensor"];
              else
                castInputTensor = inputTensor;

              MPSGraphTensor* argmaxOutTensor = [mpsGraph reductionArgMaximumWithTensor:castInputTensor
                                                                                   axis:(NSInteger)dim_
                                                                                   name:@"argmax_out"];
              MPSGraphTensor* outputTensor = [mpsGraph castTensor:argmaxOutTensor
                                                           toType:MPSDataTypeInt64
                                                             name:@"cast_out"];

              newCachedGraph->inputTensor_ = inputTensor;
              newCachedGraph->outputTensor_ = outputTensor;
            }
            return newCachedGraph;
          });
          cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        native_mps::Placeholder inputPlaceholder = native_mps::Placeholder();
        if(apparent_in_shape)
            inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t, apparent_in_shape);
        else
            inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);

        auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, apparent_out_shape);

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
          inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
        };

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
          outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
        };

        native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);

    }

}

TORCH_IMPL_FUNC(norm_out_mps)
(const Tensor& input_t,
 const OptionalScalarRef opt_p,
 IntArrayRef dim,
 bool keepdim,
 const Tensor& output_t)
{
  if (input_t.numel() == 0)
    return;
  IntArrayRef input_shape = input_t.sizes();

  for(int i = 0; i < dim.size(); i++) {
    auto wrap_dim = maybe_wrap_dim(dim[i], input_shape.size());
    TORCH_CHECK(wrap_dim < input_shape.size(),
    "norm_out_mps: reduction dim must be in the range of input shape")
  }
  namespace native_mps = at::native::mps;
  CheckedFrom c = "norm_out_mps";

  // Derive from MPSCachedGraph
  struct CachedGraph : public native_mps::MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

  auto p = opt_p.has_value() ? opt_p.get().to<double>() : Scalar(2.0).to<double>();
  auto reciprocal_p = 1 / p;
  bool pIsZero = (p == 0.0);
  bool pIsPosInf = (p == numeric_limits<double>::infinity());
  bool pIsNegInf = (p == -numeric_limits<double>::infinity());

  int64_t num_input_dims = input_shape.size();
  int64_t num_reduce_dims = dim.size();
  int64_t num_output_dims;

  // For output shape calculation, assume that keepdim is true
  num_output_dims = num_input_dims;
  NSMutableArray<NSNumber*> *apparent_output_shape = nil;
  NSMutableArray<NSNumber*> *apparent_input_shape = nil;

  // Reduction axes
  NSMutableArray<NSNumber *> *axes;
  set_axes(axes, num_reduce_dims, dim, input_shape.size());

  set_apparent_shapes(apparent_output_shape,
                      apparent_input_shape,
                      num_reduce_dims,
                      num_input_dims,
                      num_output_dims,
                      input_shape,
                      axes);
  if (output_t.numel() == 0) {
    return;
  }

  auto stream = at::mps::getCurrentMPSStream();

  @autoreleasepool {
    NSString* ns_key = [[axes valueForKey:@"description"] componentsJoinedByString:@","];
      string keepdim_info = (keepdim) ? "keepdim=1" : "keepdim=0";
      string key =  string("norm_out_mps:") + [ns_key UTF8String] + ":" + native_mps::getMPSTypeString(input_t.scalar_type()) + ":p" + to_string(p) + ":" + keepdim_info;

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

    if(!cachedGraph) {
      native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = native_mps::make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(input_t.scalar_type()));

          MPSGraphTensor *outputTensor;

          if (pIsZero)
          {
              MPSGraphTensor *absoluteTensor = [mpsGraph absoluteWithTensor:inputTensor
                                                                       name:nil];
              MPSGraphTensor *powerValTensor = [mpsGraph constantWithScalar:p
                                                                   dataType:native_mps::getMPSDataType(input_t.scalar_type())];
              MPSGraphTensor *powerTensor = [mpsGraph powerWithPrimaryTensor:absoluteTensor
                                                             secondaryTensor:powerValTensor
                                                                        name:nil];
              outputTensor = [mpsGraph reductionSumWithTensor:powerTensor
                                                         axes:axes
                                                         name:nil];
          }
          else if (pIsPosInf)
          {
              MPSGraphTensor *absoluteTensor = [mpsGraph absoluteWithTensor:inputTensor
                                                                       name:nil];
              outputTensor = [mpsGraph reductionMaximumWithTensor:absoluteTensor
                                                             axes:axes
                                                             name:nil];
          }
          else if (pIsNegInf)
          {
              MPSGraphTensor *absoluteTensor = [mpsGraph absoluteWithTensor:inputTensor
                                                                       name:nil];
              outputTensor = [mpsGraph reductionMinimumWithTensor:absoluteTensor
                                                             axes:axes
                                                             name:nil];
          }
          else
          {
              MPSGraphTensor *absoluteTensor = [mpsGraph absoluteWithTensor:inputTensor
                                                                       name:nil];

              MPSGraphTensor *powerValTensor = [mpsGraph constantWithScalar:p
                                                                   dataType:native_mps::getMPSDataType(input_t.scalar_type())];

              MPSGraphTensor *reciprocalPowerValTensor = [mpsGraph constantWithScalar:reciprocal_p
                                                                             dataType:native_mps::getMPSDataType(input_t.scalar_type())];

              MPSGraphTensor *powerTensor = [mpsGraph powerWithPrimaryTensor:absoluteTensor
                                                             secondaryTensor:powerValTensor
                                                                        name:nil];

              MPSGraphTensor *reductionSumTensor = [mpsGraph reductionSumWithTensor:powerTensor
                                                                         axes:axes
                                                                         name:nil];

              outputTensor = [mpsGraph powerWithPrimaryTensor:reductionSumTensor
                                              secondaryTensor:reciprocalPowerValTensor
                                                         name:nil];
          }

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;
        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    auto inputPlaceholder = native_mps::Placeholder();

    if(apparent_input_shape)
      inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t, apparent_input_shape);
    else
      inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);

    auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, apparent_output_shape);


    NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
      inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
    };

    NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);

  }
}

Tensor std_var_common_impl_mps(
  const Tensor & input_t,
  at::OptionalIntArrayRef dim,
  c10::optional<int64_t> correction,
  bool keepdim,
  StdVarType stdVarType)
{
  namespace native_mps = at::native::mps;

  IntArrayRef input_shape = input_t.sizes();
  int64_t num_input_dims = input_shape.size();

  bool use_dim = dim.has_value();
  IntArrayRef dim_value = use_dim ? dim.value() : NULL;

  if (use_dim)
  {
      string errMessage = (stdVarType == STANDARD_DEVIATION) ? "std_mps" : "var_mps";
      errMessage += ": reduction dim must be in the range of input shape";
      for(int i = 0; i < dim_value.size(); i++) {
        auto wrap_dim = maybe_wrap_dim(dim_value[i], input_shape.size());
        TORCH_CHECK(wrap_dim < input_shape.size(), errMessage.c_str())
    }
  }

  bool use_correction = correction.has_value();
  const auto correction_value = use_correction ? correction.value() : false;
  int64_t correction_n = 1;


  // Derive from MPSCachedGraph
  struct CachedGraph : public native_mps::MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

  int64_t num_output_dims = 0;
  NSMutableArray<NSNumber *> *axes = nil;
  NSMutableArray<NSNumber*> *apparent_output_shape = nil;
  NSMutableArray<NSNumber*> *apparent_input_shape = nil;
  int64_t* output_shape = nil;

  if ((!keepdim && !use_dim) || (!keepdim && use_dim && dim_value.size() <= 0))
  {
      // Flatten the input tensor to reduce it to one value
      apparent_input_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
      int64_t num_in_elements = 1;
      for(int i = 0; i < num_input_dims; i++) {
          num_in_elements *= input_shape[i];
      }
      apparent_input_shape[0] = [NSNumber numberWithInt:num_in_elements];

      // Output is a single value
      apparent_output_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
      apparent_output_shape[0] = @1;

      num_output_dims = 0;

      correction_n = num_in_elements;

        // Reduction axes
      axes = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
      axes[0] = @0;

  }
  else if (!keepdim && use_dim && dim_value.size() > 0)
  {
      int64_t num_reduce_dims = dim_value.size();
      num_output_dims = num_input_dims;

      set_axes(axes, num_reduce_dims, dim_value, num_input_dims);
      set_apparent_shapes(apparent_output_shape,
                           apparent_input_shape,
                           num_reduce_dims,
                           num_input_dims,
                           num_output_dims,
                           input_shape,
                           axes);

      num_output_dims = (num_input_dims >= num_reduce_dims) ? (num_input_dims - num_reduce_dims) : 0; //num_input_dims;
      output_shape = (int64_t *)malloc(num_output_dims  * sizeof(int64_t));

      unsigned int curr_i = 0;
      for (int i = 0; i < num_input_dims; i++)
      {
          bool found = false;
          for (int j = 0; j < num_reduce_dims; j++)
          {
              if (i == dim_value[j])
              {
                  found = true;
                  break;
              }
          }
          if (found) continue;
          output_shape[curr_i] = input_shape[i];
          curr_i += 1;
      }

      for(int i = 0; i < num_reduce_dims; i++)
      {
          correction_n *= input_shape[dim_value[i]];
      }
      // (3, 4, 5) --> (3, 5)
  }
  else if ((keepdim && !use_dim) || (keepdim && use_dim && dim_value.size() <= 0))
  {
      num_output_dims = 0;
      int64_t num_reduce_dims = 0;
      set_axes(axes, num_reduce_dims, dim_value, input_shape.size());
      set_apparent_shapes(apparent_output_shape,
                          apparent_input_shape,
                           num_reduce_dims,
                           num_input_dims,
                           num_output_dims,
                           input_shape,
                           axes);
      num_output_dims = num_input_dims;
      output_shape = (int64_t *)malloc(num_output_dims  * sizeof(int64_t));
      for (int i = 0; i < num_input_dims; i++)
      {
          output_shape[i] = (int64_t) 1;
          correction_n *= input_shape[i];
      }
      // scalar --> vector case [[1.0034567]]
  }
  else if (keepdim && use_dim && dim_value.size() > 0)
  {
      int64_t num_reduce_dims = dim_value.size();
      num_output_dims = num_input_dims;

      set_axes(axes, num_reduce_dims, dim_value, num_input_dims);
      set_apparent_shapes(apparent_output_shape,
                           apparent_input_shape,
                           num_reduce_dims,
                           num_input_dims,
                           num_output_dims,
                           input_shape,
                           axes);

      num_output_dims = num_input_dims;//(num_input_dims >= num_reduce_dims) ? (num_input_dims - num_reduce_dims) : 0;
      output_shape = (int64_t *)malloc(num_output_dims  * sizeof(int64_t));

      for(int i = 0; i < num_reduce_dims; i++)
      {
          correction_n *= input_shape[dim_value[i]];
      }

      for (int i = 0; i < num_input_dims; i++)
      {
          output_shape[i] = [apparent_output_shape[i] longValue];
      }
  }

  Tensor output_t = at::native::empty_mps(
                      IntArrayRef(output_shape, num_output_dims),
                      input_t.scalar_type(),
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);

  if (output_t.numel() == 0 || input_t.numel() == 0)
  {
     return output_t;
  }

  double bessel_correction = ((double) correction_n) / ((double) (correction_n-1));

  auto stream = at::mps::getCurrentMPSStream();

  @autoreleasepool {
    string op_key = (stdVarType == STANDARD_DEVIATION) ? "std_mps" : "var_mps";
    NSString* ns_key = [[axes valueForKey:@"description"] componentsJoinedByString:@","];
    string bessel_corrected = (use_correction && correction_value) ? "unbiased " : "biased ";
    string use_dim_info = (use_dim) ? "use_dim=1:" + to_string(dim_value.size()) : "use_dim=0";
    string keepdim_info = (keepdim) ? "keepdim=1" : "keepdim=0";
    string key = op_key + use_dim_info + ":" + keepdim_info + ":" + string([ns_key UTF8String]) + ":" + native_mps::getMPSTypeString(input_t.scalar_type()) + ":" + bessel_corrected;

    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    // Initialize once if configuration not found in cache
  if(!cachedGraph) {
      native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

      CachedGraph *newCachedGraph = nil;

      @autoreleasepool {
          MPSGraph* mpsGraph = native_mps::make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(input_t.scalar_type()));
          MPSGraphTensor *outputVarTensor = [mpsGraph varianceOfTensor:inputTensor
                                                                     axes:axes
                                                                     name:nil];
          MPSGraphTensor *outputTensor;

          if (use_correction && correction_value)
          {
              MPSGraphTensor *besselTensor= [mpsGraph constantWithScalar:bessel_correction
                                                    dataType:MPSDataTypeFloat32];
              MPSGraphTensor *correctedTensor = [mpsGraph multiplicationWithPrimaryTensor: outputVarTensor
                                                                          secondaryTensor: besselTensor
                                                                                     name: nil];
              outputTensor = (stdVarType == STANDARD_DEVIATION) ?
                    [mpsGraph squareRootWithTensor:correctedTensor name:nil] : correctedTensor;
          }
          else
          {
              outputTensor = (stdVarType == STANDARD_DEVIATION) ?
                    [mpsGraph squareRootWithTensor:outputVarTensor name:nil] : outputVarTensor;
          }
          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;

      }
      return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
  }
  auto inputPlaceholder = native_mps::Placeholder();

  if(apparent_input_shape)
  {
    inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t, apparent_input_shape);
  }
  else
  {
    inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);
  }
  auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, apparent_output_shape);

  NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
      inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
  };

  NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
  };
  native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }
  free(output_shape);
  return output_t;
}

Tensor var_mps(
  const Tensor & input_t,
  at::OptionalIntArrayRef dim,
  c10::optional<int64_t> correction,
  bool keepdim)
{
  return std_var_common_impl_mps(input_t, dim, correction, keepdim, STANDARD_VARIANCE);
}

Tensor std_mps(
   const Tensor & input_t,
   at::OptionalIntArrayRef dim,
   c10::optional<int64_t> correction,
   bool keepdim)
{
  return std_var_common_impl_mps(input_t, dim, correction, keepdim, STANDARD_DEVIATION);
}

TORCH_IMPL_FUNC(any_out_mps)
  (const Tensor& input_t,
   int64_t dim,
   bool keepdim,
   const Tensor& output_t)
{
    namespace native_mps = at::native::mps;

    if (output_t.numel() == 0 || input_t.numel() == 0) {
      return;
    }

    // Derive from MPSCachedGraph
    struct CachedGraph : public native_mps::MPSCachedGraph
    {
      CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
      MPSGraphTensor *inputTensor_ = nil;
      MPSGraphTensor *outputTensor_ = nil;
    };

    native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();
    int64_t dim_ = maybe_wrap_dim(dim, input_t.dim());
    native::zero_numel_check_dims(input_t, dim_, "any()");

    // Calculate the output shape according to keepdim=True
    // If there is no dim argument, the input shape is flattened
    IntArrayRef input_shape = input_t.sizes();
    int64_t num_input_dims = input_shape.size();
    NSMutableArray<NSNumber*> *apparent_out_shape = nil;
    apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:num_input_dims];
    for(int i = 0; i < num_input_dims; i++) {
        if(dim_ == i)
            apparent_out_shape[i] = @1;
        else
            apparent_out_shape[i] = [NSNumber numberWithInt:input_shape[i]];
    }

    auto stream = at::mps::getCurrentMPSStream();

    @autoreleasepool {
        MPSShape* input_t_shape = native_mps::getMPSShape(input_t);
        string key = string("any_out_mps:") + native_mps::getMPSShapeString(input_t_shape) + ":" + to_string(dim_) + ":" + native_mps::getMPSTypeString(input_t.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
          native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

            CachedGraph *newCachedGraph = nil;
            @autoreleasepool {
              MPSGraph* mpsGraph = native_mps::make_mps_graph();
              newCachedGraph = new CachedGraph(mpsGraph);

                MPSGraphTensor* outputTensor;
                MPSDataType input_type = native_mps::getMPSDataType(input_t.scalar_type());
                MPSGraphTensor* inputTensor = native_mps::mpsGraphRankedPlaceHolder(mpsGraph, input_type, input_t_shape);

                if (input_type != MPSDataTypeInt32 &&
                    input_type != MPSDataTypeFloat32 &&
                    input_type != MPSDataTypeFloat16 )
                {
                    MPSGraphTensor* inputCastedTensor = [mpsGraph castTensor:inputTensor
                                                                      toType:MPSDataTypeInt32
                                                                        name:@"any_all"];
                    MPSGraphTensor* outputCastedTensor = [mpsGraph reductionOrWithTensor:inputCastedTensor
                                                                                     axis:dim_
                                                                                     name:nil];
                    outputTensor = [mpsGraph castTensor:outputCastedTensor
                                                 toType:MPSDataTypeBool
                                                   name:@"any"];
                }
                else
                {
                    MPSGraphTensor* outputUncastedTensor = [mpsGraph reductionOrWithTensor:inputTensor
                                                                                       axis:dim_
                                                                                       name:nil];
                    outputTensor = [mpsGraph castTensor:outputUncastedTensor
                                                 toType:MPSDataTypeBool
                                                   name:@"any"];
                }
                newCachedGraph->inputTensor_ = inputTensor;
                newCachedGraph->outputTensor_ = outputTensor;

            }
            return newCachedGraph;
          });
          cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        auto inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);
        auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, apparent_out_shape);
        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
          inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
        };

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
          outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData(),
        };

        native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);
}
}

TORCH_IMPL_FUNC(any_all_out_mps)(const Tensor& input_t, const Tensor& output_t)
{
    namespace native_mps = at::native::mps;
    if (output_t.numel() == 0 || input_t.numel() == 0) {
      return;
    }

    // Derive from MPSCachedGraph
    struct CachedGraph : public native_mps::MPSCachedGraph
    {
      CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
      MPSGraphTensor *inputTensor_ = nil;
      MPSGraphTensor *outputTensor_ = nil;
    };

    native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

    auto stream = at::mps::getCurrentMPSStream();

    @autoreleasepool {
        MPSShape* input_t_shape = native_mps::getMPSShape(input_t);
        string key = string("any_all_out_mps:") + native_mps::getMPSShapeString(input_t_shape) +":" + native_mps::getMPSTypeString(input_t.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
          native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

            CachedGraph *newCachedGraph = nil;

            @autoreleasepool {
              MPSGraph* mpsGraph = native_mps::make_mps_graph();
              newCachedGraph = new CachedGraph(mpsGraph);

              MPSGraphTensor* outputTensor;
              MPSDataType input_type = native_mps::getMPSDataType(input_t.scalar_type());
              MPSGraphTensor* inputTensor = native_mps::mpsGraphRankedPlaceHolder(mpsGraph, input_type, input_t_shape);

              if (input_type != MPSDataTypeInt32 &&
                  input_type != MPSDataTypeFloat32 &&
                  input_type != MPSDataTypeFloat16 )
              {
                  MPSGraphTensor* inputCastedTensor = [mpsGraph castTensor:inputTensor
                                                                    toType:MPSDataTypeInt32
                                                                      name:@"any_all"];
                  MPSGraphTensor* outputCastedTensor = [mpsGraph reductionOrWithTensor:inputCastedTensor
                                                                                   axes:nil
                                                                                   name:nil];
                  outputTensor = [mpsGraph castTensor:outputCastedTensor
                                               toType:MPSDataTypeBool
                                                 name:@"any_all"];
              }
              else
              {
                  MPSGraphTensor* outputUncastedTensor = [mpsGraph reductionOrWithTensor:inputTensor
                                                                                     axes:nil
                                                                                     name:nil];
                  outputTensor = [mpsGraph castTensor:outputUncastedTensor
                                               toType:MPSDataTypeBool
                                                 name:@"any_all"];
              }
              newCachedGraph->inputTensor_ = inputTensor;
              newCachedGraph->outputTensor_ = outputTensor;

            }
            return newCachedGraph;
          });
          cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        auto inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);
        auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t);
        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
          inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
        };

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
          outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData(),
        };

        native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);
    }
}

TORCH_IMPL_FUNC(all_out_mps)
  (const Tensor& input_t,
   int64_t dim,
   bool keepdim,
   const Tensor& output_t)
{
    namespace native_mps = at::native::mps;

    if (output_t.numel() == 0 || input_t.numel() == 0) {
      return;
    }

    // Derive from MPSCachedGraph
    struct CachedGraph : public native_mps::MPSCachedGraph
    {
      CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
      MPSGraphTensor *inputTensor_ = nil;
      MPSGraphTensor *outputTensor_ = nil;
    };

    native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();
    int64_t dim_ = maybe_wrap_dim(dim, input_t.dim());
    native::zero_numel_check_dims(input_t, dim_, "all()");

    // Calculate the output shape according to keepdim=True
    // If there is no dim argument, the input shape is flattened
    IntArrayRef input_shape = input_t.sizes();
    int64_t num_input_dims = input_shape.size();
    NSMutableArray<NSNumber*> *apparent_out_shape = nil;
    apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:num_input_dims];
    for(int i = 0; i < num_input_dims; i++) {
        if(dim_ == i)
            apparent_out_shape[i] = @1;
        else
            apparent_out_shape[i] = [NSNumber numberWithInt:input_shape[i]];
    }

    auto stream = at::mps::getCurrentMPSStream();

    @autoreleasepool {
        MPSShape* input_t_shape = native_mps::getMPSShape(input_t);
        string key = string("all_out_mps:") + native_mps::getMPSShapeString(input_t_shape) + ":" + to_string(dim_) + ":" + native_mps::getMPSTypeString(input_t.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
          native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

            CachedGraph *newCachedGraph = nil;
            @autoreleasepool {
              MPSGraph* mpsGraph = native_mps::make_mps_graph();
              newCachedGraph = new CachedGraph(mpsGraph);

                MPSGraphTensor* outputTensor;
                MPSDataType input_type = native_mps::getMPSDataType(input_t.scalar_type());
                MPSGraphTensor* inputTensor = native_mps::mpsGraphRankedPlaceHolder(mpsGraph, input_type, input_t_shape);

                if (input_type != MPSDataTypeInt32 &&
                    input_type != MPSDataTypeFloat32 &&
                    input_type != MPSDataTypeFloat16 )
                {
                    MPSGraphTensor* inputCastedTensor = [mpsGraph castTensor:inputTensor
                                                                      toType:MPSDataTypeInt32
                                                                        name:@"all_all"];
                    MPSGraphTensor* outputCastedTensor = [mpsGraph reductionAndWithTensor:inputCastedTensor
                                                                                     axis:dim_
                                                                                     name:nil];
                    outputTensor = [mpsGraph castTensor:outputCastedTensor
                                                 toType:MPSDataTypeBool
                                                   name:@"all"];
                }
                else
                {
                    MPSGraphTensor* outputUncastedTensor = [mpsGraph reductionAndWithTensor:inputTensor
                                                                                       axis:dim_
                                                                                       name:nil];
                    outputTensor = [mpsGraph castTensor:outputUncastedTensor
                                                 toType:MPSDataTypeBool
                                                   name:@"all"];
                }
                newCachedGraph->inputTensor_ = inputTensor;
                newCachedGraph->outputTensor_ = outputTensor;

            }
            return newCachedGraph;
          });
          cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        auto inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);
        auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, apparent_out_shape);
        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
          inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
        };

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
          outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData(),
        };

        native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }
}

TORCH_IMPL_FUNC(all_all_out_mps)(const Tensor& input_t, const Tensor& output_t)
{
    namespace native_mps = at::native::mps;
    if (output_t.numel() == 0 || input_t.numel() == 0) {
      return;
    }

    // Derive from MPSCachedGraph
    struct CachedGraph : public native_mps::MPSCachedGraph
    {
      CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
      MPSGraphTensor *inputTensor_ = nil;
      MPSGraphTensor *outputTensor_ = nil;
    };

    native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

    auto stream = at::mps::getCurrentMPSStream();

    @autoreleasepool {
        MPSShape* input_t_shape = native_mps::getMPSShape(input_t);
        string key = string("all_all_out_mps:") + native_mps::getMPSShapeString(input_t_shape) +":" + native_mps::getMPSTypeString(input_t.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
          native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

            CachedGraph *newCachedGraph = nil;

            @autoreleasepool {
              MPSGraph* mpsGraph = native_mps::make_mps_graph();
              newCachedGraph = new CachedGraph(mpsGraph);

              MPSGraphTensor* outputTensor;
              MPSDataType input_type = native_mps::getMPSDataType(input_t.scalar_type());
              MPSGraphTensor* inputTensor = native_mps::mpsGraphRankedPlaceHolder(mpsGraph, input_type, input_t_shape);

              if (input_type != MPSDataTypeInt32 &&
                  input_type != MPSDataTypeFloat32 &&
                  input_type != MPSDataTypeFloat16 )
              {
                  MPSGraphTensor* inputCastedTensor = [mpsGraph castTensor:inputTensor
                                                                    toType:MPSDataTypeInt32
                                                                      name:@"all_all"];
                  MPSGraphTensor* outputCastedTensor = [mpsGraph reductionAndWithTensor:inputCastedTensor
                                                                                   axes:nil
                                                                                   name:nil];
                  outputTensor = [mpsGraph castTensor:outputCastedTensor
                                               toType:MPSDataTypeBool
                                                 name:@"all_all"];
              }
              else
              {
                  MPSGraphTensor* outputUncastedTensor = [mpsGraph reductionAndWithTensor:inputTensor
                                                                                     axes:nil
                                                                                     name:nil];
                  outputTensor = [mpsGraph castTensor:outputUncastedTensor
                                               toType:MPSDataTypeBool
                                                 name:@"all_all"];
              }
              newCachedGraph->inputTensor_ = inputTensor;
              newCachedGraph->outputTensor_ = outputTensor;

            }
            return newCachedGraph;
          });
          cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        auto inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);
        auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t);
        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
          inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
        };

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
          outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData(),
        };

        native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);
    }
}

//-----------------------------------------------------------------------
// Min and max functions

Tensor min_max_mps
  (const Tensor& input_t,
   string reduction_type,
   string func_name) {

  namespace native_mps = at::native::mps;

  // Derive from MPSCachedGraph
  struct CachedGraph : public native_mps::MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

  IntArrayRef input_shape = input_t.sizes();
  int64_t num_input_dims = input_shape.size();

  // Flatten the input tensor to reduce it to one value
  NSMutableArray<NSNumber*> *apparent_input_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
  int64_t num_in_elements = 1;
  for(int i = 0; i < num_input_dims; i++) {
      num_in_elements *= input_shape[i];
  }
  apparent_input_shape[0] = [NSNumber numberWithInt:num_in_elements];

  Tensor output_t = at::native::empty_mps({}, input_t.scalar_type(), c10::nullopt, kMPS, c10::nullopt, c10::nullopt);

  if (output_t.numel() == 0 || num_in_elements == 0) {
    return output_t;
  }

  @autoreleasepool {
    string key = func_name + mps::getTensorsStringKey(input_t);
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    // Initialize once if configuration not found in cache
    if(!cachedGraph) {
      native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          MPSGraph* mpsGraph = native_mps::make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor* inputTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(input_t.scalar_type()));

          MPSGraphTensor* outputTensor = nil;

          if(reduction_type == "max")
            outputTensor = [mpsGraph reductionMaximumWithTensor:inputTensor
                                                           axes:@[@0]
                                                           name:nil];
          else if(reduction_type == "min")
            outputTensor = [mpsGraph reductionMinimumWithTensor:inputTensor
                                                           axes:@[@0]
                                                           name:nil];

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->outputTensor_ = outputTensor;

        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    auto inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t, apparent_input_shape);
    auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, @[@1]);

    NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
      inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
    };

    NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    native_mps::runMPSGraph(getCurrentMPSStream(), cachedGraph->graph(), feeds, results);
  }

  return output_t;
}

// Max entire tensor into scalar result
Tensor max_mps(const Tensor& input_t) {

  return min_max_mps(input_t, "max", "max_mps");
}

// Min entire tensor into scalar result
Tensor min_mps(const Tensor& input_t) {

  return min_max_mps(input_t, "min", "min_mps");
}

void min_max_out_mps
  (const Tensor& input_t,
  int64_t dim,
  bool keepdim,
  const Tensor& output_t,
  const Tensor& indices_t,
  string reduction_type,
  string func_name) {

    namespace native_mps = at::native::mps;

    if (output_t.numel() == 0) {
      return;
    }
    if (input_t.numel() == 1 && input_t.dim() == 0) {
      output_t.fill_(input_t);
      indices_t.fill_(0);
      return;
    }


    // Derive from MPSCachedGraph
    struct CachedGraph : public native_mps::MPSCachedGraph
    {
      CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
      MPSGraphTensor *inputTensor_ = nil;
      MPSGraphTensor *outputTensor_ = nil;
      MPSGraphTensor *indicesTensor_ = nil;
    };

    native_mps::MPSGraphCache* cache_ = native_mps::MPSGraphCache::getInstance();

    int64_t dim_ = maybe_wrap_dim(dim, input_t.dim());

    // Calculate the output shape according to keepdim=True
    // If there is no dim argument, the input shape is flattened
    IntArrayRef input_shape = input_t.sizes();
    int64_t num_input_dims = input_shape.size();
    NSMutableArray<NSNumber*> *apparent_out_shape = nil;

    apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:num_input_dims];
    for(int i = 0; i < num_input_dims; i++) {
        if(dim_ == i)
            apparent_out_shape[i] = @1;
        else
            apparent_out_shape[i] = [NSNumber numberWithInt:input_shape[i]];
    }

    auto stream = at::mps::getCurrentMPSStream();

    @autoreleasepool {
        string key = func_name + ":" + to_string(dim_) + ":" + native_mps::getMPSTypeString(input_t.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
          native_mps::MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ native_mps::MPSCachedGraph * () {

            CachedGraph *newCachedGraph = nil;

            @autoreleasepool {
              MPSGraph* mpsGraph = native_mps::make_mps_graph();
              newCachedGraph = new CachedGraph(mpsGraph);

              MPSGraphTensor* inputTensor = native_mps::mpsGraphUnrankedPlaceHolder(mpsGraph, native_mps::getMPSDataType(input_t.scalar_type()));
              MPSGraphTensor* outputTensor = nil;
              if(reduction_type == "max")
                outputTensor = [mpsGraph reductionMaximumWithTensor:inputTensor
                                                               axis:(NSInteger)dim_
                                                               name:nil];
              else if(reduction_type == "min")
                outputTensor = [mpsGraph reductionMinimumWithTensor:inputTensor
                                                               axis:(NSInteger)dim_
                                                               name:nil];

              MPSGraphTensor* castInputTensor = nil;

              if(input_t.scalar_type() != ScalarType::Float &&
                 input_t.scalar_type() != ScalarType::Int   &&
                 input_t.scalar_type() != ScalarType::Half)
                castInputTensor =  [mpsGraph castTensor:inputTensor
                                                 toType:MPSDataTypeFloat32
                                                   name:@"castInputTensor"];
              else
                castInputTensor = inputTensor;

              MPSGraphTensor* argreduceOutTensor = nil;
              if(reduction_type == "max")
                argreduceOutTensor = [mpsGraph reductionArgMaximumWithTensor:castInputTensor
                                                                        axis:(NSInteger)dim_
                                                                        name:@"argmax_out"];
              else if(reduction_type == "min")
                argreduceOutTensor = [mpsGraph reductionArgMinimumWithTensor:castInputTensor
                                                                        axis:(NSInteger)dim_
                                                                        name:@"argmax_out"];

              MPSGraphTensor *indicesTensor = [mpsGraph castTensor:argreduceOutTensor
                                                            toType:MPSDataTypeInt64
                                                              name:@"cast_out"];

              newCachedGraph->inputTensor_ = inputTensor;
              newCachedGraph->outputTensor_ = outputTensor;
              newCachedGraph->indicesTensor_ = indicesTensor;
            }
            return newCachedGraph;
          });
          cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        auto inputPlaceholder = native_mps::Placeholder(cachedGraph->inputTensor_, input_t);
        auto outputPlaceholder = native_mps::Placeholder(cachedGraph->outputTensor_, output_t, apparent_out_shape);
        auto indicesPlaceholder = native_mps::Placeholder(cachedGraph->indicesTensor_, indices_t, apparent_out_shape);

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *feeds = @{
          inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
        };

        NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results = @{
          outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData(),
          indicesPlaceholder.getMPSGraphTensor() : indicesPlaceholder.getMPSGraphTensorData()
        };

        native_mps::runMPSGraph(stream, cachedGraph->graph(), feeds, results);

    }

}

// Max out with dim
TORCH_IMPL_FUNC(max_out_mps)
  (const Tensor& input_t,
   int64_t dim,
   bool keepdim,
   const Tensor& output_t,
   const Tensor& indices_t) {

    int64_t dim_ = maybe_wrap_dim(dim, input_t.dim());
    native::zero_numel_check_dims(input_t, dim_,  "max()");

    min_max_out_mps(input_t, dim, keepdim, output_t, indices_t, "max", "max_out_mps");
}

// Min out with dim
TORCH_IMPL_FUNC(min_out_mps)
  (const Tensor& input_t,
   int64_t dim,
   bool keepdim,
   const Tensor& output_t,
   const Tensor& indices_t) {

    int64_t dim_ = maybe_wrap_dim(dim, input_t.dim());
    native::zero_numel_check_dims(input_t, dim_, "min()");

    min_max_out_mps(input_t, dim, keepdim, output_t, indices_t, "min", "min_out_mps");
}

// Min/Max with dim
std::tuple<Tensor, Tensor> min_max_mps
   (const Tensor& input_t,
    int64_t dim,
    bool keepdim,
    string reduction_type,
    string func_name) {

    namespace native_mps = at::native::mps;

    int64_t dim_ = maybe_wrap_dim(dim, input_t.dim());
    native::zero_numel_check_dims(input_t, dim_, "max()");

    // Calculate the output shape according to keepdim=True
    // If there is no dim argument, the input shape is flattened
    IntArrayRef input_shape = input_t.sizes();
    int64_t num_input_dims = input_shape.size();
    NSMutableArray<NSNumber*> *apparent_out_shape = nil;
    // Use this if keepdim is false
    int64_t num_output_dims = num_input_dims - 1;

    int64_t* malloc_apparent_out_shape = (int64_t *)malloc(num_input_dims * sizeof(int64_t));
    int64_t* malloc_out_shape = (int64_t *)malloc(num_output_dims * sizeof(int64_t));

    apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:num_input_dims];
    // Counter for shape when keepdim is false
    int out_i = 0;
    for(int i = 0; i < num_input_dims; i++) {
        if(dim_ == i) {
            apparent_out_shape[i] = @1;
            malloc_apparent_out_shape[i] = 1;
        }
        else {
            apparent_out_shape[i] = [NSNumber numberWithInt:input_shape[i]];
            malloc_apparent_out_shape[i] = input_shape[i];
            malloc_out_shape[out_i] = input_shape[i];
            out_i++;
        }
    }

    Tensor output_t;
    Tensor indices_t;
    if(!keepdim) {
     output_t = at::native::empty_mps(
                      IntArrayRef(malloc_out_shape, num_output_dims),
                      input_t.scalar_type(),
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);
     indices_t = at::native::empty_mps(
                      IntArrayRef(malloc_out_shape, num_output_dims),
                      ScalarType::Long,
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);
    }
    else {
      output_t = at::native::empty_mps(
                      IntArrayRef(malloc_apparent_out_shape, num_input_dims),
                      input_t.scalar_type(),
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);
     indices_t = at::native::empty_mps(
                      IntArrayRef(malloc_apparent_out_shape, num_input_dims),
                      ScalarType::Long,
                      c10::nullopt,
                      kMPS,
                      c10::nullopt,
                      c10::nullopt);
    }

    if (output_t.numel() == 0 || input_t.numel() == 0) {
        free(malloc_out_shape);
        free(malloc_apparent_out_shape);
        return std::tuple<Tensor, Tensor>{output_t, indices_t};
    }

    min_max_out_mps(input_t, dim, keepdim, output_t, indices_t, reduction_type, func_name);

    free(malloc_out_shape);
    free(malloc_apparent_out_shape);
    return std::tuple<Tensor, Tensor>{output_t, indices_t};
}

// Max with dim
std::tuple<Tensor, Tensor> max_mps
   (const Tensor& input_t,
    int64_t dim,
    bool keepdim) {

    return min_max_mps(input_t, dim, keepdim, "max", "max_mps");
}

// Min with dim
std::tuple<Tensor, Tensor> min_mps
   (const Tensor& input_t,
    int64_t dim,
    bool keepdim) {

    return min_max_mps(input_t, dim, keepdim, "min", "min_mps");
}

}

}
