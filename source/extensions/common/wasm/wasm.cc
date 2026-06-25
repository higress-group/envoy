#include "source/extensions/common/wasm/wasm.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "envoy/event/deferred_deletable.h"

#include "source/common/common/logger.h"
#include "source/common/network/dns_resolver/dns_factory_util.h"
#include "source/extensions/common/wasm/plugin.h"
#include "source/extensions/common/wasm/stats_handler.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"

using proxy_wasm::FailState;
using proxy_wasm::Word;

namespace Envoy {

using ScopeWeakPtr = std::weak_ptr<Stats::Scope>;

namespace Extensions {
namespace Common {
namespace Wasm {
namespace {

struct CodeCacheEntry {
  std::string code;
  bool in_progress;
  MonotonicTime use_time;
  MonotonicTime fetch_time;
};

class RemoteDataFetcherAdapter : public Config::DataFetcher::RemoteDataFetcherCallback,
                                 public Event::DeferredDeletable {
public:
  RemoteDataFetcherAdapter(std::function<void(std::string cb)> cb) : cb_(cb) {}
  ~RemoteDataFetcherAdapter() override = default;
  void onSuccess(const std::string& data) override { cb_(data); }
  void onFailure(Config::DataFetcher::FailureReason) override { cb_(""); }
  void setFetcher(std::unique_ptr<Config::DataFetcher::RemoteDataFetcher>&& fetcher) {
    fetcher_ = std::move(fetcher);
  }

private:
  std::function<void(std::string)> cb_;
  std::unique_ptr<Config::DataFetcher::RemoteDataFetcher> fetcher_;
};

const std::string INLINE_STRING = "<inline>";
const int CODE_CACHE_SECONDS_NEGATIVE_CACHING = 10;
const int CODE_CACHE_SECONDS_CACHING_TTL = 24 * 3600; // 24 hours.
MonotonicTime::duration cache_time_offset_for_testing{};

std::mutex code_cache_mutex;
absl::flat_hash_map<std::string, CodeCacheEntry>* code_cache = nullptr;

// Downcast WasmBase to the actual Wasm.
inline Wasm* getWasm(WasmHandleSharedPtr& base_wasm_handle) {
  return static_cast<Wasm*>(base_wasm_handle->wasm().get());
}

#ifdef HIGRESS
WasmEvent failStateToWasmEvent(FailState state) {
  switch (state) {
  case FailState::Ok:
    return WasmEvent::Ok;
  case FailState::UnableToCreateVm:
    return WasmEvent::UnableToCreateVm;
  case FailState::UnableToCloneVm:
    return WasmEvent::UnableToCloneVm;
  case FailState::MissingFunction:
    return WasmEvent::MissingFunction;
  case FailState::UnableToInitializeCode:
    return WasmEvent::UnableToInitializeCode;
  case FailState::StartFailed:
    return WasmEvent::StartFailed;
  case FailState::ConfigureFailed:
    return WasmEvent::ConfigureFailed;
  case FailState::RuntimeError:
    return WasmEvent::RuntimeError;
  case FailState::RecoverError:
    return WasmEvent::RecoverError;
  }
  PANIC("corrupt enum");
}

const int MIN_RECOVER_INTERVAL_SECONDS = 1;
constexpr uint64_t DefaultReclaimMemoryThresholdBytes = 800ULL * 1024 * 1024;
constexpr absl::string_view ReclaimMemoryThresholdRuntimeKey =
    "envoy.wasm.reclaim.memory_threshold_bytes";

MonotonicTime effectiveMonotonicTime(Event::Dispatcher& dispatcher) {
  return dispatcher.timeSource().monotonicTime() + cache_time_offset_for_testing;
}

struct RebuildGuardEntry {
  MonotonicTime last_recover_time;
  std::weak_ptr<WasmHandle> current_wasm_handle;
  std::vector<std::weak_ptr<WasmHandle>> old_wasm_handles;

  void pruneExpired(const WasmHandleSharedPtr& current_wasm_handle = nullptr) {
    old_wasm_handles.erase(
        std::remove_if(old_wasm_handles.begin(), old_wasm_handles.end(),
                       [&current_wasm_handle](const std::weak_ptr<WasmHandle>& old_wasm_handle) {
                         auto locked = old_wasm_handle.lock();
                         return locked == nullptr || (current_wasm_handle != nullptr &&
                                                      locked.get() == current_wasm_handle.get());
                       }),
        old_wasm_handles.end());
  }

  bool hasLiveOldGeneration(const WasmHandleSharedPtr& current_wasm_handle) {
    pruneExpired(current_wasm_handle);
    return !old_wasm_handles.empty();
  }

  void trackCurrentGeneration(const WasmHandleSharedPtr& wasm_handle) {
    current_wasm_handle = wasm_handle;
  }

