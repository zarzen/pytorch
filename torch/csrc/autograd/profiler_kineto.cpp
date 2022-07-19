#define TORCH_ASSERT_ONLY_METHOD_OPERATORS
#include <torch/csrc/autograd/profiler_kineto.h>

#include <c10/macros/Export.h>
#include <c10/util/flat_hash_map.h>
#include <c10/util/irange.h>
#include <c10/util/overloaded.h>
#include <c10/util/variant.h>
#include <c10/util/C++17.h>

#include <torch/csrc/profiler/api.h>
#include <torch/csrc/profiler/collection.h>
#include <torch/csrc/profiler/containers.h>
#include <torch/csrc/profiler/kineto_shim.h>
#include <torch/csrc/profiler/nvtx_observer.h>

#include <ATen/Context.h>

#include <deque>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifdef USE_KINETO
#include <libkineto.h>
#include <time_since_epoch.h>

#ifndef _MSC_VER
// TODO: TO be removed, once this properly works from libkineto
// Literal copy-n-paste from third_party/kineto/libkineto/src/WeakSymbols.cpp
extern "C" {
// This function is needed to avoid superfluous dependency on GNU OpenMP library
// when cuPTI is linked statically For more details see
// https://github.com/pytorch/pytorch/issues/51026
__attribute__((weak)) int acc_get_device_type() {
  throw std::runtime_error(
      "Dummy implementation of acc_get_device_type is not supposed to be called!");
}
} // extern "C"
#endif // _MSC_VER
#endif // USE_KINETO

namespace torch {
namespace autograd {
namespace profiler {

namespace {
const std::string kMemoryEventName = "[memory]";
// TODO: consider TLS (tid + tls counter)
uint64_t next_correlation_id() {
  static std::atomic<uint64_t> corr_id_{1};
  return corr_id_++;
}

inline int64_t getTimeUs() {
#ifdef USE_KINETO
  return libkineto::timeSinceEpoch(std::chrono::system_clock::now());
#else
  return torch::profiler::impl::getTime() / 1000;
#endif // USE_KINETO
}
} // namespace

namespace python_tracer {
namespace {
CallFn call_fn;
TraceEventsFn get_events_fn;
} // namespace

void registerFunctions(CallFn call, TraceEventsFn get_events) {
  call_fn = call;
  get_events_fn = get_events;
}

void call(Command c) {
  if (call_fn != nullptr) {
    call_fn(c);
  }
}

std::vector<std::unique_ptr<PyTraceEvent>> get_events() {
  return get_events_fn != nullptr
      ? get_events_fn()
      : std::vector<std::unique_ptr<PyTraceEvent>>();
}

// We do not want `getTimeUs` to be directly visible, but we need a way for
// the python tracer to use the same timing convention as the profiler.
int64_t now() {
  return getTimeUs();
}

struct Replay {
  PyTraceEvent* frame_;
  bool enter_;

  C10_NODISCARD int64_t t() const {
    return enter_ ? frame_->startTime_ : frame_->endTime_;
  }

  C10_NODISCARD size_t idx() const {
    return enter_ ? frame_->call_idx_ : frame_->return_idx_;
  }

  bool operator<(const Replay& other) const {
    return idx() < other.idx();
  }
};

void _push_reverse_order(PyTraceEvent* e, std::vector<std::string>& names) {
  if (e != nullptr) {
    _push_reverse_order(e->parent_, names);
    names.push_back(e->name_);
  }
}
} // namespace python_tracer

namespace {
using torch::profiler::impl::ProfilerThreadLocalStateBase;
using torch::profiler::impl::ActiveProfilerType;
using torch::profiler::impl::Result;
using torch::profiler::impl::kineto::annotation_t;
using torch::profiler::impl::shapesToStr;
using torch::profiler::impl::dtypesToStr;
using torch::profiler::impl::stacksToStr;

struct MemoryEventData {
  torch::profiler::impl::approx_time_t start_time;
  void* ptr;
  int64_t alloc_size;
  int64_t total_allocated;
  int64_t total_reserved;
  uint64_t threadID;
  torch::profiler::impl::kineto::DeviceAndResource kineto_info;
  c10::DeviceType device_type;
  c10::DeviceIndex device_index;
};
static_assert(std::is_pod<MemoryEventData>::value, "Non-POD member of MemoryEventData.");

struct EventFieldsVisitor {
  EventFieldsVisitor(const Result& result, KinetoEvent& kineto_event)
      : result_{result}, kineto_event_{kineto_event} {
    handleJIT(result_.get().jit_stack_, result_.get().jit_modules_);
    c10::visit(*this, result.event_);
  }

