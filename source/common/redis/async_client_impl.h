#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "envoy/stats/stats_macros.h"
#include "envoy/redis/async_client.h"

#include "source/common/network/address_impl.h"
#include "source/common/upstream/load_balancer_context_base.h"
#include "source/common/upstream/upstream_impl.h"
#include "source/extensions/common/redis/cluster_refresh_manager.h"
#include "source/extensions/filters/network/common/redis/raw_client_impl.h"

#include "absl/container/node_hash_map.h"

namespace Envoy {
namespace Upstream {
class ThreadLocalCluster;
}
namespace Redis {

#define REDIS_CLUSTER_STATS(COUNTER)                                                               \
  COUNTER(upstream_cx_drained)                                                                     \
  COUNTER(max_upstream_unknown_connections_reached)

struct RedisClusterStats {
  REDIS_CLUSTER_STATS(GENERATE_COUNTER_STRUCT)
};

using Envoy::Extensions::NetworkFilters::Common::Redis::RedisCommandStatsSharedPtr;
using Envoy::Extensions::NetworkFilters::Common::Redis::Client::Config;
using Envoy::Extensions::NetworkFilters::Common::Redis::Client::ConfigSharedPtr;
using Envoy::Extensions::NetworkFilters::Common::Redis::Client::ReadPolicy;

using Envoy::Extensions::NetworkFilters::Common::Redis::Client::RawClientCallbacks;
using Envoy::Extensions::NetworkFilters::Common::Redis::Client::RawClientFactory;
using Envoy::Extensions::NetworkFilters::Common::Redis::Client::RawClientPtr;

class ConfigImpl : public Config {
public:
  ConfigImpl()
      : op_timeout_(std::chrono::milliseconds(1000)), max_buffer_size_before_flush_(1024),
        buffer_flush_timeout_(3), max_upstream_unknown_connections_(100),
        enable_command_stats_(true) {}
  explicit ConfigImpl(const AsyncClientConfig& config)
      : op_timeout_(config.op_timeout_),
        max_buffer_size_before_flush_(config.max_buffer_size_before_flush_),
        buffer_flush_timeout_(config.buffer_flush_timeout_),
        max_upstream_unknown_connections_(config.max_upstream_unknown_connections_),
        enable_command_stats_(config.enable_command_stats_) {}

  std::chrono::milliseconds opTimeout() const override { return op_timeout_; }
  bool disableOutlierEvents() const override { return false; }
  bool enableHashtagging() const override { return false; }
  bool enableRedirection() const override { return false; }
  uint32_t maxBufferSizeBeforeFlush() const override { return max_buffer_size_before_flush_; }
  std::chrono::milliseconds bufferFlushTimeoutInMs() const override {
    return buffer_flush_timeout_;
  }
  uint32_t maxUpstreamUnknownConnections() const override {
    return max_upstream_unknown_connections_;
  }
  bool enableCommandStats() const override { return enable_command_stats_; }
  ReadPolicy readPolicy() const override { return ReadPolicy::Primary; }

  bool connectionRateLimitEnabled() const override { return false; }
  uint32_t connectionRateLimitPerSec() const override { return 0; }

  const std::chrono::milliseconds op_timeout_;
  const uint32_t max_buffer_size_before_flush_;
  const std::chrono::milliseconds buffer_flush_timeout_;
  const uint32_t max_upstream_unknown_connections_;
  const bool enable_command_stats_;
};

class AsyncClientImpl : public AsyncClient,
                        public std::enable_shared_from_this<AsyncClient>,
                        public Logger::Loggable<Logger::Id::redis> {
public:
  AsyncClientImpl(Upstream::ThreadLocalCluster* cluster, Event::Dispatcher& dispatcher,
                  RawClientFactory& client_factory, Stats::ScopeSharedPtr&& stats_scope,
                  RedisCommandStatsSharedPtr redis_command_stats,
                  Extensions::Common::Redis::ClusterRefreshManagerSharedPtr refresh_manager);
  ~AsyncClientImpl() override;

  // Envoy::Redis::AsyncClient
  void initialize(AsyncClientConfig config) override;
  PoolRequest* send(std::string&& query, Callbacks& callbacks,
                    const AsyncClient::RedisRequestOptions& options) override;
  PoolRequest* sendToHost(const std::string& host_address, std::string_view request,
                          RawClientCallbacks& callbacks);
  Event::Dispatcher& dispatcher() override { return dispatcher_; }

private:
  struct ThreadLocalActiveClient : public Network::ConnectionCallbacks {
    ThreadLocalActiveClient(AsyncClientImpl& parent) : parent_(parent) {}

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    AsyncClientImpl& parent_;
    Upstream::HostConstSharedPtr host_;
    RawClientPtr redis_client_;
  };

  using ThreadLocalActiveClientPtr = std::unique_ptr<ThreadLocalActiveClient>;

  struct PendingRequest : public RawClientCallbacks, public PoolRequest {
    PendingRequest(AsyncClientImpl& parent, std::string&& incoming_request, Callbacks& callbacks,
                   const AsyncClient::RedisRequestOptions& options);
    ~PendingRequest() override;

    // Common::Redis::Client::RawClientCallbacks
    void onResponse(std::string&& response) override;
    void onFailure() override;

    // PoolRequest
    void cancel() override;

    AsyncClientImpl& parent_;
    std::string incoming_request_;
    PoolRequest* request_handler_;
    Callbacks& callbacks_;
    Tracing::SpanPtr redis_span_;
  };

  void onHostsAdded(const std::vector<Upstream::HostSharedPtr>& host_added);
  void onHostsRemoved(const std::vector<Upstream::HostSharedPtr>& host_removed);
  void drainClients();

  ThreadLocalActiveClientPtr& threadLocalActiveClient(Upstream::HostConstSharedPtr host);

  void onRequestCompleted();

  // Returns true when `incoming` equals the currently installed configuration
  // (the auth/params members plus the fields held by `config_`). Used by
  // initialize() to skip connection teardown when nothing changed.
  bool currentConfigEquals(const AsyncClientConfig& incoming) const;

  const std::string cluster_name_;
  Upstream::ThreadLocalCluster* cluster_{};
  Event::Dispatcher& dispatcher_;
  absl::node_hash_map<Upstream::HostConstSharedPtr, ThreadLocalActiveClientPtr> client_map_;
  Envoy::Common::CallbackHandlePtr host_set_member_update_cb_handle_;
  absl::node_hash_map<std::string, Upstream::HostConstSharedPtr> host_address_map_;
  std::string auth_username_;
  std::string auth_password_;
  std::map<std::string, std::string> params_;
  std::list<Upstream::HostSharedPtr> created_via_redirect_hosts_;
  std::list<ThreadLocalActiveClientPtr> clients_to_drain_;
  std::list<PendingRequest> pending_requests_;

  Event::TimerPtr drain_timer_;
  RawClientFactory& client_factory_;
  ConfigSharedPtr config_;
  // False until the first initialize() installs a real config. config_ is
  // non-null from construction (default ConfigImpl), so this flag -- not a null
  // check -- is what distinguishes the first call from a reload.
  bool initialized_{false};
  Stats::ScopeSharedPtr stats_scope_;
  RedisCommandStatsSharedPtr redis_command_stats_;
  RedisClusterStats redis_cluster_stats_;
  const Extensions::Common::Redis::ClusterRefreshManagerSharedPtr refresh_manager_;
};

} // namespace Redis
} // namespace Envoy
