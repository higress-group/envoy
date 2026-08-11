#pragma once

#include <memory>

#include "envoy/server/lifecycle_notifier.h"

#ifdef HIGRESS
#include "envoy/stats/histogram.h"
#endif
#include "envoy/stats/scope.h"
#include "envoy/stats/stats.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/common/logger.h"
#include "source/common/stats/symbol_table.h"

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Wasm {

// The custom stat namespace which prepends all the user-defined metrics.
// Note that the prefix is removed from the final output of /stats endpoints.
constexpr absl::string_view CustomStatNamespace = "wasmcustom";

#define CREATE_WASM_STATS(COUNTER, GAUGE)                                                          \
  COUNTER(remote_load_cache_hits)                                                                  \
  COUNTER(remote_load_cache_negative_hits)                                                         \
  COUNTER(remote_load_cache_misses)                                                                \
  COUNTER(remote_load_fetch_successes)                                                             \
  COUNTER(remote_load_fetch_failures)                                                              \
  GAUGE(remote_load_cache_entries, NeverImport)

struct CreateWasmStats {
  CREATE_WASM_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

#ifdef HIGRESS
#define LIFECYCLE_STATS(COUNTER, GAUGE, PLUGIN_COUNTER, PLUGIN_GAUGE, PLUGIN_HISTOGRAM)            \
  COUNTER(created)                                                                                 \
  GAUGE(active, NeverImport)                                                                       \
  PLUGIN_COUNTER(recover_total)                                                                    \
  PLUGIN_COUNTER(rebuild_total)                                                                    \
  PLUGIN_COUNTER(rebuild_memory_total)                                                             \
  PLUGIN_COUNTER(rebuild_explicit_total)                                                           \
  PLUGIN_COUNTER(crash_total)                                                                      \
  PLUGIN_COUNTER(recover_error)                                                                    \
  PLUGIN_GAUGE(crash, NeverImport)                                                                 \
  PLUGIN_HISTOGRAM(reclaim_latency, Milliseconds)
#else
#define LIFECYCLE_STATS(COUNTER, GAUGE)                                                            \
  COUNTER(created)                                                                                 \
  GAUGE(active, NeverImport)
#endif

#ifdef HIGRESS
#define RUNTIME_STATS(PLUGIN_GAUGE) PLUGIN_GAUGE(memory_size, NeverImport)

#define WORKER_INIT_STATS(COUNTER, GAUGE)                                                          \
  COUNTER(worker_init_retryable_failure_total)                                                     \
  COUNTER(worker_init_terminal_failure_total)                                                      \
  COUNTER(worker_init_retry_total)                                                                 \
  COUNTER(worker_init_recovered_total)                                                             \
  COUNTER(worker_fail_open_skip_total)                                                             \
  GAUGE(worker_uninitialized, NeverImport)

struct RuntimeStats {
  RUNTIME_STATS(GENERATE_GAUGE_STRUCT)
};

struct WorkerInitStats {
  WORKER_INIT_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};
#endif

struct LifecycleStats {
#ifdef HIGRESS
  LIFECYCLE_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT, GENERATE_COUNTER_STRUCT,
                  GENERATE_GAUGE_STRUCT, GENERATE_HISTOGRAM_STRUCT)
#else
  LIFECYCLE_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
#endif
};

using ScopeWeakPtr = std::weak_ptr<Stats::Scope>;

enum class WasmEvent : int {
  Ok,
  RemoteLoadCacheHit,
  RemoteLoadCacheNegativeHit,
  RemoteLoadCacheMiss,
  RemoteLoadCacheFetchSuccess,
  RemoteLoadCacheFetchFailure,
  UnableToCreateVm,
  UnableToCloneVm,
  MissingFunction,
  UnableToInitializeCode,
  StartFailed,
  ConfigureFailed,
  RuntimeError,
  VmCreated,
  VmShutDown,
#ifdef HIGRESS
  RecoverError,
#endif
};

class CreateStatsHandler : Logger::Loggable<Logger::Id::wasm> {
public:
  CreateStatsHandler() = default;
  ~CreateStatsHandler() = default;

  void initialize();

  void onEvent(WasmEvent event);
  void onRemoteCacheEntriesChanged(int remote_cache_entries);
  void createStats(const Stats::ScopeSharedPtr& scope) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  void resetStats() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_); // Delete stats pointers

  // NB: the Scope can become invalid if, for example, the owning FilterChain is deleted. When that
  // happens the stats must be recreated. This hook verifies the Scope of any existing stats and if
  // necessary recreates the stats with the newly provided scope.
  // This call takes out the mutex_ and calls createStats and possibly resetStats().
  Stats::ScopeSharedPtr lockAndCreateStats(const Stats::ScopeSharedPtr& scope);

  void resetStatsForTesting();

protected:
  absl::Mutex mutex_;
  ScopeWeakPtr scope_;
  std::unique_ptr<CreateWasmStats> create_wasm_stats_;
};