  void operator()(const torch::profiler::impl::OpEvent& op_event) {
    kineto_event_.get()
        .endThreadId(op_event.end_thread_id_)
        .scope(op_event.record_function_scope_)
        .setAsync(op_event.is_async_)
        .debugHandle(op_event.debug_handle_);

    auto& shapes = result_.get().inputs_.shapes_;
    if (!shapes.empty()) {
      kineto_event_.get().shapes(shapes);
      annotations_.emplace_back("Input Dims", shapesToStr(shapes));
    }

    auto& dtypes = result_.get().inputs_.dtypes_;
    if (!dtypes.empty()) {
      kineto_event_.get().dtypes(dtypes);
      annotations_.emplace_back("Input type", dtypesToStr(dtypes));
    }

    if (!result_.get().extra_args_.empty()) {
      kineto_event_.get().flops(
          computeFlops(result_.get().name(), result_.get().extra_args_));
    }
    kineto_event_.get().cuda_event_start_ =
        result_.get().gpu_fallback_.cuda_event_start_;
    kineto_event_.get().cuda_event_end_ =
        result_.get().gpu_fallback_.cuda_event_end_;

    // add information about an associated forward op, if a sequence number
    // is available (e.g. during training)
    if (op_event.sequence_number_ >= 0) {
      kineto_event_.get()
          .sequenceNr(op_event.sequence_number_)
          .fwdThreadId(op_event.forward_thread_id_);
      annotations_.emplace_back(
          "Fwd thread id", std::to_string(op_event.forward_thread_id_));
      annotations_.emplace_back(
          "Sequence number", std::to_string(op_event.sequence_number_));
    }
  }

  void operator()(const torch::profiler::impl::BackendEvent& backend_event) {
    kineto_event_.get()
        .endThreadId(result_.get().start_tid_)
        .scope(backend_event.record_function_scope_)
        .debugHandle(backend_event.debug_handle_)
        .backend(backend_event.backend_);

    if (!backend_event.backend_.empty()) {
      annotations_.emplace_back(
          "Backend", "\"" + backend_event.backend_ + "\"");
    }
  }

  void handleJIT(
      const std::vector<std::string>& jit_stack,
      const std::vector<std::string>& jit_modules) {
    if (!jit_stack.empty()) {
      // NB: This is only for the JIT stack. The python stack (if applicable)
      //     is constructed later.
      kineto_event_.get().stack(jit_stack);
      annotations_.emplace_back(
          "Call stack", torch::profiler::impl::stacksToStr(jit_stack, ";"));
    }

    if (!jit_modules.empty()) {
      kineto_event_.get().moduleHierarchy(jit_modules);
      annotations_.emplace_back(
          "Module Hierarchy",
          torch::profiler::impl::stacksToStr(jit_modules, "."));
    }
  }

  std::reference_wrapper<const Result> result_;
  std::reference_wrapper<KinetoEvent> kineto_event_;
  annotation_t annotations_;
};

auto getAnnotations(const MemoryEventData& event) {
  torch::profiler::impl::kineto::annotation_t out{
      {"Device Type", std::to_string((int8_t)event.device_type)},
      {"Device Id", std::to_string(event.device_index)},
      {"Addr", std::to_string(reinterpret_cast<intptr_t>(event.ptr))},
      {"Bytes", std::to_string(event.alloc_size)}};

  if (event.total_allocated >= 0) {
    out.emplace_back("Total Allocated", std::to_string(event.total_allocated));
  }
  if (event.total_reserved >= 0) {
    out.emplace_back("Total Reserved", std::to_string(event.total_reserved));
  }
  return out;
}

// Assumption: Total threads number will not exceed 2^16-1, and total ops will
// not exceed 2^48 -1.
static inline uint64_t getForwardThreadKey(uint64_t tid, uint64_t seqNr) {
  return (((tid) << 48) | ((seqNr) & (((uint64_t)1 << 48) - 1)));
}

struct KinetoThreadLocalState : public ProfilerThreadLocalStateBase {
  explicit KinetoThreadLocalState(
      const ProfilerConfig& config,
      std::set<torch::profiler::impl::ActivityType> activities)
      : ProfilerThreadLocalStateBase(config),
        start_time_(getTimeUs()),
        activities_(std::move(activities)),
        record_queue_(config),
        cpu_trace_(start_time_, "PyTorch Profiler") {}
  ~KinetoThreadLocalState() override = default;

