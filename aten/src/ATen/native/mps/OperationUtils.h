//  Copyright © 2022 Apple Inc.

#include <ATen/ATen.h>
#include <ATen/Tensor.h>
#include <ATen/Utils.h>
#include <ATen/mps/MPSStream.h>
#include <ATen/native/mps/TensorFactory.h>
#include <c10/core/ScalarType.h>
#include <torch/library.h>
#include <unordered_map>

#ifdef __OBJC__
#include <MetalPerformanceShaders/MetalPerformanceShaders.h>
#endif

using namespace at::mps;

namespace at {
namespace native {
namespace mps {

struct TORCH_CUDA_CPP_API MPSGeneratorImpl : public c10::GeneratorImpl {
  MPSGeneratorImpl(DeviceIndex device_index = -1);
  ~MPSGeneratorImpl() = default;

  void set_current_seed(uint64_t seed) override;
  uint64_t current_seed() const override;
  uint64_t seed() override;
  void set_state(const c10::TensorImpl& new_state) override;
  c10::intrusive_ptr<c10::TensorImpl> get_state() const override;
  static DeviceType device_type();

private:
  MPSGeneratorImpl* clone_impl() const override;
  uint64_t seed_ = default_rng_seed_val;
};

const Generator& getDefaultMPSGenerator();

void runMPSGraph(
    MPSStream* mpsStream,
    MPSGraph* mpsGraph,
    NSDictionary* feeds,
    NSDictionary* results);

MPSDataType getMPSDataType(ScalarType scalar_type);
MPSDataType getMPSScalarType(ScalarType scalar_type);
std::string getMPSTypeString(ScalarType scalar_type);
std::string getMPSShapeString(MPSShape* shape);
std::string getTensorsStringKey(const TensorList& tensors, bool use_scalar_value = true);
double getMPSScalarValue(const Tensor& t);
std::string getArrayRefString(const IntArrayRef s);
std::string getStridedKey(const Tensor& self, const IntArrayRef sz,
                          const IntArrayRef strides, int64_t offset);
id<MTLBuffer> gatherViewTensor(const at::Tensor& src, id<MTLBuffer> s);

MPSShape* getMPSShape(const Tensor& t);
MPSShape* getMPSShape(IntArrayRef sizes);
MPSShape* getMPSShape(c10::MaybeOwned<Tensor> t);

class Placeholder {
 public:
  Placeholder() : _placeholder(nullptr), _value(nullptr) {}
  Placeholder(MPSGraphTensor* mpsGraphTensor) : _placeholder(mpsGraphTensor), _value(nullptr) {}
  Placeholder(MPSGraphTensor* mpsGraphTensor, const Tensor& self, MPSShape *mpsShape = nullptr);
  MPSGraphTensor* getMPSGraphTensor() {
    return _placeholder;
  }
  MPSGraphTensorData* getMPSGraphTensorData() {
    return _value;
  }
  bool isIntermediate() {
    return _value == nullptr;
  }

  void allocateViewTensor(const at::Tensor& src)
  {
    assert (!_viewOutput.numel());
    _viewOutput = at::native::empty_mps(
                  src.sizes(),
                  src.scalar_type(),
                  c10::nullopt,
                  kMPS,
                  c10::nullopt,
                  c10::nullopt);
  }

