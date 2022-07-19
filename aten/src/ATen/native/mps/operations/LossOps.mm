//  Copyright © 2022 Apple Inc.

#include <ATen/native/mps/OperationUtils.h>
#include <ATen/mps/MPSStream.h>
#include <objc/NSObjCRuntime.h>
#include <torch/library.h>

#ifdef __OBJC__
#include <MetalPerformanceShaders/MetalPerformanceShaders.h>
#endif

namespace at {
namespace native {
namespace mps {

string reductionToString(int64_t reduction)
{
    switch(reduction) {
        case Reduction::Mean: return "Mean";
        case Reduction::Sum: return "Sum";
        default: return "None";
    }
}

MPSGraphTensor* reduceTensor(MPSGraphTensor *tensor, int64_t reduction, MPSGraph *mpsGraph, NSUInteger axesCount)
{
    NSMutableArray<NSNumber *> *axes = [NSMutableArray<NSNumber*> arrayWithCapacity:axesCount];
    for (NSUInteger i = 0; i < axesCount; i++) axes[i] = @(i);

    switch(reduction) {
        case Reduction::Mean:
            return [mpsGraph meanOfTensor: tensor axes: axes name: @"reductionMeanTensor"];
        case Reduction::Sum:
            return [mpsGraph reductionSumWithTensor: tensor axes: axes name: @"reductionSumTensor"];
        default:
            assert(reduction == Reduction::None);
            return tensor;
    }
}

// MSELoss
void mse_loss_out_impl(const Tensor& input, const Tensor& target,
                          int64_t reduction, const Tensor& output, const string op_name)
{
}

Tensor& mse_loss_backward_out_impl(const Tensor& grad_output, const Tensor& input, const Tensor& target,
                                   int64_t reduction, Tensor& grad_input, const string op_name)
{
    TORCH_CHECK(target.is_same_size(input), op_name + ": target and input tensors must have identical shapes")
    auto norm = reduction == Reduction::Mean ? 2. / static_cast<double>(input.numel()) : 2.;

    struct CachedGraph : public MPSCachedGraph
    {
        CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
        MPSGraphTensor *inputTensor = nil, *targetTensor = nil;
        MPSGraphTensor *gradInputTensor = nil, *gradOutputTensor = nil;
    };
    MPSGraphCache* cache_ = MPSGraphCache::getInstance();

    @autoreleasepool {
        string key = op_name + reductionToString(reduction) + ":" +
                               to_string(grad_input.sizes()[1]) +
                               getTensorsStringKey({input, target, grad_output});

        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
        if(!cachedGraph) {
            cachedGraph = static_cast<CachedGraph*>(cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

                CachedGraph *newCachedGraph = nil;

                @autoreleasepool {
                    MPSGraph* mpsGraph = make_mps_graph();
                    newCachedGraph = new CachedGraph(mpsGraph);

                    newCachedGraph->inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, input);
                    newCachedGraph->targetTensor = mpsGraphRankedPlaceHolder(mpsGraph, target);
                    newCachedGraph->gradOutputTensor = mpsGraphRankedPlaceHolder(mpsGraph, grad_output);

                    MPSGraphTensor *normTensor = [mpsGraph constantWithScalar: norm
                                                           dataType: MPSDataTypeFloat32];
                    MPSGraphTensor *diffTensor = [mpsGraph subtractionWithPrimaryTensor: newCachedGraph->inputTensor
                                                                        secondaryTensor: newCachedGraph->targetTensor
                                                                                   name: nil];
                    MPSGraphTensor *diffGradientTensor = [mpsGraph multiplicationWithPrimaryTensor: diffTensor
                                                                                   secondaryTensor: newCachedGraph->gradOutputTensor
                                                                                              name: nil];
                    newCachedGraph->gradInputTensor = [mpsGraph multiplicationWithPrimaryTensor: diffGradientTensor
                                                                                secondaryTensor: normTensor
                                                                                           name: nil];
                }
                return newCachedGraph;
            }));
        }
        Placeholder inputPlaceholder  = Placeholder(cachedGraph->inputTensor, input);
        Placeholder targetPlaceholder = Placeholder(cachedGraph->targetTensor, target);
        Placeholder gradInputPlaceholder = Placeholder(cachedGraph->gradInputTensor, grad_input);
        Placeholder gradOutputPlaceholder = Placeholder(cachedGraph->gradOutputTensor, grad_output);

        NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
            inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
            targetPlaceholder.getMPSGraphTensor() : targetPlaceholder.getMPSGraphTensorData(),
            gradOutputPlaceholder.getMPSGraphTensor() : gradOutputPlaceholder.getMPSGraphTensorData()
        };
        NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
            gradInputPlaceholder.getMPSGraphTensor() :gradInputPlaceholder.getMPSGraphTensorData()
        };

        runMPSGraph(getCurrentMPSStream(), cachedGraph->graph(), feeds, results);
    }

    return grad_input;
}