  static KinetoThreadLocalState* getTLS() {
    auto tls = ProfilerThreadLocalStateBase::getTLS();
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
        tls == nullptr || tls->profilerType() == ActiveProfilerType::KINETO);
    return static_cast<KinetoThreadLocalState*>(tls);
  }

  ActiveProfilerType profilerType() override {
    return ActiveProfilerType::KINETO;
  }

  bool tracePython() {
    return config().with_stack && activities_.count(ActivityType::CPU);
  }

  void reportMemoryUsage(
      void* ptr,
      int64_t alloc_size,
      int64_t total_allocated,
      int64_t total_reserved,
      c10::Device device) override {
    if (config_.profile_memory && config_.state != ProfilerState::Disabled) {
      std::lock_guard<std::mutex> guard(state_mutex_);
      memory_events_.emplace_back(
          torch::profiler::impl::getApproximateTime(),
          ptr,
          alloc_size,
          total_allocated,
          total_reserved,
          at::RecordFunction::currentThreadId(),
          torch::profiler::impl::kineto::kineto_ids(),
          device.type(),
          device.index());
    }
  }

  const post_process_t& getEventPostProcessingCallback() const {
    return event_post_process_cb_;
  }

  void setEventPostProcessingCallback(post_process_t&& cb) {
    event_post_process_cb_ = std::move(cb);
  }

  torch::profiler::impl::kineto::ActivityTraceWrapper finalizeTrace() {
    auto end_time = getTimeUs();
    materializeOpEvents();

    finalizeCPUTrace(cpu_trace_.get());
    {
      std::lock_guard<std::mutex> guard(state_mutex_);
      cpu_trace_.transferCpuTrace(end_time);
    }

    if (config().state != ProfilerState::KINETO_ONDEMAND) {
      auto trace = torch::profiler::impl::kineto::stopTrace();
      TORCH_CHECK(trace || !torch::profiler::kKinetoAvailable);
      addTraceEvents(trace);
      return trace;
    } else {
      return torch::profiler::impl::kineto::ActivityTraceWrapper();
    }
  }

  void materializeOpEvents() {
    std::lock_guard<std::mutex> guard(state_mutex_);
    auto converter = clock_converter_.makeConverter();

    for (const auto& e : memory_events_) {
      auto start_time_us = converter(e.start_time) / 1000;
      cpu_trace_.addCPUActivity(
          kMemoryEventName,
          torch::profiler::impl::kineto::KinetoActivityType::CPU_INSTANT_EVENT,
          e.kineto_info,
          /*correlation_id=*/0,
          start_time_us,
          start_time_us,
          getAnnotations(e));

      kineto_events_.emplace_back();
      auto& evt = kineto_events_.back();
      evt.name(kMemoryEventName)
          .startUs(start_time_us)
          .deviceIndex(e.device_index)
          .deviceType(e.device_type)
          .nBytes(e.alloc_size)
          .startThreadId(e.threadID);
    }
    memory_events_.clear();

    for (auto& e : record_queue_.getRecords(converter)) {
      // `take_data` handles time conversion.
      int64_t start_us = e.start_time_us_;
      int64_t end_us = e.end_time_us_;

      if (end_us < start_us) {
        // We initialize end_us_ to the smallest int64_t, so this means that
        // the op did not finish before we stopped profiling.
        continue;
      }

      // Call events post processing callback before finalizing trace, if there
      // is one.
      if (getEventPostProcessingCallback()) {
        getEventPostProcessingCallback()(
            c10::visit([](const auto& i) { return i.debug_handle_; }, e.event_),
            e.jit_stack_,
            e.jit_modules_);
      }

      kineto_events_.emplace_back();
      kineto_events_.back()
          .name(e.name())
          .startUs(start_us)
          .durationUs(end_us - start_us)
          .correlationId(e.correlation_id())
          .deviceType(c10::DeviceType::CPU)
          .startThreadId(e.start_tid_);

      // NB: also sets fields on `kineto_events_.back()`.
      auto annotations =
          EventFieldsVisitor(e, kineto_events_.back()).annotations_;

      cpu_trace_.addCPUActivity(
          e.name(),
          e.kinetoType(),
          e.kineto_info_,
          e.correlation_id(),
          start_us,
          end_us,
          annotations);
    }
  }