  void trackOldGeneration(const WasmHandleSharedPtr& old_wasm_handle) {
    if (old_wasm_handle == nullptr) {
      return;
    }
    pruneExpired();
    old_wasm_handles.push_back(old_wasm_handle);
  }

  bool canRetire() {
    pruneExpired();
    return current_wasm_handle.expired() && old_wasm_handles.empty();
  }
};

thread_local absl::flat_hash_map<std::string, RebuildGuardEntry> rebuild_guard_registry;

void sweepRebuildGuardRegistry(const std::string& active_vm_key) {
  for (auto it = rebuild_guard_registry.begin(); it != rebuild_guard_registry.end();) {
    if (it->first == active_vm_key) {
      ++it;
      continue;
    }
    if (it->second.canRetire()) {
      auto erase_it = it++;
      rebuild_guard_registry.erase(erase_it);
    } else {
      ++it;
    }
  }
}
#endif

} // namespace

void Wasm::initializeLifecycle(Server::ServerLifecycleNotifier& lifecycle_notifier) {
  auto weak = std::weak_ptr<Wasm>(std::static_pointer_cast<Wasm>(shared_from_this()));
  lifecycle_notifier.registerCallback(Server::ServerLifecycleNotifier::Stage::ShutdownExit,
                                      [this, weak](Event::PostCb post_cb) {
                                        auto lock = weak.lock();
                                        if (lock) { // See if we are still alive.
                                          server_shutdown_post_cb_ = std::move(post_cb);
                                        }
                                      });
}

#ifdef HIGRESS
void Wasm::initializeRuntimeStatsTimer() {
  runtime_stats_timer_ = dispatcher_.createTimer(
      [weak = std::weak_ptr<Wasm>(std::static_pointer_cast<Wasm>(shared_from_this()))]() {
        auto shared = weak.lock();
        if (shared) {
          shared->runtime_stats_handler_.updateMemorySize(shared->wasm_vm()->getMemorySize());
          shared->runtime_stats_timer_->enableTimer(
              std::chrono::milliseconds(Wasm::kRuntimeStatsInterval));
        }
      });
  runtime_stats_timer_->enableTimer(std::chrono::milliseconds(Wasm::kRuntimeStatsInterval));
}

uint64_t Wasm::reclaimMemoryThreshold() const {
  if (runtime_loader_ == nullptr) {
    return DefaultReclaimMemoryThresholdBytes;
  }
  const uint64_t threshold = runtime_loader_->snapshot().getInteger(
      ReclaimMemoryThresholdRuntimeKey, DefaultReclaimMemoryThresholdBytes);
  return threshold == 0 ? DefaultReclaimMemoryThresholdBytes : threshold;
}

void Wasm::markReclaimEligible() {
  if (!reclaim_eligible_since_.has_value()) {
    reclaim_eligible_since_ = effectiveMonotonicTime(dispatcher_);
  }
}

void Wasm::recordReclaimLatency(MonotonicTime now) {
  if (!reclaim_eligible_since_.has_value()) {
    return;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - reclaim_eligible_since_.value());
  lifecycleStats().reclaim_latency_.recordValue(elapsed.count() < 0 ? 0 : elapsed.count());
}
#endif

Wasm::Wasm(WasmConfig& config, absl::string_view vm_key, const Stats::ScopeSharedPtr& scope,
           Api::Api& api, Upstream::ClusterManager& cluster_manager, Event::Dispatcher& dispatcher
#ifdef HIGRESS
           ,
           Runtime::Loader* runtime
#endif
           )
    : WasmBase(
          createWasmVm(config.config().vm_config().runtime()), config.config().vm_config().vm_id(),
          MessageUtil::anyToBytes(config.config().vm_config().configuration()),
          toStdStringView(vm_key), config.environmentVariables(), config.allowedCapabilities()),
      scope_(scope), api_(api), stat_name_pool_(scope_->symbolTable()),
      custom_stat_namespace_(stat_name_pool_.add(CustomStatNamespace)),
      cluster_manager_(cluster_manager), dispatcher_(dispatcher),
      time_source_(dispatcher.timeSource()),
#ifdef HIGRESS
      runtime_stats_handler_(RuntimeStatsHandler(scope, config.config().vm_config().runtime(),
                                                 config.config().name(), dispatcher.name())),
      runtime_loader_(runtime),
      lifecycle_stats_handler_(LifecycleStatsHandler(scope, config.config().vm_config().runtime(),
                                                     config.config().name())) {
#else
      lifecycle_stats_handler_(
          LifecycleStatsHandler(scope, config.config().vm_config().runtime())) {
#endif
  lifecycle_stats_handler_.onEvent(WasmEvent::VmCreated);
  ENVOY_LOG(debug, "Base Wasm created {} now active", lifecycle_stats_handler_.getActiveVmCount());
}

Wasm::Wasm(WasmHandleSharedPtr base_wasm_handle, Event::Dispatcher& dispatcher
#ifdef HIGRESS
           ,
           Runtime::Loader* runtime
#endif
           )
    : WasmBase(base_wasm_handle,
               [&base_wasm_handle]() {
                 return createWasmVm(absl::StrCat(
                     "envoy.wasm.runtime.",
                     toAbslStringView(base_wasm_handle->wasm()->wasm_vm()->getEngineName())));
               }),
      scope_(getWasm(base_wasm_handle)->scope_), api_(getWasm(base_wasm_handle)->api_),
      stat_name_pool_(scope_->symbolTable()),
      custom_stat_namespace_(stat_name_pool_.add(CustomStatNamespace)),
      cluster_manager_(getWasm(base_wasm_handle)->clusterManager()), dispatcher_(dispatcher),
      time_source_(dispatcher.timeSource()),
#ifdef HIGRESS
      runtime_stats_handler_(RuntimeStatsHandler(
          getWasm(base_wasm_handle)->scope_,
          getWasm(base_wasm_handle)->runtime_stats_handler_.runtime,
          getWasm(base_wasm_handle)->runtime_stats_handler_.plugin_name, dispatcher.name())),
      runtime_loader_(runtime != nullptr ? runtime : getWasm(base_wasm_handle)->runtime_loader_),
#endif
      lifecycle_stats_handler_(getWasm(base_wasm_handle)->lifecycle_stats_handler_) {
  lifecycle_stats_handler_.onEvent(WasmEvent::VmCreated);
#ifdef HIGRESS
  auto* vm = wasm_vm();
  if (vm) {
    vm->addFailCallback([this](FailState fail_state) {
      lifecycle_stats_handler_.onEvent(failStateToWasmEvent(fail_state));
    });
  }
#endif
  ENVOY_LOG(debug, "Thread-Local Wasm created {} now active",
            lifecycle_stats_handler_.getActiveVmCount());
}

void Wasm::error(std::string_view message) { ENVOY_LOG(error, "Wasm VM failed {}", message); }

void Wasm::setTimerPeriod(uint32_t context_id, std::chrono::milliseconds new_period) {
  auto& period = timer_period_[context_id];
  auto& timer = timer_[context_id];
  bool was_running = timer && period.count() > 0;
  period = new_period;
  if (was_running) {
    timer->disableTimer();
  }
  if (period.count() > 0) {
    timer = dispatcher_.createTimer(
        [weak = std::weak_ptr<Wasm>(std::static_pointer_cast<Wasm>(shared_from_this())),
         context_id]() {
          auto shared = weak.lock();
          if (shared) {
            shared->tickHandler(context_id);
          }
        });
    timer->enableTimer(period);
  }
}

void Wasm::tickHandler(uint32_t root_context_id) {
  auto period = timer_period_.find(root_context_id);
  auto timer = timer_.find(root_context_id);
  if (period == timer_period_.end() || timer == timer_.end() || !on_tick_) {
    return;
  }
  auto context = getContext(root_context_id);
  if (context) {
    context->onTick(0);
  }
  if (timer->second && period->second.count() > 0) {
    timer->second->enableTimer(period->second);
  }
}

Wasm::~Wasm() {
  lifecycle_stats_handler_.onEvent(WasmEvent::VmShutDown);
#ifdef HIGRESS
  if (runtime_stats_timer_) {
    runtime_stats_timer_->disableTimer();
  }
#endif
  ENVOY_LOG(debug, "~Wasm {} remaining active", lifecycle_stats_handler_.getActiveVmCount());
  if (server_shutdown_post_cb_) {
    dispatcher_.post(std::move(server_shutdown_post_cb_));
  }
}

#if defined(HIGRESS)
PluginHandleSharedPtrThreadLocal::PluginHandleSharedPtrThreadLocal(PluginHandleSharedPtr handle,
                                                                   WasmHandleSharedPtr base_wasm,
                                                                   bool enable_reclaim_timer)
    : plugin_(handle != nullptr ? std::static_pointer_cast<Plugin>(handle->plugin()) : nullptr),
      base_wasm_(base_wasm), reclaim_timer_enabled_(enable_reclaim_timer), handle_(handle) {
  initializeReclaimTimer();
}

PluginHandleSharedPtrThreadLocal::~PluginHandleSharedPtrThreadLocal() {
  if (reclaim_timer_) {
    reclaim_timer_->disableTimer();
  }
}

void PluginHandleSharedPtrThreadLocal::initializeReclaimTimer() {
  if (!reclaim_timer_enabled_) {
    return;
  }
  if (handle_ == nullptr || handle_->wasmHandle() == nullptr ||
      handle_->wasmHandle()->wasm() == nullptr) {
    return;
  }
  reclaim_timer_ =
      handle_->wasmHandle()->wasm()->dispatcher().createTimer([this]() { onReclaimTimer(); });
  reclaim_timer_->enableTimer(kReclaimTimerInterval);
}

void PluginHandleSharedPtrThreadLocal::runReclaimTimerForTesting() { onReclaimTimer(); }

void PluginHandleSharedPtrThreadLocal::onReclaimTimer() {
  auto rearm = [this]() {
    if (reclaim_timer_) {
      reclaim_timer_->enableTimer(kReclaimTimerInterval);
    }
  };

  if (handle_ == nullptr || handle_->wasmHandle() == nullptr ||
      handle_->wasmHandle()->wasm() == nullptr) {
    rearm();
    return;
  }

  auto wasm = handle_->wasmHandle()->wasm();
  const uint64_t memory_size = wasm->wasm_vm() != nullptr ? wasm->wasm_vm()->getMemorySize() : 0;
  const bool memory_triggered = memory_size > wasm->reclaimMemoryThreshold();
  const bool explicit_triggered = wasm->shouldRebuild();
  if (!memory_triggered && !explicit_triggered) {
    rearm();
    return;
  }

  RebuildSource source = RebuildSource::Explicit;
  if (memory_triggered) {
    source = RebuildSource::Memory;
    wasm->markReclaimEligible();
    wasm->setShouldRebuild(true);
  } else {
    wasm->markReclaimEligible();
  }

  rebuild(false, source);
  if (memory_triggered) {
    wasm->setShouldRebuild(false);
  }
  rearm();
}

bool PluginHandleSharedPtrThreadLocal::syncHandleToCurrentGeneration(const std::string& vm_key) {
  auto current_base = proxy_wasm::getThreadLocalWasm(vm_key);
  if (current_base == nullptr) {
    ENVOY_LOG(info, "wasm vm proactive rebuild skipped: no current generation for vm_key");
    return false;
  }
  auto current_wasm_handle = std::static_pointer_cast<WasmHandle>(current_base);
  if (handle_ != nullptr && handle_->wasmHandle() != nullptr &&
      handle_->wasmHandle().get() == current_wasm_handle.get()) {
    return true;
  }
  if (base_wasm_ != nullptr && plugin_ != nullptr) {
    auto synced_handle = getOrCreateThreadLocalPlugin(base_wasm_, plugin_,
                                                      current_wasm_handle->wasm()->dispatcher());
    if (synced_handle != nullptr && synced_handle->wasmHandle() != nullptr &&
        synced_handle->wasmHandle().get() == current_wasm_handle.get()) {
      handle_ = synced_handle;
      ENVOY_LOG(info, "wasm vm proactive rebuild skipped: stale wrapper synchronized to current "
                      "generation");
      return false;
    }
  }
  ENVOY_LOG(info, "wasm vm proactive rebuild skipped: stale wrapper has no current plugin handle");
  return false;
}

bool PluginHandleSharedPtrThreadLocal::rebuild(bool is_fail_recovery, RebuildSource source) {
  if (handle_ == nullptr || handle_->wasmHandle() == nullptr ||
      handle_->wasmHandle()->wasm() == nullptr) {
    ENVOY_LOG(warn, "wasm has not been initialized");
    return false;
  }
  auto current_wasm_handle = handle_->wasmHandle();
  auto current_wasm = current_wasm_handle->wasm();
  const std::string vm_key(current_wasm->vm_key());
  if (!is_fail_recovery && !syncHandleToCurrentGeneration(vm_key)) {
    return false;
  }
  current_wasm_handle = handle_->wasmHandle();
  current_wasm = current_wasm_handle->wasm();
  sweepRebuildGuardRegistry(vm_key);
  auto& guard = rebuild_guard_registry[vm_key];
  guard.trackCurrentGeneration(current_wasm_handle);
  auto& dispatcher = current_wasm->dispatcher();
  auto now = effectiveMonotonicTime(dispatcher);
  if (now - guard.last_recover_time < std::chrono::seconds(MIN_RECOVER_INTERVAL_SECONDS)) {
    ENVOY_LOG(info, "wasm vm rebuild skipped: recover interval has not been reached");
    return false;
  }
  if (!is_fail_recovery && guard.hasLiveOldGeneration(current_wasm_handle)) {
    ENVOY_LOG(info, "wasm vm proactive rebuild skipped: live generation cap reached");
    return false;
  }
  const auto active_stream_count = current_wasm->activeStreamCount();
  if (!is_fail_recovery && active_stream_count > 0) {
    ENVOY_LOG(info, "wasm vm proactive rebuild skipped: current generation has {} active streams",
              active_stream_count);
    return false;
  }
  // Even if rebuild fails, it will be retried after the interval
  guard.last_recover_time = now;
  std::shared_ptr<PluginHandleBase> new_handle;
  if (handle_->rebuild(new_handle) && new_handle != nullptr) {
    auto new_plugin_handle = std::static_pointer_cast<PluginHandle>(new_handle);
    auto new_wasm_handle = new_plugin_handle->wasmHandle();
    if (new_wasm_handle != nullptr && new_wasm_handle.get() != current_wasm_handle.get()) {
      guard.trackOldGeneration(current_wasm_handle);
    }
    guard.trackCurrentGeneration(new_wasm_handle);
    handle_ = new_plugin_handle;
    // Increment appropriate metrics based on rebuild type
    if (is_fail_recovery) {
      handle_->wasmHandle()->wasm()->lifecycleStats().recover_total_.inc();
      ENVOY_LOG(info, "wasm vm recover from crash success");
    } else {
      auto& stats = handle_->wasmHandle()->wasm()->lifecycleStats();
      stats.rebuild_total_.inc();
      if (source == RebuildSource::Memory) {
        stats.rebuild_memory_total_.inc();
      } else {
        stats.rebuild_explicit_total_.inc();
      }
      current_wasm->recordReclaimLatency(now);
      current_wasm->clearReclaimEligible();
      ENVOY_LOG(info, "wasm vm rebuild success");
    }
    return true;
  }
  ENVOY_LOG(info, "wasm vm {} execution failed", is_fail_recovery ? "recover" : "rebuild");
  return false;
}
#endif

// NOLINTNEXTLINE(readability-identifier-naming)
Word resolve_dns(Word dns_address_ptr, Word dns_address_size, Word token_ptr) {
  auto context = static_cast<Context*>(proxy_wasm::contextOrEffectiveContext());
  auto root_context = context->isRootContext() ? context : context->rootContext();
  auto address = context->wasmVm()->getMemory(dns_address_ptr, dns_address_size);
  if (!address) {
    return WasmResult::InvalidMemoryAccess;
  }
  // Verify set and verify token_ptr before initiating the async resolve.
  uint32_t token = context->wasm()->nextDnsToken();
  if (!context->wasm()->setDatatype(token_ptr, token)) {
    return WasmResult::InvalidMemoryAccess;
  }
  auto callback = [weak_wasm = std::weak_ptr<Wasm>(context->wasm()->sharedThis()), root_context,
                   context_id = context->id(),
                   token](Envoy::Network::DnsResolver::ResolutionStatus status,
                          std::list<Envoy::Network::DnsResponse>&& response) {
    auto wasm = weak_wasm.lock();
    if (!wasm) {
      return;
    }
    root_context->onResolveDns(token, status, std::move(response));
  };
  if (!context->wasm()->dnsResolver()) {
    envoy::config::core::v3::TypedExtensionConfig typed_dns_resolver_config;
    Network::DnsResolverFactory& dns_resolver_factory =
        Network::createDefaultDnsResolverFactory(typed_dns_resolver_config);
    context->wasm()->dnsResolver() = dns_resolver_factory.createDnsResolver(
        context->wasm()->dispatcher(), context->wasm()->api(), typed_dns_resolver_config);
  }
  context->wasm()->dnsResolver()->resolve(std::string(address.value()),
                                          Network::DnsLookupFamily::Auto, callback);
  return WasmResult::Ok;
}

void Wasm::registerCallbacks() {
  WasmBase::registerCallbacks();
#define _REGISTER(_fn)                                                                             \
  wasm_vm_->registerCallback(                                                                      \
      "env", "envoy_" #_fn, &_fn,                                                                  \
      &proxy_wasm::ConvertFunctionWordToUint32<decltype(_fn), _fn>::convertFunctionWordToUint32)
  _REGISTER(resolve_dns);
#undef _REGISTER
}

void Wasm::getFunctions() {
  WasmBase::getFunctions();
#define _GET(_fn) wasm_vm_->getFunction("envoy_" #_fn, &_fn##_);
  _GET(on_resolve_dns)
  _GET(on_stats_update)
#undef _GET
}

proxy_wasm::CallOnThreadFunction Wasm::callOnThreadFunction() {
  auto& dispatcher = dispatcher_;
  return [&dispatcher](const std::function<void()>& f) { return dispatcher.post(f); };
}

ContextBase* Wasm::createContext(const std::shared_ptr<PluginBase>& plugin) {
  if (create_context_for_testing_) {
    return create_context_for_testing_(this, std::static_pointer_cast<Plugin>(plugin));
  }
  return new Context(this, std::static_pointer_cast<Plugin>(plugin));
}

ContextBase* Wasm::createRootContext(const std::shared_ptr<PluginBase>& plugin) {
  if (create_root_context_for_testing_) {
    return create_root_context_for_testing_(this, std::static_pointer_cast<Plugin>(plugin));
  }
  return new Context(this, std::static_pointer_cast<Plugin>(plugin));
}

ContextBase* Wasm::createVmContext() { return new Context(this); }

void Wasm::log(const PluginSharedPtr& plugin, const Http::RequestHeaderMap* request_headers,
               const Http::ResponseHeaderMap* response_headers,
               const Http::ResponseTrailerMap* response_trailers,
               const StreamInfo::StreamInfo& stream_info,
               AccessLog::AccessLogType access_log_type) {
  auto context = getRootContext(plugin, true);
  context->log(request_headers, response_headers, response_trailers, stream_info, access_log_type);
}

void Wasm::onStatsUpdate(const PluginSharedPtr& plugin, Envoy::Stats::MetricSnapshot& snapshot) {
  auto context = getRootContext(plugin, true);
  context->onStatsUpdate(snapshot);
}

void clearCodeCacheForTesting() {
  std::lock_guard<std::mutex> guard(code_cache_mutex);
  if (code_cache) {
    delete code_cache;
    code_cache = nullptr;
  }
  cache_time_offset_for_testing = {};
#if defined(HIGRESS)
  rebuild_guard_registry.clear();
#endif
  getCreateStatsHandler().resetStatsForTesting();
}

#if defined(HIGRESS)
size_t rebuildGuardRegistrySizeForTesting() { return rebuild_guard_registry.size(); }
#endif

// TODO: remove this post #4160: Switch default to SimulatedTimeSystem.
void setTimeOffsetForCodeCacheForTesting(MonotonicTime::duration d) {
  cache_time_offset_for_testing = d;
}

static proxy_wasm::WasmHandleFactory
getWasmHandleFactory(WasmConfig& wasm_config, const Stats::ScopeSharedPtr& scope, Api::Api& api,
                     Upstream::ClusterManager& cluster_manager, Event::Dispatcher& dispatcher,
                     Server::ServerLifecycleNotifier& lifecycle_notifier
#if defined(HIGRESS)
                     ,
                     Runtime::Loader* runtime
#endif
) {
  return [&wasm_config, &scope, &api, &cluster_manager, &dispatcher, &lifecycle_notifier
#if defined(HIGRESS)
          ,
          runtime
#endif
  ](std::string_view vm_key) -> WasmHandleBaseSharedPtr {
    auto wasm = std::make_shared<Wasm>(wasm_config, toAbslStringView(vm_key), scope, api,
                                       cluster_manager, dispatcher
#if defined(HIGRESS)
                                       ,
                                       runtime
#endif
    );
    wasm->initializeLifecycle(lifecycle_notifier);
    return std::static_pointer_cast<WasmHandleBase>(std::make_shared<WasmHandle>(std::move(wasm)));
  };
}

static proxy_wasm::WasmHandleCloneFactory
getWasmHandleCloneFactory(Event::Dispatcher& dispatcher,
                          CreateContextFn create_root_context_for_testing) {
  return [&dispatcher, create_root_context_for_testing](
             WasmHandleBaseSharedPtr base_wasm) -> std::shared_ptr<WasmHandleBase> {
    auto wasm = std::make_shared<Wasm>(std::static_pointer_cast<WasmHandle>(base_wasm), dispatcher);
    wasm->setCreateContextForTesting(nullptr, create_root_context_for_testing);
#ifdef HIGRESS
    wasm->initializeRuntimeStatsTimer();
#endif
    return std::static_pointer_cast<WasmHandleBase>(std::make_shared<WasmHandle>(std::move(wasm)));
  };
}

static proxy_wasm::PluginHandleFactory getPluginHandleFactory() {
  return [](WasmHandleBaseSharedPtr base_wasm,
            PluginBaseSharedPtr base_plugin) -> std::shared_ptr<PluginHandleBase> {
    return std::static_pointer_cast<PluginHandleBase>(
        std::make_shared<PluginHandle>(std::static_pointer_cast<WasmHandle>(base_wasm),
                                       std::static_pointer_cast<Plugin>(base_plugin)));
  };
}

WasmEvent toWasmEvent(const std::shared_ptr<WasmHandleBase>& wasm) {
  if (!wasm) {
    return WasmEvent::UnableToCreateVm;
  }
  switch (wasm->wasm()->fail_state()) {
  case FailState::Ok:
    return WasmEvent::Ok;
  case FailState::UnableToCreateVm:
    return WasmEvent::UnableToCreateVm;
  case FailState::UnableToCloneVm:
    return WasmEvent::UnableToCloneVm;
  case FailState::MissingFunction:
    return WasmEvent::MissingFunction;
  case FailState::UnableToInitializeCode:
    return WasmEvent::UnableToInitializeCode;
  case FailState::StartFailed:
    return WasmEvent::StartFailed;
  case FailState::ConfigureFailed:
    return WasmEvent::ConfigureFailed;
  case FailState::RuntimeError:
    return WasmEvent::RuntimeError;
#if defined(HIGRESS)
  case FailState::RecoverError:
    return WasmEvent::RecoverError;
#endif
  }
  PANIC("corrupt enum");
}

bool createWasm(const PluginSharedPtr& plugin, const Stats::ScopeSharedPtr& scope,
                Upstream::ClusterManager& cluster_manager, Init::Manager& init_manager,
                Event::Dispatcher& dispatcher, Api::Api& api,
                Server::ServerLifecycleNotifier& lifecycle_notifier,
                Config::DataSource::RemoteAsyncDataProviderPtr& remote_data_provider,
                CreateWasmCallback&& cb, CreateContextFn create_root_context_for_testing
#if defined(HIGRESS)
                ,
                Runtime::Loader* runtime
#endif
) {
  auto& stats_handler = getCreateStatsHandler();
  std::string source, code;
  auto config = plugin->wasmConfig();
  auto vm_config = config.config().vm_config();
  bool fetch = false;
  if (vm_config.code().has_remote()) {
    // TODO(https://github.com/envoyproxy/envoy/issues/25052) Stabilize this feature.
    ENVOY_LOG_TO_LOGGER(Envoy::Logger::Registry::getLog(Envoy::Logger::Id::wasm), warn,
                        "Wasm remote code fetch is unstable and may cause a crash");
    auto now = dispatcher.timeSource().monotonicTime() + cache_time_offset_for_testing;
    source = vm_config.code().remote().http_uri().uri();
    std::lock_guard<std::mutex> guard(code_cache_mutex);
    if (!code_cache) {
      code_cache = new std::remove_reference<decltype(*code_cache)>::type;
    }
    Stats::ScopeSharedPtr create_wasm_stats_scope = stats_handler.lockAndCreateStats(scope);
    // Remove entries older than CODE_CACHE_SECONDS_CACHING_TTL except for our target.
    for (auto it = code_cache->begin(); it != code_cache->end();) {
      if (now - it->second.use_time > std::chrono::seconds(CODE_CACHE_SECONDS_CACHING_TTL) &&
          it->first != vm_config.code().remote().sha256()) {
        code_cache->erase(it++);
      } else {
        ++it;
      }
    }
    stats_handler.onRemoteCacheEntriesChanged(code_cache->size());
    auto it = code_cache->find(vm_config.code().remote().sha256());
    if (it != code_cache->end()) {
      it->second.use_time = now;
      if (it->second.in_progress) {
        stats_handler.onEvent(WasmEvent::RemoteLoadCacheMiss);
        ENVOY_LOG_TO_LOGGER(Envoy::Logger::Registry::getLog(Envoy::Logger::Id::wasm), warn,
                            "createWasm: failed to load (in progress) from {}", source);
        cb(nullptr);
      }
      code = it->second.code;
      if (code.empty()) {
        if (now - it->second.fetch_time <
            std::chrono::seconds(CODE_CACHE_SECONDS_NEGATIVE_CACHING)) {
          stats_handler.onEvent(WasmEvent::RemoteLoadCacheNegativeHit);
          ENVOY_LOG_TO_LOGGER(Envoy::Logger::Registry::getLog(Envoy::Logger::Id::wasm), warn,
                              "createWasm: failed to load (cached) from {}", source);
          cb(nullptr);
        }
        fetch = true; // Fetch failed, retry.
        it->second.in_progress = true;
        it->second.fetch_time = now;
      } else {
        stats_handler.onEvent(WasmEvent::RemoteLoadCacheHit);
      }
    } else {
      fetch = true; // Not in cache, fetch.
      auto& e = (*code_cache)[vm_config.code().remote().sha256()];
      e.in_progress = true;
      e.use_time = e.fetch_time = now;
      stats_handler.onRemoteCacheEntriesChanged(code_cache->size());
      stats_handler.onEvent(WasmEvent::RemoteLoadCacheMiss);
    }
  } else if (vm_config.code().has_local()) {
    code = Config::DataSource::read(vm_config.code().local(), true, api);
    source = Config::DataSource::getPath(vm_config.code().local())
                 .value_or(code.empty() ? EMPTY_STRING : INLINE_STRING);
  }

  auto vm_key = proxy_wasm::makeVmKey(vm_config.vm_id(),
                                      MessageUtil::anyToBytes(vm_config.configuration()), code);
  auto complete_cb = [cb, vm_key, plugin, scope, &api, &cluster_manager, &dispatcher
#if defined(HIGRESS)
                      ,
                      runtime
#endif
                      ,
                      &lifecycle_notifier, create_root_context_for_testing,
                      &stats_handler](std::string code) -> bool {
    if (code.empty()) {
      cb(nullptr);
      return false;
    }

    auto config = plugin->wasmConfig();
    auto wasm = proxy_wasm::createWasm(
        vm_key, code, plugin,
        getWasmHandleFactory(config, scope, api, cluster_manager, dispatcher, lifecycle_notifier
#if defined(HIGRESS)
                             ,
                             runtime
#endif
                             ),
        getWasmHandleCloneFactory(dispatcher, create_root_context_for_testing),
        config.config().vm_config().allow_precompiled());
    Stats::ScopeSharedPtr create_wasm_stats_scope = stats_handler.lockAndCreateStats(scope);
    stats_handler.onEvent(toWasmEvent(wasm));
    if (!wasm || wasm->wasm()->isFailed()) {
      ENVOY_LOG_TO_LOGGER(Envoy::Logger::Registry::getLog(Envoy::Logger::Id::wasm), trace,
                          "Unable to create Wasm");
      cb(nullptr);
      return false;
    }
    cb(std::static_pointer_cast<WasmHandle>(wasm));
    return true;
  };

  if (fetch) {
    auto holder = std::make_shared<std::unique_ptr<Event::DeferredDeletable>>();
    auto fetch_callback = [vm_config, complete_cb, source, &dispatcher, scope, holder, plugin,
                           &stats_handler](const std::string& code) {
      {
        std::lock_guard<std::mutex> guard(code_cache_mutex);
        auto& e = (*code_cache)[vm_config.code().remote().sha256()];
        e.in_progress = false;
        e.code = code;
        Stats::ScopeSharedPtr create_wasm_stats_scope = stats_handler.lockAndCreateStats(scope);
        if (code.empty()) {
          stats_handler.onEvent(WasmEvent::RemoteLoadCacheFetchFailure);
        } else {
          stats_handler.onEvent(WasmEvent::RemoteLoadCacheFetchSuccess);
        }
        stats_handler.onRemoteCacheEntriesChanged(code_cache->size());
      }
      // NB: xDS currently does not support failing asynchronously, so we fail immediately
      // if remote Wasm code is not cached and do a background fill.
      if (!vm_config.nack_on_code_cache_miss()) {
        if (code.empty()) {
          ENVOY_LOG_TO_LOGGER(Envoy::Logger::Registry::getLog(Envoy::Logger::Id::wasm), trace,
                              "Failed to load Wasm code (fetch failed) from {}", source);
        }
        complete_cb(code);
      }
      // NB: must be deleted explicitly.
      if (*holder) {
        dispatcher.deferredDelete(Envoy::Event::DeferredDeletablePtr{holder->release()});
      }
    };
    if (vm_config.nack_on_code_cache_miss()) {
      auto adapter = std::make_unique<RemoteDataFetcherAdapter>(fetch_callback);
      auto fetcher = std::make_unique<Config::DataFetcher::RemoteDataFetcher>(
          cluster_manager, vm_config.code().remote().http_uri(), vm_config.code().remote().sha256(),
          *adapter);
      auto fetcher_ptr = fetcher.get();
      adapter->setFetcher(std::move(fetcher));
      *holder = std::move(adapter);
      fetcher_ptr->fetch();
      ENVOY_LOG_TO_LOGGER(Envoy::Logger::Registry::getLog(Envoy::Logger::Id::wasm), trace,
                          fmt::format("Failed to load Wasm code (fetching) from {}", source));
      cb(nullptr);
      return false;
    } else {
      remote_data_provider = std::make_unique<Config::DataSource::RemoteAsyncDataProvider>(
          cluster_manager, init_manager, vm_config.code().remote(), dispatcher,
          api.randomGenerator(), true, fetch_callback);
    }
  } else {
    return complete_cb(code);
  }
  return true;
}

PluginHandleSharedPtr
getOrCreateThreadLocalPlugin(const WasmHandleSharedPtr& base_wasm, const PluginSharedPtr& plugin,
                             Event::Dispatcher& dispatcher,
                             CreateContextFn create_root_context_for_testing) {
  if (!base_wasm) {
    if (!plugin->fail_open_) {
      ENVOY_LOG_TO_LOGGER(Envoy::Logger::Registry::getLog(Envoy::Logger::Id::wasm), critical,
                          "Plugin configured to fail closed failed to load");
    }
    // To handle the case when failed to create VMs and fail-open/close properly,
    // we still create PluginHandle with null WasmBase.
    return std::make_shared<PluginHandle>(nullptr, plugin);
  }
  return std::static_pointer_cast<PluginHandle>(proxy_wasm::getOrCreateThreadLocalPlugin(
      std::static_pointer_cast<WasmHandle>(base_wasm), plugin,
      getWasmHandleCloneFactory(dispatcher, create_root_context_for_testing),
      getPluginHandleFactory()));
}

} // namespace Wasm
} // namespace Common
} // namespace Extensions
} // namespace Envoy