// namespace to localize the CachedGraph struct for Binary Cross Entropy
namespace BCELoss
{

struct CachedGraph : public MPSCachedGraph
{
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor = nil, *targetTensor = nil;
    // gradOutput only used on backward pass
    MPSGraphTensor *weightTensor = nil, *gradOutputTensor = nil;
    // lossTensor used for forward, and gradInputTensor for backward pass
    union { MPSGraphTensor *lossTensor = nil; MPSGraphTensor *gradInputTensor; };
};

MPSGraphTensor* bce_forward_mps(CachedGraph *bceGraph)
{
    MPSGraph *mpsGraph = bceGraph->graph();

    // Forward BCE: L = -w (y ln(x) + (1-y) ln(1-x))
    MPSGraphTensor *one = [mpsGraph constantWithScalar: 1.0
                                              dataType: MPSDataTypeFloat32];
    // -100 is the hard limit value defined in BCELoss Spec. to clamp the log
    MPSGraphTensor *neg100 = [mpsGraph constantWithScalar: -100.0
                                                 dataType: MPSDataTypeFloat32];
    // 1 - x
    MPSGraphTensor *one_Input = [mpsGraph subtractionWithPrimaryTensor: one
                                                       secondaryTensor: bceGraph->inputTensor
                                                                  name: nil];
    // log(x)
    MPSGraphTensor *logInput = [mpsGraph logarithmWithTensor: bceGraph->inputTensor
                                                        name: nil];
    // max(log(x), -100)
    MPSGraphTensor *clampedLogInput = [mpsGraph maximumWithPrimaryTensor: logInput
                                                         secondaryTensor: neg100
                                                                    name: nil];
    // log(1 - x)
    MPSGraphTensor *log1_Input = [mpsGraph logarithmWithTensor: one_Input
                                                          name: nil];
    // max(log(1 - x), -100)
    MPSGraphTensor *clampedLog1_Input = [mpsGraph maximumWithPrimaryTensor: log1_Input
                                                           secondaryTensor: neg100
                                                                      name: nil];
    // (y - 1) resulted from -(1 - y)
    MPSGraphTensor *target_1 = [mpsGraph subtractionWithPrimaryTensor: bceGraph->targetTensor
                                                      secondaryTensor: one
                                                                 name: nil];
    // (y - 1) * max(log(1 - x), -100)
    MPSGraphTensor *target_1TimesLog1_Input = [mpsGraph multiplicationWithPrimaryTensor: target_1
                                                                        secondaryTensor: clampedLog1_Input
                                                                                   name: nil];
    // y * max(log(x), -100)
    MPSGraphTensor *targetTimesLogInput = [mpsGraph multiplicationWithPrimaryTensor: bceGraph->targetTensor
                                                                    secondaryTensor: clampedLogInput
                                                                               name: nil];
    // ((y - 1) * max(log(1 - x), -100)) - (y * max(log(x), -100))
    MPSGraphTensor *bceLoss = [mpsGraph subtractionWithPrimaryTensor: target_1TimesLog1_Input
                                                     secondaryTensor: targetTimesLogInput
                                                                name: nil];
    return bceLoss;
}

MPSGraphTensor* bce_backward_mps(CachedGraph *bceGraph)
{
    MPSGraph *mpsGraph = bceGraph->graph();

    // Backward BCE: d(L)/d(x) = -w (y - x) / (x - x^2)
    MPSGraphTensor *one = [mpsGraph constantWithScalar: 1.0
                                              dataType: MPSDataTypeFloat32];
    // epsilon used to clamp the grad input denominator
    MPSGraphTensor *epsilon = [mpsGraph constantWithScalar: 1e-12
                                                  dataType: MPSDataTypeFloat32];
    // 1 - x
    MPSGraphTensor *one_Input = [mpsGraph subtractionWithPrimaryTensor: one
                                                       secondaryTensor: bceGraph->inputTensor
                                                                  name: nil];
    // x * (1 - x)
    MPSGraphTensor *inputTimes1_Input = [mpsGraph multiplicationWithPrimaryTensor: bceGraph->inputTensor
                                                                  secondaryTensor: one_Input
                                                                             name: nil];
    // max(x * (1 - x), epsilon)
    MPSGraphTensor *gradInputDenominator = [mpsGraph maximumWithPrimaryTensor: inputTimes1_Input
                                                              secondaryTensor: epsilon
                                                                         name: nil];
    // (x - y)
    MPSGraphTensor *input_target = [mpsGraph subtractionWithPrimaryTensor: bceGraph->inputTensor
                                                          secondaryTensor: bceGraph->targetTensor
                                                                     name: nil];
    // (x - y) / max(x * (1 - x), epsilon)
    MPSGraphTensor *inputDivGradInputDenom = [mpsGraph divisionWithPrimaryTensor: input_target
                                                                 secondaryTensor: gradInputDenominator
                                                                            name: nil];
    // gradOutput * (((x - y) / max(x * (1 - x), epsilon)))
    MPSGraphTensor *gradInput = [mpsGraph multiplicationWithPrimaryTensor: bceGraph->gradOutputTensor
                                                          secondaryTensor: inputDivGradInputDenom
                                                                     name: nil];
    return gradInput;
}

// Binary Cross Enropy (Forward/Backward BCELoss)
// NOTE: "loss" tensor would be "grad_input" if it's a backward pass
Tensor& bce_loss_out_impl(const Tensor& input, const Tensor& target,
                          const c10::optional<Tensor>& weight_opt, int64_t reduction, Tensor& loss,
                          const c10::optional<Tensor>& grad_output_opt, const string op_name)
{
    // TODO: add sanity check for the elements of input tensor to be within [0..1]
    TORCH_CHECK(target.is_same_size(input), op_name + ": target and input tensors must have identical shapes")

    c10::MaybeOwned<Tensor> weight_maybe_owned      = at::borrow_from_optional_tensor(weight_opt);
    c10::MaybeOwned<Tensor> grad_output_maybe_owned = at::borrow_from_optional_tensor(grad_output_opt);
    const Tensor& weight      = *weight_maybe_owned;
    const Tensor& grad_output = *grad_output_maybe_owned;

    loss.resize_((reduction == Reduction::None || grad_output.defined()) ? target.sizes() : IntArrayRef({}));
    TORCH_CHECK(loss.is_mps());

    Tensor loss_squeezed = at::squeeze(loss);
    Tensor input_squeezed = at::squeeze(input);
    Tensor target_squeezed = at::squeeze(target);

    MPSGraphCache* cache_ = MPSGraphCache::getInstance();

    @autoreleasepool {
        string key = op_name + reductionToString(reduction) + getTensorsStringKey({input_squeezed, target_squeezed, weight});

        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
        if(!cachedGraph) {
            cachedGraph = static_cast<CachedGraph*>(cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

                CachedGraph *newCachedGraph = nil;

                @autoreleasepool {
                    MPSGraph* mpsGraph = make_mps_graph();
                    newCachedGraph = new CachedGraph(mpsGraph);

                    newCachedGraph->inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, input_squeezed);
                    newCachedGraph->targetTensor = mpsGraphRankedPlaceHolder(mpsGraph, target_squeezed);

                    MPSGraphTensor *bceLossUnweighted = nil;
                    // if grad_output is defined, then it's a backward pass
                    if (grad_output.defined()) {
                        newCachedGraph->gradOutputTensor = mpsGraphRankedPlaceHolder(mpsGraph, grad_output);
                        bceLossUnweighted = bce_backward_mps(newCachedGraph);
                    } else {
                        bceLossUnweighted = bce_forward_mps(newCachedGraph);
                    }

                    MPSGraphTensor *bceLoss = bceLossUnweighted;
                    if (weight.defined()) {
                        newCachedGraph->weightTensor = mpsGraphRankedPlaceHolder(mpsGraph, weight);
                        bceLoss = [mpsGraph multiplicationWithPrimaryTensor: bceLossUnweighted
                                                            secondaryTensor: newCachedGraph->weightTensor
                                                                       name: nil];
                    }

                    if (grad_output.defined()) {
                        if (reduction == at::Reduction::Mean) {
                            MPSGraphTensor *inputNumel = [mpsGraph constantWithScalar: static_cast<double>(input.numel())
                                                                             dataType: MPSDataTypeFloat32];
                            newCachedGraph->gradInputTensor = [mpsGraph divisionWithPrimaryTensor: bceLoss
                                                                                  secondaryTensor: inputNumel
                                                                                             name: nil];
                        } else {
                            newCachedGraph->gradInputTensor = bceLoss;
                        }
                    } else {
                        newCachedGraph->lossTensor = reduceTensor(bceLoss, reduction, mpsGraph, input.sizes().size());
                    }
                }
                return newCachedGraph;
            }));
        }
        Placeholder inputPlaceholder  = Placeholder(cachedGraph->inputTensor, input_squeezed);
        Placeholder targetPlaceholder = Placeholder(cachedGraph->targetTensor, target_squeezed);
        Placeholder lossPlaceholder   = Placeholder(cachedGraph->lossTensor, loss_squeezed);

        NSMutableDictionary *feeds = [[NSMutableDictionary new] autorelease];

        feeds[inputPlaceholder.getMPSGraphTensor()] = inputPlaceholder.getMPSGraphTensorData();
        feeds[targetPlaceholder.getMPSGraphTensor()] = targetPlaceholder.getMPSGraphTensorData();
        if (weight.defined()) {
            Placeholder weightPlaceholder = Placeholder(cachedGraph->weightTensor, weight);
            feeds[weightPlaceholder.getMPSGraphTensor()] = weightPlaceholder.getMPSGraphTensorData();
        }
        if (grad_output.defined()) {
            Placeholder gradOutputPlaceholder = Placeholder(cachedGraph->gradOutputTensor, grad_output);
            feeds[gradOutputPlaceholder.getMPSGraphTensor()] = gradOutputPlaceholder.getMPSGraphTensorData();
        }

        NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
            lossPlaceholder.getMPSGraphTensor() : lossPlaceholder.getMPSGraphTensorData()
        };

        runMPSGraph(getCurrentMPSStream(), cachedGraph->graph(), feeds, results);
    }

    return loss;
}

} // namespace BCELoss