  void finalizeCPUTrace(std::unique_ptr<torch::profiler::impl::kineto::trace_t>& cpu_trace) {
#ifndef USE_KINETO
  }
#else // USE_KINETO
    TORCH_INTERNAL_ASSERT(
        cpu_trace->activities.size() == kineto_events_.size());
    // startThreadId_seqNum to pointer of activity.
    // Low-16bits of startThreadId and low-48bits seqNum are concatenated into
    // one uint64_t variable as key.

    // From the time being, we need disable the forward/backward correlation feature to
    // workaround the crash bug.
    // TODO: by Mike Guo
    // reenable the forward/backward correlation when kineto fix the following raw pointer
    //    GenericTraceActivity.flow.linkedActivity

    /*
    std::unordered_map<uint64_t, libkineto::GenericTraceActivity*>
        tidSeq2activity;

    for (const auto idx : c10::irange(cpu_trace->activities.size())) {
      auto& kineto_event = kineto_events_[idx];
      auto& activity = cpu_trace->activities[idx];

      // add information about an associated forward op, if a sequence number
      // is available (e.g. during training)
      if (kineto_event.sequenceNr() >= 0) {
        generateForwardBackwardLink(
            kineto_event, fwd_bwd_link_id, activity, tidSeq2activity);
      }
    }
    */

    addPythonEvents(cpu_trace);
  }

  void addPythonEvents(std::unique_ptr<torch::profiler::impl::kineto::trace_t>& cpu_trace) {
    if (!tracePython()) {
      return;
    }

    auto py_events = python_tracer::get_events();
    for (const auto& e : py_events) {
      TORCH_INTERNAL_ASSERT(
          !e->thread_id_,
          "Profiler expects only single threaded Python tracing.");
    }

    // The remainder of this function merges the Python and Kineto event
    // streams into a single stream. If Python tracing is not enabled, we want
    // to avoid this process altogether to cut down on processing time.
    if (!py_events.size()) {
      return;
    }

    // Kineto event times
    std::vector<int64_t> op_start_times;
    for (const auto& a : cpu_trace->activities) {
      op_start_times.push_back(a.startTime);
    }
    std::sort(op_start_times.begin(), op_start_times.end());

    // Map PyTraceEvent* to sequential integers for JSON export.
    ska::flat_hash_map<python_tracer::PyTraceEvent*, std::string>
        py_event_indices_{
            { nullptr,
              std::string("null") }};
    for (const auto i : c10::irange(py_events.size())) {
      py_event_indices_.insert({py_events[i].get(), std::to_string(i)});
    }

    ska::flat_hash_map<std::string, size_t> module_counter_;
    ska::flat_hash_map<size_t, std::string> module_id_map_;
    auto record_module_id = [&](python_tracer::PyTraceEvent* e) {
      if (e->call_type_ == python_tracer::CallType::kPyModuleCall &&
          module_id_map_.find(e->module_id_) == module_id_map_.end()) {
        // We use the fact that operator[] will default initialize new keys.
        module_id_map_[e->module_id_] =
            std::to_string(module_counter_[e->name_]++);
      }
    };

    // Python events
    std::vector<python_tracer::Replay> py_replay;
    for (const auto& e : py_events) {
      py_replay.push_back({e.get(), true});
      py_replay.push_back({e.get(), false});
    }
    std::sort(py_replay.begin(), py_replay.end());

    // In order to determine the state of the python interpreter when a
    // particular op is called, we have to replay the python events and note
    // timestamps which are associated with op start times.
    std::vector<python_tracer::PyTraceEvent*> py_stack;
    ska::flat_hash_map<int64_t, python_tracer::PyTraceEvent*> op_py_map;
    auto replay_it = py_replay.begin();
    for (auto t : op_start_times) {
      while (replay_it != py_replay.end() && replay_it->t() <= t) {
        if (replay_it->enter_) {
          py_stack.push_back(replay_it->frame_);
          record_module_id(replay_it->frame_);
        } else {
          TORCH_INTERNAL_ASSERT(py_stack.size());
          TORCH_INTERNAL_ASSERT(py_stack.back() == replay_it->frame_);
          py_stack.pop_back();
        }
        replay_it++;
      }
      op_py_map.insert({t, py_stack.size() ? py_stack.back() : nullptr});
    }

    std::vector<libkineto::GenericTraceActivity> py_activities;
    auto py_events_it = py_events.begin();
    auto py_device = libkineto::processId();
    auto main_thread = libkineto::systemThreadId();
    auto push_py_event = [&]() {
      auto e = (*py_events_it).get();
      libkineto::GenericTraceActivity op(
          cpu_trace->span, libkineto::ActivityType::PYTHON_FUNCTION, e->name_);

      op.device = py_device;
      op.resource = main_thread;
      op.startTime = e->startTime_;
      op.endTime = e->endTime_;

      op.addMetadata("Python id", py_event_indices_.at(e));
      op.addMetadata("Python parent id", py_event_indices_.at(e->parent_));
      op.addMetadata("Python thread", std::to_string(e->thread_id_));
      if (e->call_type_ == python_tracer::CallType::kPyModuleCall) {
        op.addMetadata("Python module id", module_id_map_.at(e->module_id_));
      }

      py_activities.push_back(op);
      py_events_it++;
    };

    TORCH_INTERNAL_ASSERT(cpu_trace->activities.size() == kineto_events_.size());
    for (const auto idx : c10::irange(cpu_trace->activities.size())) {
      auto& activity = cpu_trace->activities[idx];

      // Add any python events that occurred between this Kineto event and the
      // previous Kineto event.
      while (py_events_it != py_events.end() &&
             (*py_events_it)->endTime_ <= activity.endTime) {
        push_py_event();
      }

      auto python_caller = op_py_map.at(activity.startTime);
      activity.addMetadata(
          "python_caller_id", py_event_indices_.at(python_caller));

      // If the kineto event has a stack that means the JIT model has a stack
      // associated with it that we need to respect.
      if (!kineto_events_[idx].hasStack()) {
        std::vector<std::string> py_names;
        _push_reverse_order(python_caller, py_names);
        kineto_events_[idx].stack(py_names);
        activity.addMetadata("Call stack", torch::profiler::impl::stacksToStr(py_names, ";"));
      }
    }

    // Add any Python events which finish after the last Kineto event.
    while (py_events_it != py_events.end()) {
      push_py_event();
    }

    cpu_trace->activities.insert(cpu_trace->activities.end(), py_activities.begin(), py_activities.end());
  }

