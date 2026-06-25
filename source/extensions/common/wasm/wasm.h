#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>

#include "envoy/common/exception.h"
#include "envoy/extensions/wasm/v3/wasm.pb.validate.h"
#include "envoy/http/filter.h"
#if defined(HIGRESS)
#include "envoy/runtime/runtime.h"
#endif
#include "envoy/server/lifecycle_notifier.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/config/datasource.h"
#include "source/common/stats/symbol_table.h"
#include "source/common/version/version.h"
#include "source/extensions/common/wasm/context.h"
#include "source/extensions/common/wasm/plugin.h"
#include "source/extensions/common/wasm/stats_handler.h"
#include "source/extensions/common/wasm/wasm_vm.h"

#include "include/proxy-wasm/exports.h"
#include "include/proxy-wasm/wasm.h"

#if defined(HIGRESS)
#include "absl/types/optional.h"
#endif

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Wasm {

using CreateContextFn =
    std::function<ContextBase*(Wasm* wasm, const std::shared_ptr<Plugin>& plugin)>;

class WasmHandle;

// Wasm execution instance. Manages the Envoy side of the Wasm interface.
class Wasm : public WasmBase, Logger::Loggable<Logger::Id::wasm> {
public:
  Wasm(WasmConfig& config, absl::string_view vm_key, const Stats::ScopeSharedPtr& scope,
       Api::Api& api, Upstream::ClusterManager& cluster_manager, Event::Dispatcher& dispatcher
#if defined(HIGRESS)
       ,
       Runtime::Loader* runtime = nullptr
#endif
  );
  Wasm(std::shared_ptr<WasmHandle> other, Event::Dispatcher& dispatcher
#if defined(HIGRESS)
       ,
       Runtime::Loader* runtime = nullptr
#endif
  );
  ~Wasm() override;

  Upstream::ClusterManager& clusterManager() const { return cluster_manager_; }
  Event::Dispatcher& dispatcher() { return dispatcher_; }
  Api::Api& api() { return api_; }
  Context* getRootContext(const std::shared_ptr<PluginBase>& plugin, bool allow_closed) {
    return static_cast<Context*>(WasmBase::getRootContext(plugin, allow_closed));
  }
  void setTimerPeriod(uint32_t root_context_id, std::chrono::milliseconds period) override;
  virtual void tickHandler(uint32_t root_context_id);
  std::shared_ptr<Wasm> sharedThis() { return std::static_pointer_cast<Wasm>(shared_from_this()); }
  Network::DnsResolverSharedPtr& dnsResolver() { return dns_resolver_; }

  // WasmBase
  void error(std::string_view message) override;
  proxy_wasm::CallOnThreadFunction callOnThreadFunction() override;
  ContextBase* createContext(const std::shared_ptr<PluginBase>& plugin) override;
  ContextBase* createRootContext(const std::shared_ptr<PluginBase>& plugin) override;
  ContextBase* createVmContext() override;
  void registerCallbacks() override;
  void getFunctions() override;

  // AccessLog::Instance
  void log(const PluginSharedPtr& plugin, const Http::RequestHeaderMap* request_headers,
           const Http::ResponseHeaderMap* response_headers,
           const Http::ResponseTrailerMap* response_trailers,
           const StreamInfo::StreamInfo& stream_info, AccessLog::AccessLogType access_log_type);

  void onStatsUpdate(const PluginSharedPtr& plugin, Envoy::Stats::MetricSnapshot& snapshot);

  virtual std::string buildVersion() { return BUILD_VERSION_NUMBER; }

  void initializeLifecycle(Server::ServerLifecycleNotifier& lifecycle_notifier);

#if defined(HIGRESS)
  void initializeRuntimeStatsTimer();
  uint64_t reclaimMemoryThreshold() const;
  void markReclaimEligible();
  void clearReclaimEligible() { reclaim_eligible_since_.reset(); }
  const absl::optional<MonotonicTime>& reclaimEligibleSinceForTesting() const {
    return reclaim_eligible_since_;
  }
#endif

  uint32_t nextDnsToken() {
    do {
      dns_token_++;
    } while (!dns_token_);
    return dns_token_;
  }

  void setCreateContextForTesting(CreateContextFn create_context,
                                  CreateContextFn create_root_context) {
    create_context_for_testing_ = create_context;
    create_root_context_for_testing_ = create_root_context;
  }
  void setFailStateForTesting(proxy_wasm::FailState fail_state) { failed_ = fail_state; }

#if defined(HIGRESS)
  LifecycleStats& lifecycleStats() { return lifecycle_stats_handler_.stats(); }
  void recordReclaimLatency(MonotonicTime now);
  void incrementActiveStreamCount() { ++active_stream_count_; }
  void decrementActiveStreamCount() {
    ASSERT(active_stream_count_ > 0);
    if (active_stream_count_ == 0) {
      ENVOY_LOG(error, "wasm active stream count underflow");
      return;
    }
    --active_stream_count_;
  }
  uint64_t activeStreamCount() const { return active_stream_count_; }
#endif

protected:
  friend class Context;

  void initializeStats();
  // Calls into the VM.
  proxy_wasm::WasmCallVoid<3> on_resolve_dns_;
  proxy_wasm::WasmCallVoid<2> on_stats_update_;

  Stats::ScopeSharedPtr scope_;
  Api::Api& api_;
  Stats::StatNamePool stat_name_pool_;
  const Stats::StatName custom_stat_namespace_;
  Upstream::ClusterManager& cluster_manager_;
  Event::Dispatcher& dispatcher_;
  Event::PostCb server_shutdown_post_cb_;
  absl::flat_hash_map<uint32_t, Event::TimerPtr> timer_; // per root_id.
  TimeSource& time_source_;

#if defined(HIGRESS)
  // Runtime stats
  RuntimeStatsHandler runtime_stats_handler_;
  Event::TimerPtr runtime_stats_timer_;
  static constexpr std::chrono::milliseconds kRuntimeStatsInterval{1000};
  uint64_t active_stream_count_ = 0;
  Runtime::Loader* runtime_loader_;
  absl::optional<MonotonicTime> reclaim_eligible_since_;
#endif

  // Lifecycle stats
  LifecycleStatsHandler lifecycle_stats_handler_;

  // Plugin stats
  absl::flat_hash_map<uint32_t, Stats::Counter*> counters_;
  absl::flat_hash_map<uint32_t, Stats::Gauge*> gauges_;
  absl::flat_hash_map<uint32_t, Stats::Histogram*> histograms_;

  CreateContextFn create_context_for_testing_;
  CreateContextFn create_root_context_for_testing_;
  Network::DnsResolverSharedPtr dns_resolver_;
  uint32_t dns_token_ = 1;
};
using WasmSharedPtr = std::shared_ptr<Wasm>;

class WasmHandle : public WasmHandleBase, public ThreadLocal::ThreadLocalObject {
public:
  explicit WasmHandle(const WasmSharedPtr& wasm)
      : WasmHandleBase(std::static_pointer_cast<WasmBase>(wasm)), wasm_(wasm) {}

  WasmSharedPtr& wasm() { return wasm_; }

private:
  WasmSharedPtr wasm_;
};

using WasmHandleSharedPtr = std::shared_ptr<WasmHandle>;

class PluginHandle : public PluginHandleBase {
public:
  explicit PluginHandle(const WasmHandleSharedPtr& wasm_handle, const PluginSharedPtr& plugin)
      : PluginHandleBase(std::static_pointer_cast<WasmHandleBase>(wasm_handle),
                         std::static_pointer_cast<PluginBase>(plugin)),
        plugin_(plugin), wasm_handle_(wasm_handle) {}

  WasmHandleSharedPtr& wasmHandle() { return wasm_handle_; }
  uint32_t rootContextId() { return wasm_handle_->wasm()->getRootContext(plugin_, false)->id(); }

private:
  PluginSharedPtr plugin_;
  WasmHandleSharedPtr wasm_handle_;
};

using PluginHandleSharedPtr = std::shared_ptr<PluginHandle>;

#if defined(HIGRESS)
enum class RebuildSource { Explicit, Memory };

class PluginHandleSharedPtrThreadLocal : public ThreadLocal::ThreadLocalObject,
                                         public Logger::Loggable<Logger::Id::wasm> {
public:
  PluginHandleSharedPtrThreadLocal(PluginHandleSharedPtr handle,
                                   WasmHandleSharedPtr base_wasm = nullptr,
                                   bool enable_reclaim_timer = false);
  ~PluginHandleSharedPtrThreadLocal() override;
  bool rebuild(bool is_fail_recovery = false, RebuildSource source = RebuildSource::Explicit);
  void runReclaimTimerForTesting();
#else
class PluginHandleSharedPtrThreadLocal : public ThreadLocal::ThreadLocalObject {
public:
  PluginHandleSharedPtrThreadLocal(PluginHandleSharedPtr handle) : handle_(handle){};
#endif
  PluginHandleSharedPtr& handle() { return handle_; }

private:
#if defined(HIGRESS)
  void initializeReclaimTimer();
  void onReclaimTimer();
  bool syncHandleToCurrentGeneration(const std::string& vm_key);
  PluginSharedPtr plugin_;
  WasmHandleSharedPtr base_wasm_;
  Event::TimerPtr reclaim_timer_;
  bool reclaim_timer_enabled_;
  static constexpr std::chrono::milliseconds kReclaimTimerInterval{1000};
#endif
  PluginHandleSharedPtr handle_;
};

using CreateWasmCallback = std::function<void(WasmHandleSharedPtr)>;

// Returns false if createWasm failed synchronously. This is necessary because xDS *MUST* report
// all failures synchronously as it has no facility to report configuration update failures
// asynchronously. Callers should throw an exception if they are part of a synchronous xDS update
// because that is the mechanism for reporting configuration errors.
bool createWasm(const PluginSharedPtr& plugin, const Stats::ScopeSharedPtr& scope,
                Upstream::ClusterManager& cluster_manager, Init::Manager& init_manager,
                Event::Dispatcher& dispatcher, Api::Api& api,
                Envoy::Server::ServerLifecycleNotifier& lifecycle_notifier,
                Config::DataSource::RemoteAsyncDataProviderPtr& remote_data_provider,
                CreateWasmCallback&& callback,
                CreateContextFn create_root_context_for_testing = nullptr
#if defined(HIGRESS)
                ,
                Runtime::Loader* runtime = nullptr
#endif
);

PluginHandleSharedPtr
getOrCreateThreadLocalPlugin(const WasmHandleSharedPtr& base_wasm, const PluginSharedPtr& plugin,
                             Event::Dispatcher& dispatcher,
                             CreateContextFn create_root_context_for_testing = nullptr);

void clearCodeCacheForTesting();
void setTimeOffsetForCodeCacheForTesting(MonotonicTime::duration d);
#if defined(HIGRESS)
size_t rebuildGuardRegistrySizeForTesting();
#endif
WasmEvent toWasmEvent(const std::shared_ptr<WasmHandleBase>& wasm);

} // namespace Wasm
} // namespace Common
} // namespace Extensions
} // namespace Envoy