// NLLLoss
void nllnd_loss_backward_impl(
Tensor& grad_input,
const Tensor& grad_output,
const Tensor& input,
const Tensor& target,
const Tensor& weight,
int64_t reduction,
int64_t ignore_index,
const Tensor& total_weight,
bool is2D)
{
    // Empty output
    if(grad_input.numel() == 0)
        return;

    MPSStream* stream = getCurrentMPSStream();

    struct CachedGraph : public MPSCachedGraph
    {
        CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
        MPSGraphTensor* inputTensor_ = nil;
        MPSGraphTensor* targetTensor_ = nil;
        MPSGraphTensor* weightTensor_ = nil;
        MPSGraphTensor* totalWeightTensor_ = nil;
        MPSGraphTensor* gradInputTensor_ = nil;
    };

    MPSGraphCache* cache_ = MPSGraphCache::getInstance();

    @autoreleasepool {

        auto numClasses = grad_input.sizes()[1];
        bool isWeightsArrayValid = (weight.numel() > 0);

        MPSShape* input_shape = getMPSShape(input);
        MPSShape* target_shape = getMPSShape(target);
        MPSShape* weight_shape = getMPSShape(weight);
        MPSShape* total_weight_shape = getMPSShape(total_weight);

        NSString* ns_shape_key = [[input_shape valueForKey:@"description"] componentsJoinedByString:@","];

        string key = "nllnd_loss_backward_impl:" + to_string(numClasses) + ":" +
                                                   to_string(ignore_index) + ":" +
                                                   to_string(isWeightsArrayValid) + ":" +
                                                   reductionToString(reduction) + ":" +
                                                   [ns_shape_key UTF8String] + ":" +
                                                   getMPSTypeString(input.scalar_type()) + ":" +
                                                   getMPSTypeString(target.scalar_type()) + ":" +
                                                   getMPSTypeString(weight.scalar_type()) + ":" +
                                                   getMPSTypeString(total_weight.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
            MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

                CachedGraph *newCachedGraph = nil;

                @autoreleasepool {

                    MPSGraph* mpsGraph = make_mps_graph();
                    newCachedGraph = new CachedGraph(mpsGraph);

                    MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, getMPSDataType(input.scalar_type()), input_shape);
                    MPSGraphTensor* targetTensor = mpsGraphRankedPlaceHolder(mpsGraph, getMPSDataType(target.scalar_type()), target_shape);
                    MPSGraphTensor* weightTensor = nil;
                    if(isWeightsArrayValid)
                        weightTensor = mpsGraphRankedPlaceHolder(mpsGraph, getMPSDataType(weight.scalar_type()), weight_shape);
                    MPSGraphTensor* totalWeightTensor = mpsGraphRankedPlaceHolder(mpsGraph, getMPSDataType(total_weight.scalar_type()), total_weight_shape);

                    MPSGraphTensor *udpatedTargetTensor = targetTensor;

                    // Replace ignored_index with length depth + 1 so that oneHotAPI ignores it
                    if(ignore_index != -100)
                    {
                        MPSGraphTensor *mpsGraphIndexTensor = [mpsGraph constantWithScalar: ignore_index
                                                                                  dataType: MPSDataTypeInt64];
                        MPSGraphTensor *mpsGraphDepthPlusOneTensor = [mpsGraph constantWithScalar: (numClasses + 1)
                                                                                  dataType: MPSDataTypeInt64];

                        // Equal tensor
                        MPSGraphTensor* mpsGraphIsEqualTensor = [mpsGraph equalWithPrimaryTensor: targetTensor
                                                                                 secondaryTensor: mpsGraphIndexTensor
                                                                                            name: @"isEqualTensor"];

                        udpatedTargetTensor = [mpsGraph selectWithPredicateTensor: mpsGraphIsEqualTensor
                                                          truePredicateTensor: mpsGraphDepthPlusOneTensor
                                                         falsePredicateTensor: targetTensor
                                                                         name: @"predicateTensor"];
                    }

                    float onValue = -1.0f;

                    MPSGraphTensor *oneHotTensor;

                    oneHotTensor = [mpsGraph oneHotWithIndicesTensor:udpatedTargetTensor
                                                               depth:numClasses
                                                                axis:1
                                                            dataType:inputTensor.dataType
                                                             onValue:onValue
                                                            offValue:0.0f
                                                                name:nil];

                    if(isWeightsArrayValid)
                    {
                        oneHotTensor = [mpsGraph multiplicationWithPrimaryTensor:oneHotTensor
                                                                 secondaryTensor:weightTensor
                                                                            name:@"scaleByWeightTensor"];
                    }

                    if(reduction == Reduction::Mean)
                    {
                        oneHotTensor = [mpsGraph divisionNoNaNWithPrimaryTensor:oneHotTensor
                                                                secondaryTensor:totalWeightTensor
                                                                           name:@"divisionTensor"];
                    }

                    MPSGraphTensor* gradInputTensor = oneHotTensor;

                    newCachedGraph->inputTensor_ = inputTensor;
                    newCachedGraph->targetTensor_ = targetTensor;
                    newCachedGraph->weightTensor_ = weightTensor;
                    newCachedGraph->totalWeightTensor_ = totalWeightTensor;
                    newCachedGraph->gradInputTensor_ = gradInputTensor;

                }
                return newCachedGraph;
            });
            cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        auto inputPlaceholder   = Placeholder(cachedGraph->inputTensor_, input);
        auto targetPlaceholder   = Placeholder(cachedGraph->targetTensor_, target);
        Placeholder weightPlaceholder = Placeholder();
        if(isWeightsArrayValid)
            weightPlaceholder  = Placeholder(cachedGraph->weightTensor_, weight);
        auto totalWeightPlaceholder   = Placeholder(cachedGraph->totalWeightTensor_, total_weight);
        auto gradInputPlaceholder = Placeholder(cachedGraph->gradInputTensor_, grad_input);

        NSMutableDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = [[NSMutableDictionary alloc] initWithCapacity: 4];
        feeds[inputPlaceholder.getMPSGraphTensor()] = inputPlaceholder.getMPSGraphTensorData();
        feeds[targetPlaceholder.getMPSGraphTensor()] = targetPlaceholder.getMPSGraphTensorData();
        feeds[totalWeightPlaceholder.getMPSGraphTensor()] = totalWeightPlaceholder.getMPSGraphTensorData();

        if(isWeightsArrayValid)
            feeds[weightPlaceholder.getMPSGraphTensor()] = weightPlaceholder.getMPSGraphTensorData();

        NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
            gradInputPlaceholder.getMPSGraphTensor() : gradInputPlaceholder.getMPSGraphTensorData()
        };

        runMPSGraph(stream, cachedGraph->graph(), feeds, results);
    }

    return;
}