  void generateForwardBackwardLink(
      const KinetoEvent& kineto_event,
      uint64_t& fwd_bwd_link_id,
      libkineto::GenericTraceActivity& activity,
      std::unordered_map<uint64_t, libkineto::GenericTraceActivity*>&
          tidSeq2activity) {
    if (kineto_event.fwdThreadId() > 0) {
      // act is backward op.
      uint64_t key = getForwardThreadKey(
          kineto_event.fwdThreadId(), kineto_event.sequenceNr());
      auto iter = tidSeq2activity.find(key);
      if (iter != tidSeq2activity.end()) {
        libkineto::GenericTraceActivity* fwd = iter->second;
        fwd->flow.start = true;
        activity.flow.id = fwd->flow.id = fwd_bwd_link_id;
        activity.flow.type = fwd->flow.type = libkineto::kLinkFwdBwd;
        ++fwd_bwd_link_id;
      }
    } else if (kineto_event.startThreadId() != 0) {
      // act is forward op.
      uint64_t key = getForwardThreadKey(
          kineto_event.startThreadId(), kineto_event.sequenceNr());
      // Assumption: Among all ops with same sequence number,
      // the one with biggest start time is most likely launching backward op.
      auto iter = tidSeq2activity.find(key);
      if (iter == tidSeq2activity.end()) {
        tidSeq2activity[key] = &activity;
      } else {
        // Now the sequence number is only incremented on creating a "Node"
        // object for backward pass, by calling
        // "at::sequence_number::get_and_increment()". Among all ops with same
        // sequence number, the one with biggest startTime is the one launching
        // backward op.
        if (activity.startTime >= iter->second->startTime) {
          tidSeq2activity[key] = &activity;
        }
      }
    }
  }
#endif // USE_KINETO