CreateStatsHandler& getCreateStatsHandler();

class LifecycleStatsHandler {
public:
#ifdef HIGRESS
  LifecycleStatsHandler(const Stats::ScopeSharedPtr& scope, std::string runtime,
                        std::string plugin_name)
      : lifecycle_stats_(LifecycleStats{LIFECYCLE_STATS(
            POOL_COUNTER_PREFIX(*scope, absl::StrCat("wasm.", runtime, ".")),
            POOL_GAUGE_PREFIX(*scope, absl::StrCat("wasm.", runtime, ".")),
            POOL_COUNTER_PREFIX(*scope,
                                absl::StrCat("wasm.", runtime, ".plugin.", plugin_name, ".")),
            POOL_GAUGE_PREFIX(*scope, absl::StrCat("wasm.", runtime, ".plugin.", plugin_name, ".")),
            POOL_HISTOGRAM_PREFIX(
                *scope, absl::StrCat("wasm.", runtime, ".plugin.", plugin_name, ".")))}){};
#else
  LifecycleStatsHandler(const Stats::ScopeSharedPtr& scope, std::string runtime)
      : lifecycle_stats_(LifecycleStats{
            LIFECYCLE_STATS(POOL_COUNTER_PREFIX(*scope, absl::StrCat("wasm.", runtime, ".")),
                            POOL_GAUGE_PREFIX(*scope, absl::StrCat("wasm.", runtime, ".")))}){};
#endif
  ~LifecycleStatsHandler() = default;

  void onEvent(WasmEvent event);
  static int64_t getActiveVmCount();

#ifdef HIGRESS
  LifecycleStats& stats() { return lifecycle_stats_; }
#endif

protected:
  LifecycleStats lifecycle_stats_;
#ifdef HIGRESS
  bool is_crashed_ = false;
#endif
};

#ifdef HIGRESS
class RuntimeStatsHandler {
public:
  RuntimeStatsHandler(const Stats::ScopeSharedPtr& scope, const std::string& runtime,
                      const std::string& plugin_name, const std::string& thread_name)
      : runtime(runtime), plugin_name(plugin_name),
        runtime_stats_(RuntimeStats{RUNTIME_STATS(
            POOL_GAUGE_PREFIX(*scope, absl::StrCat("wasm.", runtime, ".plugin.", plugin_name, ".",
                                                   thread_name, ".")))}) {}

  ~RuntimeStatsHandler() = default;

  std::string runtime;
  std::string plugin_name;
  void updateMemorySize(uint64_t memory_size) { runtime_stats_.memory_size_.set(memory_size); }

protected:
  RuntimeStats runtime_stats_;
};

class WorkerInitStatsHandler {
public:
  WorkerInitStatsHandler(const Stats::ScopeSharedPtr& scope, const std::string& runtime,
                         const std::string& plugin_name)
      : scope_(scope),
        worker_init_stats_(WorkerInitStats{WORKER_INIT_STATS(
            POOL_COUNTER_PREFIX(*scope_,
                                absl::StrCat("wasm.", runtime, ".plugin.", plugin_name, ".")),
            POOL_GAUGE_PREFIX(*scope_,
                              absl::StrCat("wasm.", runtime, ".plugin.", plugin_name, ".")))}) {}

  WorkerInitStats& stats() { return worker_init_stats_; }

private:
  // Keep the scope alive for the lifetime of the references stored in worker_init_stats_.
  Stats::ScopeSharedPtr scope_;
  WorkerInitStats worker_init_stats_;
};

using WorkerInitStatsHandlerSharedPtr = std::shared_ptr<WorkerInitStatsHandler>;
#endif

} // namespace Wasm
} // namespace Common
} // namespace Extensions
} // namespace Envoy