void nllnd_loss_forward_impl
(Tensor& output,
 Tensor& total_weight,
 const Tensor& input,
 const Tensor& target,
 const Tensor& weight,
 int64_t reduction,
 int64_t ignore_index,
 bool is2D)
{
    std::vector<long long> reshapedTarget(target.sizes().begin(), target.sizes().end());
    reshapedTarget.push_back(1);

    Tensor batchSizeTensor = at::empty_like(input).resize_(IntArrayRef(1));
    float batchVal = 1.0f;
    for(size_t i = 0; i < reshapedTarget.size(); ++i)
        batchVal *= reshapedTarget[i];
    batchSizeTensor[0] = batchVal;

    if(reduction == Reduction::None)
        output.resize_(target.sizes());
    if(reduction == Reduction::Sum)
        output.resize_({});
    if(reduction == Reduction::Mean)
        output.resize_({});

    TORCH_CHECK(output.is_mps());

    // Empty output
    if(output.numel() == 0)
        return;

    struct CachedGraph : public MPSCachedGraph
    {
        CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
        MPSGraphTensor* inputTensor_ = nil;
        MPSGraphTensor* targetTensor_ = nil;
        MPSGraphTensor* weightTensor_ = nil;
        MPSGraphTensor* batchSizeTensor_ = nil;
        MPSGraphTensor* totalWeightTensor_ = nil;
        MPSGraphTensor* outputTensor_ = nil;
    };

    MPSGraphCache* cache_ = MPSGraphCache::getInstance();

    MPSStream* stream = getCurrentMPSStream();

    @autoreleasepool {

        bool isWeightsArrayValid = (weight.numel() > 0);

        MPSShape* input_shape = getMPSShape(input);
        MPSShape* target_shape = getMPSShape(target);
        MPSShape* weight_shape = getMPSShape(weight);

        NSString* ns_shape_key = [[input_shape valueForKey:@"description"] componentsJoinedByString:@","];

        // TODO: Make the key
        string key = "nllnd_loss_forward_impl:" + to_string(ignore_index) + ":" +
                                                  to_string(isWeightsArrayValid) + ":" +
                                                  reductionToString(reduction) + ":" +
                                                  [ns_shape_key UTF8String] + ":" +
                                                  getMPSTypeString(input.scalar_type()) + ":" +
                                                  getMPSTypeString(target.scalar_type()) + ":" +
                                                  getMPSTypeString(weight.scalar_type());
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));

        if(!cachedGraph) {
            MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

                CachedGraph *newCachedGraph = nil;

                @autoreleasepool {
                    MPSGraph* mpsGraph = make_mps_graph();
                    newCachedGraph = new CachedGraph(mpsGraph);

                    MPSGraphTensor* inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, getMPSDataType(input.scalar_type()), input_shape);
                    MPSGraphTensor* targetTensor = mpsGraphRankedPlaceHolder(mpsGraph, getMPSDataType(target.scalar_type()), target_shape);
                    MPSGraphTensor* weightTensor = nil;
                    if(isWeightsArrayValid)
                        weightTensor = mpsGraphRankedPlaceHolder(mpsGraph, getMPSDataType(weight.scalar_type()), weight_shape);
                    MPSGraphTensor* mps_batchSizeTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(batchSizeTensor.scalar_type()));

                    MPSGraphTensor* mpsGraphBatchSizeTensor = mps_batchSizeTensor;

                    // The transposes are needed to get the class dimension (dim 1) to the inner most dim for gather op.
                    // The transpose become nop in the 2D case.
                    MPSGraphTensor* mpsTransposeTensor = inputTensor;
                    int classDim = 1;
                    int lastDim = input.sizes().size()-1;
                    mpsTransposeTensor = [mpsGraph transposeTensor:inputTensor
                                                         dimension:classDim
                                                     withDimension:lastDim
                                                              name:nil];
                    for(int i = 0; i < lastDim - 2; ++i)
                    {
                        mpsTransposeTensor = [mpsGraph transposeTensor:mpsTransposeTensor
                                                             dimension:classDim+i
                                                         withDimension:classDim+i+1 name:nil];
                    }


                    MPSGraphTensor* mpsGatherTensor = [mpsGraph gatherWithUpdatesTensor:mpsTransposeTensor
                                                                          indicesTensor:targetTensor
                                                                                   axis:lastDim
                                                                        batchDimensions:lastDim
                                                                                   name:@"gatherTensor"];

                    bool isIgnoreIndexValid = (ignore_index != -100);
                    MPSGraphTensor* weightGatherTensor;

                    if(isWeightsArrayValid)
                    {
                        weightGatherTensor = [mpsGraph gatherWithUpdatesTensor:weightTensor
                                                                 indicesTensor:targetTensor
                                                                          axis:0
                                                               batchDimensions:0
                                                                          name:@"weightGatherTensor"];
                        MPSGraphTensor *mpsGatherCopyTensor = [mpsGraph identityWithTensor:mpsGatherTensor
                                                                                      name:@"identityTensor"];
                        mpsGatherTensor = [mpsGraph multiplicationWithPrimaryTensor:weightGatherTensor
                                                                    secondaryTensor:mpsGatherCopyTensor
                                                                               name:@"scaledLossTensor"];
                    }

                    // Both these cases need recomputation of denominator when reductionMode == mean
                    if(isIgnoreIndexValid || isWeightsArrayValid)
                    {
                        // Setup tensors
                        MPSGraphTensor *mpsGraphZeroTensor = [mpsGraph constantWithScalar:0.0
                                                                                 dataType:mpsGatherTensor.dataType];
                        MPSGraphTensor *mpsGraphOneTensor = [mpsGraph constantWithScalar:1.0
                                                                                dataType:mpsGatherTensor.dataType];
                        // @TODO: Remove this identity call with ToT StarSky MPSGraph
                        MPSGraphTensor *mpsGraphOneTensorCopy = [mpsGraph identityWithTensor:mpsGraphOneTensor
                                                                                        name:@"IdentityHackTensor"];

                        MPSGraphTensor *mpsGraphIsEqualTensor;

                        if(isIgnoreIndexValid)
                        {
                            MPSGraphTensor *mpsGraphIndexTensor = [mpsGraph constantWithScalar:ignore_index
                                                                                      dataType:MPSDataTypeInt64];
                            // Equal tensor
                            mpsGraphIsEqualTensor = [mpsGraph equalWithPrimaryTensor:targetTensor
                                                                     secondaryTensor:mpsGraphIndexTensor
                                                                                name:@"isEqualTensor"];
                            // Zero out loss
                            MPSGraphTensor *mpsGatherCopyTensor = [mpsGraph identityWithTensor:mpsGatherTensor
                                                                                          name:@"identityTensor"];
                            mpsGatherTensor = [mpsGraph selectWithPredicateTensor:mpsGraphIsEqualTensor
                                                              truePredicateTensor:mpsGraphZeroTensor
                                                             falsePredicateTensor:mpsGatherCopyTensor
                                                                             name:@"predicateTensor"];
                        }

                        if(isWeightsArrayValid)
                        {
                            mpsGraphOneTensorCopy = weightGatherTensor;
                            if(!isIgnoreIndexValid)
                            {
                                mpsGraphIsEqualTensor = [mpsGraph constantWithScalar: 0.0
                                                                               shape: targetTensor.shape
                                                                            dataType: targetTensor.dataType];
                            }
                        }

                        // Compute new batch size
                        MPSGraphTensor* mpsSelectOneTensor = [mpsGraph selectWithPredicateTensor:mpsGraphIsEqualTensor
                                                                             truePredicateTensor:mpsGraphZeroTensor
                                                                            falsePredicateTensor:mpsGraphOneTensorCopy
                                                                                            name:@"predicateOneTensor"];
                        mpsGraphBatchSizeTensor = [mpsGraph reductionSumWithTensor:mpsSelectOneTensor
                                                                              axes:nil
                                                                              name:@"batchSizeReductionTensor"];
                    }

                    MPSGraphTensor *mpsGraphNegTensor = [mpsGraph negativeWithTensor:mpsGatherTensor
                                                                                name:@"negativeTensor"];

                    MPSGraphTensor* mpsGraphReducedTensor = mpsGraphNegTensor;

                    if(!(reduction == Reduction::None))
                    {
                        mpsGraphReducedTensor = [mpsGraph reductionSumWithTensor:mpsGraphNegTensor
                                                                            axes:nil
                                                                            name:@"reductionSumTensor"];
                        if(reduction == Reduction::Mean)
                        {
                            mpsGraphReducedTensor = [mpsGraph divisionNoNaNWithPrimaryTensor:mpsGraphReducedTensor
                                                                             secondaryTensor:mpsGraphBatchSizeTensor
                                                                                        name:@"divisionTensor"];
                        }
                    }

                    mpsGraphReducedTensor = [mpsGraph reshapeTensor:mpsGraphReducedTensor
                                                          withShape:getMPSShape(output)
                                                               name:nil];

                    newCachedGraph->inputTensor_ = inputTensor;
                    newCachedGraph->targetTensor_ = targetTensor;
                    newCachedGraph->weightTensor_ = weightTensor;
                    newCachedGraph->batchSizeTensor_ = mps_batchSizeTensor;
                    newCachedGraph->totalWeightTensor_ = mpsGraphBatchSizeTensor;
                    newCachedGraph->outputTensor_ = mpsGraphReducedTensor;
                }
                return newCachedGraph;
            });
            cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
        }

        Placeholder selfPlaceholder   = Placeholder(cachedGraph->inputTensor_, input);
        Placeholder targetPlaceholder   = Placeholder(cachedGraph->targetTensor_, target);
        Placeholder weightPlaceholder = Placeholder();
        if(isWeightsArrayValid)
            weightPlaceholder = Placeholder(cachedGraph->weightTensor_, weight);
        Placeholder batchSizePlaceholder   = Placeholder(cachedGraph->batchSizeTensor_, batchSizeTensor);
        Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output);
        Placeholder totalWeightsPlaceholder = Placeholder(cachedGraph->totalWeightTensor_, total_weight);

        // Create dictionary of inputs and outputs
        NSMutableDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = [[NSMutableDictionary alloc] initWithCapacity: 4];
        feeds[selfPlaceholder.getMPSGraphTensor()] = selfPlaceholder.getMPSGraphTensorData();
        feeds[targetPlaceholder.getMPSGraphTensor()] = targetPlaceholder.getMPSGraphTensorData();
        feeds[batchSizePlaceholder.getMPSGraphTensor()] = batchSizePlaceholder.getMPSGraphTensorData();

        if(isWeightsArrayValid)
            feeds[weightPlaceholder.getMPSGraphTensor()] = weightPlaceholder.getMPSGraphTensorData();

        NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
            outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData(),
            totalWeightsPlaceholder.getMPSGraphTensor() : totalWeightsPlaceholder.getMPSGraphTensorData()
        };

        runMPSGraph(stream, cachedGraph->graph(), feeds, results);

    }

    return;
}