  void addTraceEvents(torch::profiler::impl::kineto::ActivityTraceWrapper& trace) {
#ifdef USE_KINETO
    const auto& events = *(trace.get()->activities());
    for (const auto& ev_ptr : events) {
      if (ev_ptr == nullptr) {
        continue;
      }
      const auto& activity = *ev_ptr;
      // These events are already processed
      if (activity.type() != libkineto::ActivityType::CPU_OP &&
          activity.type() != libkineto::ActivityType::CPU_INSTANT_EVENT &&
          activity.type() != libkineto::ActivityType::USER_ANNOTATION &&
          activity.type() != libkineto::ActivityType::PYTHON_FUNCTION) {
        kineto_events_.emplace_back();
        auto& kineto_event = kineto_events_.back();
        kineto_event.name(activity.name())
            .deviceIndex(activity.deviceId())
            .deviceResourceId(activity.resourceId())
            .startUs(activity.timestamp())
            .durationUs(activity.duration())
            .activityType((uint8_t)activity.type());
        if (activity.linkedActivity()) {
          kineto_event.linkedCorrelationId(
              activity.linkedActivity()->correlationId());
        }
        kineto_event.deviceType(deviceTypeFromActivity(activity.type()));
      }
    }
#endif // USE_KINETO
  }

  uint64_t start_time_;
  torch::profiler::impl::ApproximateClockToUnixTimeConverter clock_converter_;
  std::set<torch::profiler::impl::ActivityType> activities_;
  torch::profiler::impl::RecordQueue record_queue_;
  torch::profiler::impl::AppendOnlyList<MemoryEventData, 1024> memory_events_;
  torch::profiler::impl::kineto::TraceWrapper cpu_trace_;
  std::vector<KinetoEvent> kineto_events_;
  // Optional, if event post-processing is enabled.
  post_process_t event_post_process_cb_;
};

static std::unique_ptr<KinetoThreadLocalState> globalStatePtr;

template<typename... Args>
static void initGlobalState(Args... args) {
  if (globalStatePtr) {
    LOG(WARNING) << "GlobalStatePtr already exists!";
  } else {
    globalStatePtr = std::make_unique<KinetoThreadLocalState>(std::forward<Args>(args)...);
  }
}

static void resetGlobalState() {
  TORCH_INTERNAL_ASSERT(globalStatePtr != nullptr, "Global state ptr cannot be null before resetting");
  globalStatePtr.reset();
}

template<bool use_global>
static KinetoThreadLocalState* getStatePtr() {
  return c10::guts::if_constexpr<use_global>(
      [] { return globalStatePtr.get(); },
      [] { return KinetoThreadLocalState::getTLS(); });
}

template<bool use_global_state_ptr = false>
std::unique_ptr<at::ObserverContext> onFunctionEnter(const at::RecordFunction& fn) {
  auto state_ptr = getStatePtr<use_global_state_ptr>();
  if (!state_ptr) {
    return nullptr;
  }
  auto corr_id = next_correlation_id();
  if (fn.scope() == at::RecordScope::USER_SCOPE) {
    torch::profiler::impl::kineto::pushUserCorrelationId(corr_id);
  } else {
    torch::profiler::impl::kineto::pushCorrelationId(corr_id);
  }
  return state_ptr->record_queue_.getSubqueue()->begin_op(fn, corr_id);
}

// @lint-ignore CLANGTIDY clang-diagnostic-unused-parameter
template<bool use_global_state_ptr = false>
void onFunctionExit(const at::RecordFunction& fn, at::ObserverContext* ctx_ptr) {
  auto state_ptr = getStatePtr<use_global_state_ptr>();
  if (!state_ptr) {
    return;
  }
  const auto& config = state_ptr->config();
  auto* kineto_ctx_ptr =
    static_cast<torch::profiler::impl::KinetoObserverContext*>(ctx_ptr);
  TORCH_INTERNAL_ASSERT(kineto_ctx_ptr != nullptr);
  kineto_ctx_ptr->event_->end_time_ = torch::profiler::impl::getApproximateTime();
  kineto_ctx_ptr->event_->end_thread_id_ = at::RecordFunction::currentThreadId();
  if (config.state == ProfilerState::KINETO_GPU_FALLBACK) {
    try {
      auto fallback = kineto_ctx_ptr->fallback_;
      TORCH_INTERNAL_ASSERT(fallback != nullptr);
      torch::profiler::impl::cudaStubs()->record(
          nullptr, &fallback->cuda_event_end_, nullptr);
    } catch (const std::exception& e) {
      LOG(WARNING) << "Failed to record CUDA event. " << e.what();
    }
  }

  if (fn.scope() == at::RecordScope::USER_SCOPE) {
    torch::profiler::impl::kineto::popUserCorrelationId();
  } else {
    torch::profiler::impl::kineto::popCorrelationId();
  }
}

template <bool use_global_callback = false>
void pushProfilingCallbacks(const std::unordered_set<at::RecordScope>& scopes) {
  auto registration_state_ptr = getStatePtr<use_global_callback>();
  TORCH_INTERNAL_ASSERT(registration_state_ptr, "Expected profiler state set");
  auto recordFunctionCallback =
      at::RecordFunctionCallback(
          onFunctionEnter<use_global_callback>,
          onFunctionExit<use_global_callback>)
          .needsInputs(registration_state_ptr->config().report_input_shapes)
          .scopes(scopes);

  auto handle = c10::guts::if_constexpr<use_global_callback>(
      [&] { return at::addGlobalCallback(recordFunctionCallback); },
      [&] { return at::addThreadLocalCallback(recordFunctionCallback);
      });
  registration_state_ptr->setCallbackHandle(handle);
}

} // namespace

void reportBackendEventToActiveKinetoProfiler(
    const int64_t start_time_us,
    const int64_t end_time_us,
    const int64_t debug_handle,
    const at::RecordScope scope,
    const std::string& event_name,
    const std::string& backend_name) {
  TORCH_INTERNAL_ASSERT(globalStatePtr == nullptr, "On-demand profiling does not support post processing callback");

  auto state_ptr = KinetoThreadLocalState::getTLS();
  if (!state_ptr) {
    return;
  }

  state_ptr->record_queue_.getSubqueue()->emplace_backend_event(
    torch::profiler::impl::BackendEvent {
      start_time_us,
      end_time_us,
      (uint8_t)scope,
      debug_handle,
      event_name,
      backend_name});

  /* no support for input shapes now?
  if (config.report_input_shapes) {
    ctx_ptr->shapes = inputSizes(fn);
    ctx_ptr->dtypes = inputTypes(fn);
  }
  */
}

void prepareProfiler(
    const torch::profiler::impl::ProfilerConfig& config,
    const std::set<torch::profiler::impl::ActivityType>& activities) {
  if (config.state == ProfilerState::NVTX) {
    return;
  }
  TORCH_CHECK(
      config.state == ProfilerState::KINETO ||
          config.state == ProfilerState::KINETO_GPU_FALLBACK,
      "Supported only in Kineto profiler");
  torch::profiler::impl::kineto::prepareTrace(
      /*cpuOnly=*/!at::hasCUDA(), activities, config.experimental_config);
}

void enableProfilerWithEventPostProcess(
    const torch::profiler::impl::ProfilerConfig& config,
    const std::set<torch::profiler::impl::ActivityType>& activities,
    post_process_t&& cb,
    const std::unordered_set<at::RecordScope>& scopes) {
  TORCH_CHECK(
      config.state != ProfilerState::NVTX,
      "NVTX does not support post processing callback.");
  TORCH_INTERNAL_ASSERT(globalStatePtr == nullptr, "On-demand profiling does not support post processing callback");

  enableProfiler(config, activities, scopes);
  auto state_ptr = KinetoThreadLocalState::getTLS();
  state_ptr->setEventPostProcessingCallback(std::move(cb));
}

void enableProfiler(
    const torch::profiler::impl::ProfilerConfig& config,
    const std::set<torch::profiler::impl::ActivityType>& activities,
    const std::unordered_set<at::RecordScope>& scopes) {
  TORCH_CHECK(!profilerEnabled(), "Profiler is already enabled on this thread");
  if (config.state == ProfilerState::NVTX) {
    torch::profiler::impl::pushNVTXCallbacks(config, scopes);
    return;
  }

  TORCH_CHECK(
      config.state == ProfilerState::KINETO ||
      config.state == ProfilerState::KINETO_GPU_FALLBACK ||
      config.state == ProfilerState::KINETO_ONDEMAND);
  TORCH_CHECK(
      !activities.empty(), "No activities specified for Kineto profiler");

  if (config.state == ProfilerState::KINETO ||
      config.state == ProfilerState::KINETO_GPU_FALLBACK) {
    auto state = std::make_shared<KinetoThreadLocalState>(config, activities);
    c10::ThreadLocalDebugInfo::_push(c10::DebugInfoKind::PROFILER_STATE, state);

    if (state->tracePython()) {
      python_tracer::call(python_tracer::Command::kStartOne);
    }

    if (activities.count(ActivityType::CPU)) {
      pushProfilingCallbacks<false>(scopes);
    }
    torch::profiler::impl::kineto::startTrace();
  }

  if (config.state == ProfilerState::KINETO_ONDEMAND) {
    initGlobalState(config, activities);

    TORCH_INTERNAL_ASSERT(activities.count(ActivityType::CPU), "Ondemand profiling must enable CPU tracing");
    pushProfilingCallbacks<true>(scopes);
  }
}

std::unique_ptr<ProfilerResult> disableProfiler() {
  auto state_ptr = static_cast<ProfilerThreadLocalStateBase*>(
      (globalStatePtr == nullptr) ? getStatePtr<false>() : getStatePtr<true>());

  const auto& config = state_ptr->config();
  TORCH_CHECK(
      state_ptr &&
          (config.state == ProfilerState::KINETO ||
           config.state == ProfilerState::KINETO_GPU_FALLBACK ||
           config.state == ProfilerState::KINETO_ONDEMAND ||
           config.state == ProfilerState::NVTX),
      "Can't disable Kineto profiler when it's not running");

  if (state_ptr->hasCallbackHandle()) {
    at::removeCallback(state_ptr->callbackHandle());
  }

  // Traces are converged via libkineto automatically for ondemand flow
  if (state_ptr->config().state == ProfilerState::KINETO_ONDEMAND) {
    auto kineto_state_ptr = static_cast<KinetoThreadLocalState*>(state_ptr);
    auto trace = kineto_state_ptr->finalizeTrace();
    resetGlobalState();
    return std::make_unique<ProfilerResult>();
  }

  // Shared among NVTX, KINETO, KINETO_GPU_FALLBACK
  std::unique_ptr<ProfilerResult> result;
  if (state_ptr->config().state == ProfilerState::NVTX) {
    result = std::make_unique<ProfilerResult>();
  }

  if (config.state == ProfilerState::KINETO ||
      config.state == ProfilerState::KINETO_GPU_FALLBACK) {
    auto kineto_state_ptr = static_cast<KinetoThreadLocalState*>(state_ptr);
    if (kineto_state_ptr->tracePython()) {
      python_tracer::call(python_tracer::Command::kStop);
    }

    auto trace = kineto_state_ptr->finalizeTrace();
    if (kineto_state_ptr->tracePython()) {
      python_tracer::call(python_tracer::Command::kClear);
    }

    result = std::make_unique<ProfilerResult>(
        kineto_state_ptr->start_time_,
        std::move(kineto_state_ptr->kineto_events_),
        std::move(trace));
  }

  // Disable thread-local profiler. We can't pop until the very end as it would invalidate
  // the `state_ptr` reference which we need to process the traces.
  (void)c10::ThreadLocalDebugInfo::_pop(c10::DebugInfoKind::PROFILER_STATE);
  return result;
}

int64_t KinetoEvent::cudaElapsedUs() const {
  if (!cuda_event_start_ || !cuda_event_end_) {
    return -1;
  }
  try {
    return (int64_t)torch::profiler::impl::cudaStubs()->elapsed(&cuda_event_start_, &cuda_event_end_);
  } catch (std::exception& e) {
    LOG(WARNING) << "Failed to measure time between two CUDA events. "
                 << e.what();
  }
  return -1;
}

ProfilerResult::ProfilerResult(
    uint64_t start_time,
    std::vector<KinetoEvent> events,
    torch::profiler::impl::kineto::ActivityTraceWrapper trace)
    : trace_start_us_(start_time),
      events_(std::move(events)),
      trace_(std::move(trace)) {}
ProfilerResult::ProfilerResult() = default;
ProfilerResult::~ProfilerResult() = default;

void ProfilerResult::save(const std::string& path) {
  trace_.save(path);
}

} // namespace profiler
} // namespace autograd
} // namespace torch
