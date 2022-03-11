#include <torch/csrc/profiler/kineto_shim.h>

#include <type_traits>

#ifdef USE_KINETO
#include <libkineto.h>
#endif

namespace torch {
namespace profiler {
namespace impl {
namespace kineto {

// Here lies pain and `#ifdef USE_KINETO`

#ifdef USE_KINETO
namespace {
const std::set<libkineto::ActivityType> cpuTypes{
    libkineto::ActivityType::CPU_OP,
    libkineto::ActivityType::CPU_INSTANT_EVENT,
    libkineto::ActivityType::USER_ANNOTATION,
    libkineto::ActivityType::EXTERNAL_CORRELATION,
    libkineto::ActivityType::CUDA_RUNTIME,
    libkineto::ActivityType::PYTHON_FUNCTION,
};

const std::set<libkineto::ActivityType> cudaTypes = {
    libkineto::ActivityType::GPU_MEMCPY,
    libkineto::ActivityType::GPU_MEMSET,
    libkineto::ActivityType::CONCURRENT_KERNEL,
    // CUDA_RUNTIME appears in both cpuTypes and cudaTypes.
    libkineto::ActivityType::CUDA_RUNTIME,
};
} // namespace
#endif // USE_KINETO

static_assert(
    std::is_pod<DeviceAndResource>::value,
    "Kineto specific details should be in `kineto_ids`.");

const DeviceAndResource kineto_ids() {
#ifdef USE_KINETO
  return {
      /*device=*/libkineto::processId(),
      /*resource=*/libkineto::systemThreadId()};
#else
  return {};
#endif // USE_KINETO
}

TraceWrapper::TraceWrapper(const int64_t start_time, const std::string& name)
#ifdef USE_KINETO
    : cpu_trace_(std::make_unique<libkineto::CpuTraceBuffer>()) {
  cpu_trace_->span.startTime = start_time;
  cpu_trace_->gpuOpCount = -1;
  cpu_trace_->span.name = name;
}
#else
{
}
#endif // USE_KINETO

void TraceWrapper::addCPUActivity(
    const std::string& name,
    const DeviceAndResource device_and_resource,
    const uint64_t correlation_id,
    const int64_t start_time,
    const int64_t end_time) {
#ifdef USE_KINETO
  TORCH_CHECK((bool)(*this), "Cannot add event to non-existent trace.");
  cpu_trace_->activities.emplace_back(libkineto::GenericTraceActivity(
    cpu_trace_->span, libkineto::ActivityType::CPU_OP, name));
  auto& act = cpu_trace_->activities.back();
  act.device = device_and_resource.device;
  act.resource = device_and_resource.resource;
  act.id = correlation_id;
  act.startTime = start_time;
  act.endTime = end_time;
#endif // USE_KINETO
}

void TraceWrapper::addMemoryUsageActivity(
    const std::string& name,
    const DeviceAndResource device_and_resource,
    const int64_t time,
    const c10::Device device,
    const void* ptr,
    const int64_t alloc_size,
    const int64_t total_allocated,
    const int64_t total_reserved) {
#ifdef USE_KINETO
  TORCH_CHECK((bool)(*this), "Cannot add event to non-existent trace.");
  cpu_trace_->activities.emplace_back(libkineto::GenericTraceActivity(
    cpu_trace_->span, libkineto::ActivityType::CPU_INSTANT_EVENT, name));
  auto& act = cpu_trace_->activities.back();
  act.device = device_and_resource.device;
  act.resource = device_and_resource.resource;
  act.startTime = time;
  act.addMetadata("Device Type", std::to_string((int8_t)device.type()));
  act.addMetadata("Device Id", std::to_string(device.index()));
  act.addMetadata("Addr", std::to_string(reinterpret_cast<intptr_t>(ptr)));
  act.addMetadata("Bytes", std::to_string(alloc_size));
  if (total_allocated >= 0) {
    act.addMetadata("Total Allocated", std::to_string(total_allocated));
  }
  if (total_reserved >= 0) {
    act.addMetadata("Total Reserved", std::to_string(total_reserved));
  }
#endif // USE_KINETO
}

void TraceWrapper::transferCpuTrace(int64_t end_time) {
#ifdef USE_KINETO
  cpu_trace_->span.endTime = end_time;
  libkineto::api().activityProfiler().transferCpuTrace(std::move(cpu_trace_));
#endif // USE_KINETO
}

TraceWrapper::operator bool() const {
#ifdef USE_KINETO
  return cpu_trace_ != nullptr;
#else
  return false;
#endif // USE_KINETO
}

ActivityTraceWrapper::ActivityTraceWrapper(
    std::unique_ptr<interface_trace_t> trace)
    : trace_(std::move(trace)), saved_{false} {}

ActivityTraceWrapper::operator bool() const {
#ifdef USE_KINETO
  return trace_ != nullptr;
#else
  return false;
#endif // USE_KINETO
}

void ActivityTraceWrapper::save(const std::string& path) {
#ifdef USE_KINETO
  TORCH_CHECK(!saved_, "Trace is already saved.");
  TORCH_CHECK(trace_ != nullptr, "Missing trace.")
  trace_->save(path);
  saved_ = true;
#else
  TORCH_CHECK(
      false,
      "Saving a trace requires using torch.profiler with Kineto support (USE_KINETO=1)");
#endif // USE_KINETO
}

void prepareTrace(const bool cpuOnly, const ActivitySet& activities) {
#ifdef USE_KINETO
  if (!libkineto::api().isProfilerRegistered()) {
    libkineto_init(/*cpuOnly=*/cpuOnly, /*logOnError=*/true);
    libkineto::api().suppressLogMessages();
  }

  if (!libkineto::api().isProfilerInitialized()) {
    libkineto::api().initProfilerIfRegistered();
  }

  std::set<libkineto::ActivityType> k_activities;
  if (activities.count(torch::autograd::profiler::ActivityType::CPU)) {
    k_activities.insert(cpuTypes.begin(), cpuTypes.end());
  }
  if (activities.count(torch::autograd::profiler::ActivityType::CUDA)) {
    k_activities.insert(cudaTypes.begin(), cudaTypes.end());
  }

  libkineto::api().activityProfiler().prepareTrace(k_activities);
#endif // USE_KINETO
}

void startTrace() {
#ifdef USE_KINETO
  libkineto::api().activityProfiler().startTrace();
#endif // USE_KINETO
}

ActivityTraceWrapper stopTrace() {
  return ActivityTraceWrapper{
#ifdef USE_KINETO
      libkineto::api().activityProfiler().stopTrace()
#else
      std::make_unique<interface_trace_t>()
#endif // USE_KINETO
  };
}

void pushCorrelationId(uint64_t correlation_id) {
#ifdef USE_KINETO
  libkineto::api().activityProfiler().pushCorrelationId(correlation_id);
#endif // USE_KINETO
}

void popCorrelationId() {
#ifdef USE_KINETO
  libkineto::api().activityProfiler().popCorrelationId();
#endif // USE_KINETO
}

void recordThreadInfo() {
#ifdef USE_KINETO
  libkineto::api().activityProfiler().recordThreadInfo();
#endif // USE_KINETO
}

} // namespace kineto
} // namespace impl
} // namespace profiler

namespace autograd {
namespace profiler {
#ifdef USE_KINETO
c10::DeviceType deviceTypeFromActivity(libkineto::ActivityType activity_type) {
  // fallthrough
  switch (activity_type) {
    case libkineto::ActivityType::GPU_MEMCPY:
    case libkineto::ActivityType::GPU_MEMSET:
    case libkineto::ActivityType::CONCURRENT_KERNEL:
    case libkineto::ActivityType::GPU_USER_ANNOTATION:
      return c10::DeviceType::CUDA;
    case libkineto::ActivityType::CPU_OP:
    case libkineto::ActivityType::USER_ANNOTATION:
    case libkineto::ActivityType::EXTERNAL_CORRELATION:
    case libkineto::ActivityType::CUDA_RUNTIME:
    case libkineto::ActivityType::CPU_INSTANT_EVENT:
    case libkineto::ActivityType::GLOW_RUNTIME:
    case libkineto::ActivityType::PYTHON_FUNCTION:
      return c10::DeviceType::CPU;
    default: {
      LOG(WARNING) << "Unknown activity type (" << (uint8_t)activity_type
                   << "), assuming CPU device";
      return c10::DeviceType::CPU;
    }
  }
}
#endif // USE_KINETO

void addMetadataJson(const std::string& key, const std::string& value) {
#ifdef USE_KINETO
  if (libkineto::api().isProfilerInitialized()) {
    libkineto::api().activityProfiler().addMetadata(key, value);
  } else {
    LOG(WARNING) << "Profiler is not initialized: skipping profiling metadata";
  }
#else
  LOG(WARNING) << "Adding profiling metadata requires using "
               << "torch.profiler with Kineto support (USE_KINETO=1)";
#endif // USE_KINETO
}

} // namespace profiler
} // namespace autograd
} // namespace torch