void smooth_l1_loss_impl(
    const Tensor &input,
    const Tensor &target,
    const int64_t reduction,
    double beta,
    const Tensor &output,
    MPSShape *mpsInputShape,
    MPSShape *mpsOutputShape)
{
 struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *targetTensor_ = nil;
    MPSGraphTensor *outputTensor_ = nil;
  };

  MPSGraphCache *cache_ = MPSGraphCache::getInstance();

  MPSStream *stream= getCurrentMPSStream();

  @autoreleasepool {
    MPSShape* input_shape = getMPSShape(input);
    NSString* ns_shape_key = [[input_shape valueForKey:@"description"] componentsJoinedByString:@","];

    string key = "smooth_l1_loss_impl:" + reductionToString(reduction) + ":" +
                                          [ns_shape_key UTF8String] + ":" +
                                          to_string(beta) + ":" +
                                          getMPSTypeString(input.scalar_type()) + ":" +
                                          getMPSTypeString(target.scalar_type());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        // smooth_l1_loss_mps:
        // ln = 0.5 * ( xn - yn ) ^ 2 / beta,       if |xn - yn| < beta
        //    = | xn - yn | - 0.5 * beta,           otherwise

        @autoreleasepool {
          // Initialize graph
          MPSGraph *mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(input.scalar_type()));
          MPSGraphTensor *targetTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(target.scalar_type()));

          // Setup tensors
          MPSGraphTensor *mpsGraphZeroTensor = [mpsGraph constantWithScalar: 0.0
                                                                   dataType: inputTensor.dataType];
          MPSGraphTensor *mpsGraphOneTensor = [mpsGraph constantWithScalar: 1.0
                                                                  dataType: inputTensor.dataType];
          MPSGraphTensor *mpsGraphHalfTensor = [mpsGraph constantWithScalar: 0.5
                                                                   dataType: MPSDataTypeFloat32];
          MPSGraphTensor *betaTensor = [mpsGraph constantWithScalar: beta
                                                           dataType: MPSDataTypeFloat32];
          // 0.5 * beta
          MPSGraphTensor *halfTensorMulBetaTensor = [mpsGraph constantWithScalar: beta * 0.5
                                                                        dataType: MPSDataTypeFloat32];
          // Calculating first part of the equation:
          // ln = 0.5(xn - yn)^2/beta, if |xn - yn| < beta

          // xn - yn
          MPSGraphTensor *diffTensor = [mpsGraph subtractionWithPrimaryTensor: inputTensor
                                                              secondaryTensor: targetTensor
                                                                         name: nil];

          // | xn - yn |
          MPSGraphTensor *diffAbsTensor = [mpsGraph absoluteWithTensor: diffTensor
                                                                  name: nil];

          // | xn - yn | < beta
          MPSGraphTensor *diffAbsLessThanBetaTensor = [mpsGraph lessThanWithPrimaryTensor: diffAbsTensor
                                                                          secondaryTensor: betaTensor
                                                                                     name: nil];

          // ( xn - yn ) ^ 2
          MPSGraphTensor *diffSquare = [mpsGraph squareWithTensor: diffTensor
                                                             name: nil];

          // 0.5 * ( xn - yn ) ^ 2
          MPSGraphTensor *diffSquareMulHalfTensor = [mpsGraph multiplicationWithPrimaryTensor: diffSquare
                                                                              secondaryTensor: mpsGraphHalfTensor
                                                                                         name: nil];

          // 0.5 * ( xn - yn ) ^ 2 / beta
          MPSGraphTensor *loss1Temp = [mpsGraph divisionWithPrimaryTensor: diffSquareMulHalfTensor
                                                          secondaryTensor: betaTensor
                                                                    name: nil];

          // Calculating second part of the equation:
          // | xn - yn | - 0.5 * beta, if | xn - yn | >= beta

          // | xn - yn | - 0.5 * beta
          MPSGraphTensor *loss2Temp = [mpsGraph subtractionWithPrimaryTensor: diffAbsTensor
                                                             secondaryTensor: halfTensorMulBetaTensor
                                                                        name: nil];

          MPSGraphTensor *lossTensor = [mpsGraph selectWithPredicateTensor: diffAbsLessThanBetaTensor
                                                            truePredicateTensor: loss1Temp
                                                           falsePredicateTensor: loss2Temp
                                                                           name: @"lossTensor"];

          MPSGraphTensor *outputTensor = reduceTensor(lossTensor, reduction, mpsGraph, 1);

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->targetTensor_ = targetTensor;
          newCachedGraph->outputTensor_ = outputTensor;

        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }

    Placeholder inputPlaceholder = Placeholder(cachedGraph->inputTensor_, input, mpsInputShape);
    Placeholder targetPlaceholder = Placeholder(cachedGraph->targetTensor_, target, mpsInputShape);
    Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor_, output, mpsOutputShape);

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
      targetPlaceholder.getMPSGraphTensor() : targetPlaceholder .getMPSGraphTensorData()
    };
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }
}

