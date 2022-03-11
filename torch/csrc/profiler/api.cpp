#include <torch/csrc/profiler/api.h>

namespace torch {
namespace profiler {
namespace impl {

namespace {
enum ProfilerIValueIdx {
  STATE = 0,
  REPORT_INPUT_SHAPES,
  PROFILE_MEMORY,
  NUM_PROFILER_CFG_IVALUE_IDX // must be last in list
};
} // namespace

at::IValue ProfilerConfig::toIValue() const {
  c10::impl::GenericList eventIValueList(at::AnyType::get());
  eventIValueList.reserve(NUM_PROFILER_CFG_IVALUE_IDX);
  eventIValueList.emplace_back(static_cast<int64_t>(state));
  eventIValueList.emplace_back(report_input_shapes);
  eventIValueList.emplace_back(profile_memory);
  return eventIValueList;
}

ProfilerConfig ProfilerConfig::fromIValue(
    const at::IValue& profilerConfigIValue) {
  TORCH_INTERNAL_ASSERT(
      profilerConfigIValue.isList(),
      "Expected IValue to contain type c10::impl::GenericList");
  auto ivalues = profilerConfigIValue.toList();
  TORCH_INTERNAL_ASSERT(
      ivalues.size() == NUM_PROFILER_CFG_IVALUE_IDX,
      c10::str(
          "Expected exactly ",
          NUM_PROFILER_CFG_IVALUE_IDX,
          " ivalues to resconstruct ProfilerConfig."));
  return ProfilerConfig(
      static_cast<ProfilerState>(ivalues.get(ProfilerIValueIdx::STATE).toInt()),
      ivalues.get(ProfilerIValueIdx::REPORT_INPUT_SHAPES).toBool(),
      ivalues.get(ProfilerIValueIdx::PROFILE_MEMORY).toBool());
}

bool profilerEnabled() {
  auto state_ptr = ProfilerThreadLocalStateBase::getTLS();
  return state_ptr &&
      state_ptr->config().state !=
      torch::profiler::impl::ProfilerState::Disabled;
}

TORCH_API ActiveProfilerType profilerType() {
  auto state_ptr = ProfilerThreadLocalStateBase::getTLS();
  return state_ptr == nullptr
      ? ActiveProfilerType::NONE
      : state_ptr->profilerType();
}

torch::profiler::impl::ProfilerConfig getProfilerConfig() {
  auto state_ptr = ProfilerThreadLocalStateBase::getTLS();
  TORCH_CHECK(
      state_ptr,
      "Tried to access profiler config, but profiler is not enabled!");
  return state_ptr->config();
}

CUDAStubs::~CUDAStubs() = default;

namespace {
struct DefaultCUDAStubs : public CUDAStubs {
  void record(int* /*device*/, CUDAEventStub* /*event*/, int64_t* /*cpu_ns*/)
      const override {
    fail();
  }
  float elapsed(const CUDAEventStub* /*event*/, const CUDAEventStub* /*event2*/)
      const override {
    fail();
    return 0.f;
  }
  void nvtxMarkA(const char* /*name*/) const override {
    fail();
  }
  void nvtxRangePushA(const char* /*name*/) const override {
    fail();
  }
  void nvtxRangePop() const override {
    fail();
  }
  bool enabled() const override {
    return false;
  }
  void onEachDevice(std::function<void(int)> /*op*/) const override {
    fail();
  }
  void synchronize() const override {
    fail();
  }
  ~DefaultCUDAStubs() override = default;

 private:
  void fail() const {
    AT_ERROR("CUDA used in profiler but not enabled.");
  }
};

const DefaultCUDAStubs default_stubs;
constexpr const DefaultCUDAStubs* default_stubs_addr = &default_stubs;
// Constant initialization, so it is guaranteed to be initialized before
// static initialization calls which may invoke registerCUDAMethods
inline const CUDAStubs*& cuda_stubs() {
  static const CUDAStubs* stubs_ =
      static_cast<const CUDAStubs*>(default_stubs_addr);
  return stubs_;
}
} // namespace

const CUDAStubs* cudaStubs() {
  return cuda_stubs();
}

void registerCUDAMethods(CUDAStubs* stubs) {
  cuda_stubs() = stubs;
}

} // namespace impl
} // namespace profiler
} // namespace torch