 private:
  MPSGraphTensor* _placeholder;
  MPSGraphTensorData* _value;
  Tensor _viewOutput;
};

void resize_tensor(Tensor* output);
MPSGraphTensor* trunc_tensor(MPSGraph* mpsGraph, MPSGraphTensor* inputTensor);
MPSGraphTensorData *getMPSGraphTensorData(MPSGraph* mpsGraph, MPSStream* mpsStream, const Tensor& tensor);
MPSGraphTensorData* getMPSGraphTensorFromScalar(MPSStream* mpsStream, const Scalar& scalar, MPSDataType dataType);

MPSGraph* make_mps_graph();
void printTensorNDArray(const Tensor& t);

MPSGraphTensor* mpsGraphUnrankedPlaceHolder(MPSGraph *mpsGraph, MPSDataType dataType);
MPSGraphTensor* mpsGraphRankedPlaceHolder(MPSGraph *mpsGraph, MPSDataType dataType, MPSShape* mpsShape);
MPSGraphTensor* mpsGraphRankedPlaceHolder(MPSGraph *mpsGraph, const Tensor& tensor);
MPSGraphTensor* mpsGraphConstantPlaceHolder(MPSGraph *mpsGraph, const double value, MPSShape* mpsShape, MPSDataType dataType);

string get_mem_format_string(c10::MemoryFormat memory_format);

using MPSCacheKey = int64_t;

// derive this class to cache a graph and its inputs/ouputs
// can be used to store any NSObject
struct MPSCachedGraph
{
  MPSCachedGraph(NSObject *object) : _object([object retain]) {}
  virtual ~MPSCachedGraph() {
   [_object release];
   _object = nullptr;
  }
  MPSGraph *graph() const { return (MPSGraph *)_object; }
  NSObject *object() const { return _object; }
private:
  NSObject *_object = nullptr;
};

// TODO: Improve the overall design of MPSGraphCache.
// https://github.com/pytorch/pytorch/issues/77176
// Cache holding various keys mapped to graphs

struct MPSGraphCache
{
  typedef MPSCachedGraph * (^CreateCachedGraphBlock)();

  struct CacheEntry {
    CacheEntry(std::string key, MPSCachedGraph *cachedGraph) : cachedGraph_(cachedGraph), key_(key) {}
    MPSCachedGraph* cachedGraph_ = nullptr;
    std::string key_ = nullptr;
  };

 public:

  static MPSGraphCache* getInstance() {
    if(_instance_cache == nullptr) {
      _instance_cache = new MPSGraphCache();
    }
    return _instance_cache;
  }

  ~MPSGraphCache() {
    dispatch_release(serialQueue_);

    for (auto i : cache_) {
      delete i.second.cachedGraph_;
    }
  }

  // Disallow the copy constructor and operator= functions
  MPSGraphCache(const MPSGraphCache&) = delete;
  void operator=(const MPSGraphCache&) = delete;

  MPSCachedGraph* CreateCachedGraph(const std::string& key, CreateCachedGraphBlock createCacheBlock) {

    __block MPSCachedGraph * result = nil;

    MPSCacheKey hash = std::hash<std::string>{}(key);

    dispatch_sync(serialQueue_, ^() {

      // verify the cached entry doesn't already exist
      if (cache_.count(hash) != 0) {
        auto& entry = cache_.at(hash);
        TORCH_INTERNAL_ASSERT_DEBUG_ONLY(key == entry.key_, "Key collision in the MPS cached graph!\n");
        result = entry.cachedGraph_;
      }
      else {
        result = createCacheBlock();
        CacheEntry entry(key, result);
        cache_.emplace(hash, entry);
      }
    });
    return result;
  }

  MPSCachedGraph* LookUp(const std::string& key) const {

    __block MPSCachedGraph* result = nullptr;

    MPSCacheKey hash = std::hash<std::string>{}(key);

    dispatch_sync(serialQueue_, ^() {

      if (cache_.count(hash) != 0) {
        auto& entry = cache_.at(hash);
        TORCH_INTERNAL_ASSERT_DEBUG_ONLY(key == entry.key_, "Key collision in the MPS cached graph!\n");
        result = entry.cachedGraph_;
      }
    });
    return result;
  }
 private:
  MPSGraphCache() {
    serialQueue_ = dispatch_queue_create("cache queue", DISPATCH_QUEUE_SERIAL);
  }

  static MPSGraphCache* _instance_cache;
  std::unordered_map<MPSCacheKey, CacheEntry> cache_;
  dispatch_queue_t serialQueue_ = nullptr;

};

} // namespace mps
} // namespace native
} // namespace at