void smooth_l1_loss_template(
    const Tensor &input,
    const Tensor &target,
    const int64_t reduction,
    double beta,
    const Tensor &output)
{
 TORCH_CHECK(beta >= 0, "smooth_l1_loss does not support negative values for beta.");
  TORCH_CHECK(input.is_mps());
  TORCH_CHECK(target.is_mps());

  MPSShape *mpsInputShape = nil;
  MPSShape *mpsOutputShape = nil;

  // Determine the shape of the output
  // If the reduction is 'mean' or 'sum', the output shape is a scalar,
  // otherwise, the output shape is the same shape as input
  if (reduction == Reduction::Mean || reduction == Reduction::Sum)
  {
      // Output: scalar, if reduction is 'mean' or 'sum'
      IntArrayRef input_shape = input.sizes();
      int64_t num_input_dims = input_shape.size();
      NSMutableArray<NSNumber*> *apparent_input_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
      int64_t num_in_elements = 1;
      for(int i = 0; i < num_input_dims; i++) {
          num_in_elements *= input_shape[i];
      }
      apparent_input_shape[0] = [NSNumber numberWithInt:num_in_elements];

      // Output is a single value in case reduction is set to mean or sum
      NSMutableArray<NSNumber*> *apparent_out_shape = [NSMutableArray<NSNumber*> arrayWithCapacity:1];
      apparent_out_shape[0] = @1;
      mpsInputShape = apparent_input_shape;
      mpsOutputShape = apparent_out_shape;
  }
  else
  {
      // Output: If reduction is 'none', then (N, *); same shape as the input
      assert(reduction == Reduction::None);
      mpsInputShape = getMPSShape(input);
      mpsOutputShape = mpsInputShape;
      //resize_tensor(&output);
  }
  TORCH_CHECK(output.is_mps());

  smooth_l1_loss_impl(
      input,
      target,
      reduction,
      beta,
      output,
      mpsInputShape,
      mpsOutputShape);
}

void smooth_l1_loss_backward_impl(
    const Tensor& grad_output,
    const Tensor& input,
    const Tensor& target,
    int64_t reduction,
    double beta,
    Tensor& grad_input)
{
 struct CachedGraph : public MPSCachedGraph
  {
    CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
    MPSGraphTensor *inputTensor_ = nil;
    MPSGraphTensor *targetTensor_ = nil;
    MPSGraphTensor *gradInputTensor_ = nil;
  };

 MPSGraphCache *cache_ = MPSGraphCache::getInstance();

  MPSStream *stream= getCurrentMPSStream();

  @autoreleasepool {

    auto numClasses = grad_input.sizes()[1];
    MPSShape* input_shape = getMPSShape(input);
    NSString* ns_shape_key = [[input_shape valueForKey:@"description"] componentsJoinedByString:@","];

    string key = "smooth_l1_loss_backward_impl:" + to_string(numClasses) + ":" +
                                                   reductionToString(reduction) + ":" +
                                                   [ns_shape_key UTF8String] + ":" +
                                                   to_string(beta) + ":" +
                                                   getMPSTypeString(input.scalar_type()) + ":" +
                                                   getMPSTypeString(target.scalar_type());
    CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
    if(!cachedGraph) {
      MPSCachedGraph *tmpCachedGraph = cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

        CachedGraph *newCachedGraph = nil;

        @autoreleasepool {
          auto numElements = input.numel();

          MPSGraph *mpsGraph = make_mps_graph();
          newCachedGraph = new CachedGraph(mpsGraph);

          MPSGraphTensor *inputTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(input.scalar_type()));
          MPSGraphTensor *targetTensor = mpsGraphUnrankedPlaceHolder(mpsGraph, getMPSDataType(target.scalar_type()));

          MPSGraphTensor *betaTensor = [mpsGraph constantWithScalar: beta
                                                           dataType: MPSDataTypeFloat32];

          MPSGraphTensor *numelTensor = [mpsGraph constantWithScalar: numElements
                                                            dataType: MPSDataTypeFloat32];

          // xn - yn
          MPSGraphTensor *diffTensor = [mpsGraph subtractionWithPrimaryTensor: inputTensor
                                                              secondaryTensor: targetTensor
                                                                         name: nil];

          // | xn - yn |
          MPSGraphTensor *diffAbsTensor = [mpsGraph absoluteWithTensor: diffTensor
                                                                  name: nil];

          // | xn - yn | < beta
          MPSGraphTensor *diffAbsLessThanBetaTensor = [mpsGraph lessThanWithPrimaryTensor: diffAbsTensor
                                                                          secondaryTensor: betaTensor
                                                                                     name: nil];

          // ( xn - yn ) / beta
          MPSGraphTensor *truePredicateTensor = [mpsGraph divisionWithPrimaryTensor: diffTensor
                                                                    secondaryTensor: betaTensor
                                                                               name: nil];

          // ( x - y ) / | x - y |
           MPSGraphTensor *falsePredicateTensor = [mpsGraph divisionWithPrimaryTensor: diffTensor
                                                                      secondaryTensor: diffAbsTensor
                                                                                 name: nil];

          MPSGraphTensor *lossTensor = [mpsGraph selectWithPredicateTensor: diffAbsLessThanBetaTensor
                                                            truePredicateTensor: truePredicateTensor
                                                           falsePredicateTensor: falsePredicateTensor
                                                                           name: @"lossTensor"];

          MPSGraphTensor *outputTensor = lossTensor;
          if (reduction == Reduction::Mean)
          {
              outputTensor = [mpsGraph divisionWithPrimaryTensor: lossTensor
                                                 secondaryTensor: numelTensor
                                                            name: nil];
          }

          MPSGraphTensor *gradInputTensor = outputTensor;

          newCachedGraph->inputTensor_ = inputTensor;
          newCachedGraph->targetTensor_ = targetTensor;
          newCachedGraph->gradInputTensor_ = gradInputTensor;

        }
        return newCachedGraph;
      });
      cachedGraph = static_cast<CachedGraph *>(tmpCachedGraph);
    }
    Placeholder inputPlaceholder = Placeholder(cachedGraph->inputTensor_, input);
    Placeholder targetPlaceholder = Placeholder(cachedGraph->targetTensor_, target);
    Placeholder gradInputPlaceholder = Placeholder(cachedGraph->gradInputTensor_, grad_input);

    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
      inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
      targetPlaceholder.getMPSGraphTensor() : targetPlaceholder .getMPSGraphTensorData()
    };
    NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
      gradInputPlaceholder.getMPSGraphTensor() : gradInputPlaceholder.getMPSGraphTensorData()
    };

    runMPSGraph(stream, cachedGraph->graph(), feeds, results);
  }
}

void smooth_l1_loss_backward_template(
    const Tensor& grad_output,
    const Tensor& input,
    const Tensor& target,
    int64_t reduction,
    double beta,
    Tensor& grad_input)
{
  TORCH_CHECK(beta >= 0, "smooth_l1_loss_backward does not support negative values for beta.");
  TORCH_CHECK(input.is_mps());
  TORCH_CHECK(target.is_mps());

  smooth_l1_loss_backward_impl(
      grad_output, input, target, reduction, beta, grad_input
  );
}

} // namespace mps

// APIs exposed to at::native scope

// MSELoss
TORCH_IMPL_FUNC(mse_loss_out_mps) (
        const Tensor& input, const Tensor& target, int64_t reduction, const Tensor& output) {
    string op_name = __func__;
    using namespace mps;
    TORCH_CHECK(target.is_same_size(input), op_name + ": target and input tensors must have identical shapes")
    TORCH_CHECK(output.is_mps());

    struct CachedGraph : public MPSCachedGraph
    {
        CachedGraph(MPSGraph *graph) : MPSCachedGraph(graph) {}
        MPSGraphTensor* inputTensor = nil;
        MPSGraphTensor* targetTensor = nil;
        MPSGraphTensor* outputTensor = nil;
    };
    MPSGraphCache* cache_ = MPSGraphCache::getInstance();

    @autoreleasepool {
        string key = op_name + reductionToString(reduction) + getTensorsStringKey({input, target});
        CachedGraph* cachedGraph = static_cast<CachedGraph *>(cache_->LookUp(key));
        if(!cachedGraph) {
            cachedGraph = static_cast<CachedGraph*>(cache_->CreateCachedGraph(key, ^ MPSCachedGraph * () {

                CachedGraph *newCachedGraph = nil;

                @autoreleasepool {
                    MPSGraph* mpsGraph = make_mps_graph();
                    newCachedGraph = new CachedGraph(mpsGraph);

                    newCachedGraph->inputTensor = mpsGraphRankedPlaceHolder(mpsGraph, input);
                    newCachedGraph->targetTensor = mpsGraphRankedPlaceHolder(mpsGraph, target);

                    MPSGraphTensor *diffTensor = [mpsGraph subtractionWithPrimaryTensor: newCachedGraph->inputTensor
                                                                        secondaryTensor: newCachedGraph->targetTensor
                                                                                   name: nil];
                    MPSGraphTensor *diffSquareTensor = [mpsGraph squareWithTensor: diffTensor
                                                                             name: nil];
                    newCachedGraph->outputTensor = reduceTensor(diffSquareTensor, reduction, mpsGraph, input.sizes().size());
                }
                return newCachedGraph;
            }));
        }
        Placeholder inputPlaceholder  = Placeholder(cachedGraph->inputTensor, input);
        Placeholder targetPlaceholder = Placeholder(cachedGraph->targetTensor, target);
        Placeholder outputPlaceholder = Placeholder(cachedGraph->outputTensor, output);

        NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* feeds = @{
            inputPlaceholder.getMPSGraphTensor() : inputPlaceholder.getMPSGraphTensorData(),
            targetPlaceholder.getMPSGraphTensor() : targetPlaceholder.getMPSGraphTensorData()
        };
        NSDictionary<MPSGraphTensor*, MPSGraphTensorData*>* results = @{
            outputPlaceholder.getMPSGraphTensor() : outputPlaceholder.getMPSGraphTensorData()
        };

        runMPSGraph(getCurrentMPSStream(), cachedGraph->graph(), feeds, results);
    }
}

Tensor& mse_loss_backward_out_mps(const Tensor& grad_output, const Tensor& input,
                                  const Tensor& target, int64_t reduction, Tensor& grad_input)
{
    return mps::mse_loss_backward_out_impl(grad_output, input, target, reduction, grad_input, __func__);
}

Tensor mse_loss_backward_mps(const Tensor& grad_output, const Tensor& input,
                             const Tensor& target, int64_t reduction)
{
    Tensor grad_input = at::zeros_like(input, LEGACY_CONTIGUOUS_MEMORY_FORMAT);
    return mps::mse_loss_backward_out_impl(grad_output, input, target, reduction, grad_input, __func__);
}

// BCELoss
Tensor& binary_cross_entropy_out_mps(const Tensor& input, const Tensor& target,
                                     const c10::optional<Tensor>& weight_opt, int64_t reduction, Tensor& loss)
{
    return mps::BCELoss::bce_loss_out_impl(input, target, weight_opt, reduction, loss, c10::nullopt, __func__);
}

Tensor binary_cross_entropy_mps(const Tensor& input, const Tensor& target,
                                const c10::optional<Tensor>& weight_opt, int64_t reduction)
{
    Tensor loss = at::empty_like(input);
    return mps::BCELoss::bce_loss_out_impl(input, target, weight_opt, reduction, loss, c10::nullopt, __func__);
}

Tensor& binary_cross_entropy_backward_out_mps(const Tensor& grad_output, const Tensor& input,
                                              const Tensor& target, const c10::optional<Tensor>& weight_opt,
                                              int64_t reduction, Tensor& grad_input)
{
    return mps::BCELoss::bce_loss_out_impl(input, target, weight_opt, reduction, grad_input, grad_output, __func__);
}

Tensor binary_cross_entropy_backward_mps(const Tensor& grad_output, const Tensor& input, const Tensor& target,
                                         const c10::optional<Tensor>& weight_opt, int64_t reduction)
{
    Tensor grad_input = at::empty_like(input);
    return mps::BCELoss::bce_loss_out_impl(input, target, weight_opt, reduction, grad_input, grad_output, __func__);
}

// SmoothL1Loss
TORCH_IMPL_FUNC(smooth_l1_loss_out_mps)(
                    const Tensor& input,
                    const Tensor& target,
                    int64_t reduction,
                    double beta,
                    const Tensor& result) {
  mps::smooth_l1_loss_template(
      input, target, reduction, beta, result);
}

Tensor& smooth_l1_loss_backward_out_mps(
    const Tensor& grad_output,
    const Tensor& input,
    const Tensor& target,
    int64_t reduction,
    double beta,
    Tensor& grad_input) {
  mps::smooth_l1_loss_backward_template(
      grad_output, input, target, reduction, beta, grad_input);
  return grad_input;
}

// NLLLoss
TORCH_IMPL_FUNC(nll_loss_backward_out_mps)
(const Tensor& grad_output,
 const Tensor& self,
 const Tensor& target,
 OptionalTensorRef weight_opt,
 int64_t reduction,
 int64_t ignore_index,
 const Tensor& total_weight,
 const Tensor& grad_input
)
{
    const Tensor& weight = weight_opt.getTensorRef();

    mps::nllnd_loss_backward_impl((Tensor &)grad_input,
                             grad_output,
                             self,
                             target,
                             weight,
                             reduction,
                             ignore_index,
                             total_weight,
                             false);
    return;
}

TORCH_IMPL_FUNC(nll_loss_forward_out_mps)
(const Tensor& self,
 const Tensor& target,
 const OptionalTensorRef weight_opt,
 int64_t reduction,
 int64_t ignore_index,
 const Tensor& output,
 const Tensor& total_weight) {

    const Tensor& weight = weight_opt.getTensorRef();

    mps::nllnd_loss_forward_impl((Tensor &)output,
                            (Tensor &)total_weight,
                             self,
                             target,
                             weight,
                             reduction,
                             ignore_index,
                             false);

    return;
}

inline void check_inputs_nll_loss2d(
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight) {
  TORCH_CHECK(
      target.dim() == 3,
      "only batches of spatial targets supported (3D tensors)"
      " but got targets of dimension: ",
      target.dim());
  TORCH_CHECK(
      input.dim() == 4,
      "only batches of spatial inputs supported (4D tensors), "
      "but got input of dimension: ",
      input.dim());
  TORCH_CHECK(
      !weight.defined() || weight.numel() == input.size(1),
      "weight tensor should be defined either for all or no classes");

  const int64_t input0 = input.size(0);
  const int64_t input2 = input.size(2);
  const int64_t input3 = input.size(3);
  const int64_t target0 = target.size(0);
  const int64_t target1 = target.size(1);
  const int64_t target2 = target.size(2);
  TORCH_CHECK(
      input0 == target0 && input2 == target1 && input3 == target2,
      "size mismatch (got input: ",
      input.sizes(),
      " , target: ",
      target.sizes());
}


void nll_loss2d_forward_out_mps_template(
    Tensor& output,
    Tensor& total_weight,
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index) {
  check_inputs_nll_loss2d(input, target, weight);
  total_weight.resize_({});

    mps::nllnd_loss_forward_impl(output,
                         total_weight,
                         input,
                         target,
                         weight,
                         reduction,
                         ignore_index,
                         true);

    return;
}

std::tuple<Tensor&, Tensor&> nll_loss2d_forward_out_mps(const Tensor& self,
    const Tensor& target, const c10::optional<Tensor>& weight_opt,
    int64_t reduction,
    int64_t ignore_index,
    Tensor& output,
    Tensor& total_weight) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  nll_loss2d_forward_out_mps_template(
      output, total_weight, self, target, weight, reduction, ignore_index);
  return std::tuple<Tensor&, Tensor&>(output, total_weight);
}

std::tuple<Tensor, Tensor> nll_loss2d_forward_mps(
    const Tensor& self,
    const Tensor& target, const c10::optional<Tensor>& weight_opt,
    int64_t reduction,
    int64_t ignore_index) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  auto output = at::empty({0}, self.options());
  auto total_weight = at::empty({0}, self.options());
  at::native::nll_loss2d_forward_out_mps(
      self, target, weight, reduction, ignore_index, output, total_weight);
  return std::make_tuple(output, total_weight);
}

void nll_loss2d_backward_out_mps_template(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& input,
    const Tensor& target,
    const Tensor& weight,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {
  check_inputs_nll_loss2d(input, target, weight);
  grad_input.resize_as_(input);
  grad_input.zero_();
  TORCH_CHECK(grad_input.is_contiguous(), "grad_input must be contiguous");
  TORCH_CHECK(
      total_weight.numel() == 1,
      "expected total_weight to be a single element tensor, got: ",
      total_weight.sizes(),
      " (",
      total_weight.numel(),
      " elements)");

    mps::nllnd_loss_backward_impl(grad_input,
                             grad_output,
                             input,
                             target,
                             weight,
                             reduction,
                             ignore_index,
                             total_weight,
                             true);

    return;
}

Tensor& nll_loss2d_backward_out_mps(const Tensor& grad_output,
    const Tensor& self,
    const Tensor& target, const c10::optional<Tensor>& weight_opt,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight,
    Tensor& grad_input) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  nll_loss2d_backward_out_mps_template(
      grad_input,
      grad_output,
      self,
      target,
      weight,
      reduction,
      ignore_index,
      total_weight);
  return grad_input;
}

Tensor nll_loss2d_backward_mps(
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& target, const c10::optional<Tensor>& weight_opt,
    int64_t reduction,
    int64_t ignore_index,
    const Tensor& total_weight) {

  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  auto grad_input = at::zeros_like(self);
  nll_loss2d_backward_out_mps(
      grad_output,
      self,
      target,
      weight,
      reduction,
      ignore_index,
      total_weight,
      grad_input);
  return grad_input;
}


} // namespace native
} // namespace at
