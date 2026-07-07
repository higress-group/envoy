#include "source/common/redis/async_client_impl.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/stats/utility.h"
#include "source/common/tracing/tracer_impl.h"

namespace Envoy {
namespace Redis {

AsyncClientImpl::AsyncClientImpl(
    Upstream::ThreadLocalCluster* cluster, Event::Dispatcher& dispatcher,
    RawClientFactory& client_factory, Stats::ScopeSharedPtr&& stats_scope,
    RedisCommandStatsSharedPtr redis_command_stats,
    Extensions::Common::Redis::ClusterRefreshManagerSharedPtr refresh_manager)
    : cluster_name_(cluster->info()->name()), cluster_(cluster), dispatcher_(dispatcher),
      drain_timer_(dispatcher.createTimer([this]() -> void { drainClients(); })),
      client_factory_(client_factory), config_(new ConfigImpl()), stats_scope_(stats_scope),
      redis_command_stats_(std::move(redis_command_stats)),
      redis_cluster_stats_{REDIS_CLUSTER_STATS(POOL_COUNTER(*stats_scope_))},
      refresh_manager_(std::move(refresh_manager)) {

  host_set_member_update_cb_handle_ = cluster_->prioritySet().addMemberUpdateCb(
      [this](const std::vector<Upstream::HostSharedPtr>& hosts_added,
             const std::vector<Upstream::HostSharedPtr>& hosts_removed) -> void {
        onHostsAdded(hosts_added);
        onHostsRemoved(hosts_removed);
      });

  for (const auto& i : cluster_->prioritySet().hostSetsPerPriority()) {
    for (auto& host : i->hosts()) {
      host_address_map_[host->address()->asString()] = host;
    }
  }
}

AsyncClientImpl::~AsyncClientImpl() {
  while (!pending_requests_.empty()) {
    pending_requests_.pop_front();
  }
  while (!client_map_.empty()) {
    client_map_.begin()->second->redis_client_->close();
  }
  while (!clients_to_drain_.empty()) {
    (*clients_to_drain_.begin())->redis_client_->close();
  }
}

bool AsyncClientImpl::currentConfigEquals(const AsyncClientConfig& incoming) const {
  // config_ holds the numeric/bool fields; auth/params live on this instance.
  return auth_username_ == incoming.auth_username_ &&
         auth_password_ == incoming.auth_password_ && params_ == incoming.params_ &&
         config_->opTimeout() == incoming.op_timeout_ &&
         config_->maxBufferSizeBeforeFlush() == incoming.max_buffer_size_before_flush_ &&
         config_->bufferFlushTimeoutInMs() == incoming.buffer_flush_timeout_ &&
         config_->maxUpstreamUnknownConnections() == incoming.max_upstream_unknown_connections_ &&
         config_->enableCommandStats() == incoming.enable_command_stats_;
}

void AsyncClientImpl::initialize(AsyncClientConfig config) {
  // Reloading a plugin re-invokes initialize() with the same Redis config. Tearing
  // down healthy connections in that case fails in-flight requests during the lazy
  // reconnect window (see higress #4086). Skip teardown when nothing changed.
  if (initialized_ && currentConfigEquals(config)) {
    return;
  }

  while (!client_map_.empty()) {
    client_map_.begin()->second->redis_client_->close();
  }
  while (!clients_to_drain_.empty()) {
    (*clients_to_drain_.begin())->redis_client_->close();
  }

  config_ = std::make_shared<ConfigImpl>(config);
  auth_username_ = config.auth_username_;
  auth_password_ = config.auth_password_;
  params_ = config.params_;
  initialized_ = true;
}

PoolRequest* AsyncClientImpl::send(std::string&& query, AsyncClient::Callbacks& callbacks,
                                   const AsyncClient::RedisRequestOptions& options) {
  if (cluster_ == nullptr) {
    ASSERT(client_map_.empty());
    ASSERT(host_set_member_update_cb_handle_ == nullptr);
    return nullptr;
  }

  Upstream::LoadBalancerContextBase lb_context;
  auto host_selection = cluster_->loadBalancer().chooseHost(&lb_context);
  Upstream::HostConstSharedPtr host = host_selection.host;
  if (!host) {
    ENVOY_LOG(debug, "no available host");
    return nullptr;
  }
  pending_requests_.emplace_back(*this, std::move(query), callbacks, options);
  PendingRequest& pending_request = pending_requests_.back();
  ThreadLocalActiveClientPtr& client = this->threadLocalActiveClient(host);
  pending_request.request_handler_ =
      client->redis_client_->makeRawRequest(pending_request.incoming_request_, pending_request);
  if (pending_request.request_handler_) {
    return &pending_request;
  } else {
    onRequestCompleted();
    return nullptr;
  }
}

PoolRequest* AsyncClientImpl::sendToHost(const std::string& host_address, std::string_view request,
                                         RawClientCallbacks& callbacks) {
  if (cluster_ == nullptr) {
    ASSERT(client_map_.empty());
    ASSERT(host_set_member_update_cb_handle_ == nullptr);
    return nullptr;
  }

  auto colon_pos = host_address.rfind(':');
  if ((colon_pos == std::string::npos) || (colon_pos == (host_address.size() - 1))) {
    return nullptr;
  }

  const std::string ip_address = host_address.substr(0, colon_pos);
  const bool ipv6 = (ip_address.find(':') != std::string::npos);
  std::string host_address_map_key;
  Network::Address::InstanceConstSharedPtr address_ptr;

  if (!ipv6) {
    host_address_map_key = host_address;
  } else {
    const auto ip_port = absl::string_view(host_address).substr(colon_pos + 1);
    uint32_t ip_port_number;
    if (!absl::SimpleAtoi(ip_port, &ip_port_number) || (ip_port_number > 65535)) {
      return nullptr;
    }
    try {
      address_ptr = std::make_shared<Network::Address::Ipv6Instance>(ip_address, ip_port_number);
    } catch (const EnvoyException&) {
      return nullptr;
    }
    host_address_map_key = address_ptr->asString();
  }

  auto it = host_address_map_.find(host_address_map_key);
  if (it == host_address_map_.end()) {
    // This host is not known to the cluster manager. Create a new host and insert it into the map.
    if (created_via_redirect_hosts_.size() == config_->maxUpstreamUnknownConnections()) {
      // Too many upstream connections to unknown hosts have been created.
      redis_cluster_stats_.max_upstream_unknown_connections_reached_.inc();
      return nullptr;
    }
    if (!ipv6) {
      // Only create an IPv4 address instance if we need a new Upstream::HostImpl.
      const auto ip_port = absl::string_view(host_address).substr(colon_pos + 1);
      uint32_t ip_port_number;
      if (!absl::SimpleAtoi(ip_port, &ip_port_number) || (ip_port_number > 65535)) {
        return nullptr;
      }
      try {
        address_ptr = std::make_shared<Network::Address::Ipv4Instance>(ip_address, ip_port_number);
      } catch (const EnvoyException&) {
        return nullptr;
      }
    }
    auto host_or_error = Upstream::HostImpl::create(
        cluster_->info(), "", address_ptr, nullptr, nullptr, 1, envoy::config::core::v3::Locality(),
        envoy::config::endpoint::v3::Endpoint::HealthCheckConfig::default_instance(), 0,
        envoy::config::core::v3::UNKNOWN);
    if (!host_or_error.ok()) {
      return nullptr;
    }
    Upstream::HostSharedPtr new_host =
        std::shared_ptr<Upstream::HostImpl>(std::move(host_or_error.value()));
    host_address_map_[host_address_map_key] = new_host;
    created_via_redirect_hosts_.push_back(new_host);
    it = host_address_map_.find(host_address_map_key);
  }

  ThreadLocalActiveClientPtr& client = threadLocalActiveClient(it->second);

  return client->redis_client_->makeRawRequest(request, callbacks);
}

void AsyncClientImpl::ThreadLocalActiveClient::onEvent(Network::ConnectionEvent event) {
  if (event == Network::ConnectionEvent::RemoteClose ||
      event == Network::ConnectionEvent::LocalClose) {
    auto client_to_delete = parent_.client_map_.find(host_);
    if (client_to_delete != parent_.client_map_.end()) {
      parent_.dispatcher_.deferredDelete(std::move(redis_client_));
      parent_.client_map_.erase(client_to_delete);
    } else {
      for (auto it = parent_.clients_to_drain_.begin(); it != parent_.clients_to_drain_.end();
           it++) {
        if ((*it).get() == this) {
          if (!redis_client_->active()) {
            parent_.redis_cluster_stats_.upstream_cx_drained_.inc();
          }
          parent_.dispatcher_.deferredDelete(std::move(redis_client_));
          parent_.clients_to_drain_.erase(it);
          break;
        }
      }
    }
  }
}

AsyncClientImpl::PendingRequest::PendingRequest(AsyncClientImpl& parent,
                                                std::string&& incoming_request,
                                                Callbacks& callbacks,
                                                const AsyncClient::RedisRequestOptions& options)
    : parent_(parent), incoming_request_(incoming_request), callbacks_(callbacks) {
  // Create child span for Redis call tracing
  if (nullptr != options.parent_span_) {
    const std::string child_span_name =
        options.child_span_name_.empty()
            ? absl::StrCat("redis ", parent_.cluster_name_, " egress")
            : options.child_span_name_;
    redis_span_ = options.parent_span_->spawnChild(Tracing::EgressConfig::get(), child_span_name,
                                                   parent_.dispatcher_.timeSource().systemTime());
    
    // Set sampling decision
    if (options.sampled_.has_value()) {
      redis_span_->setSampled(options.sampled_.value());
    }
  } else {
    redis_span_ = std::make_unique<Tracing::NullSpan>();
  }
}

AsyncClientImpl::PendingRequest::~PendingRequest() {
  if (request_handler_) {
    request_handler_->cancel();
    request_handler_ = nullptr;

    // treat canceled request as failure
    callbacks_.onFailure(incoming_request_);
  }
}

void AsyncClientImpl::PendingRequest::onResponse(std::string&& response) {
  // Finish Redis span
  if (redis_span_) {
    redis_span_->setTag(Tracing::Tags::get().UpstreamCluster, parent_.cluster_name_);
    redis_span_->setTag("redis.status", "success");
    redis_span_->setTag(Tracing::Tags::get().Component, "redis");
    redis_span_->finishSpan();
  }
  
  request_handler_ = nullptr;
  callbacks_.onSuccess(incoming_request_, std::move(response));
  parent_.onRequestCompleted();
}

void AsyncClientImpl::PendingRequest::onFailure() {
  // Finish Redis span
  if (redis_span_) {
    redis_span_->setTag(Tracing::Tags::get().UpstreamCluster, parent_.cluster_name_);
    redis_span_->setTag("redis.status", "failure");
    redis_span_->setTag(Tracing::Tags::get().Component, "redis");
    redis_span_->setTag(Tracing::Tags::get().Error, Tracing::Tags::get().True);
    redis_span_->finishSpan();
  }
  
  request_handler_ = nullptr;
  callbacks_.onFailure(incoming_request_);
  // refresh_manager is not constructed
  // parent.refresh_manager_->onFailure(parent_.cluster_name);
  parent_.onRequestCompleted();
}

void AsyncClientImpl::PendingRequest::cancel() {
  request_handler_->cancel();
  request_handler_ = nullptr;
  parent_.onRequestCompleted();
}

void AsyncClientImpl::onHostsAdded(const std::vector<Upstream::HostSharedPtr>& host_added) {
  for (const auto& host : host_added) {
    std::string host_address = host->address()->asString();
    // Insert new host into address map, possibly overwriting a previous host's entry.
    host_address_map_[host_address] = host;
    for (const auto& created_host : created_via_redirect_hosts_) {
      if (created_host->address()->asString() == host_address) {
        // Remove our "temporary" host create in sendRequestToHost().
        onHostsRemoved({created_host});
        created_via_redirect_hosts_.remove(created_host);
        break;
      }
    }
  }
}

void AsyncClientImpl::onHostsRemoved(const std::vector<Upstream::HostSharedPtr>& host_removed) {
  for (const auto& host : host_removed) {
    auto it = client_map_.find(host);
    if (it != client_map_.end()) {
      if (it->second->redis_client_->active()) {
        clients_to_drain_.push_back(std::move(it->second));
        client_map_.erase(it);
        if (!drain_timer_->enabled()) {
          drain_timer_->enableTimer(std::chrono::seconds(1));
        }
      } else {
        // There is no pending requests so close the connection
        it->second->redis_client_->close();
      }
    }
    // There is the possibility that multiple hosts with the same address
    // are registered in host_address_map_ given that hosts may be created
    // upon redirection or supplied as part of the cluster's definition.
    // only remove cluster defined host here.
    auto it2 = host_address_map_.find(host->address()->asString());
    if (it2 != host_address_map_.end() && (it2->second == host)) {
      host_address_map_.erase(it2);
    }
  }
}

void AsyncClientImpl::drainClients() {
  while (!clients_to_drain_.empty() && !(*clients_to_drain_.begin())->redis_client_->active()) {
    (*clients_to_drain_.begin())->redis_client_->close();
  }
  if (!clients_to_drain_.empty()) {
    drain_timer_->enableTimer(std::chrono::seconds(1));
  }
}

AsyncClientImpl::ThreadLocalActiveClientPtr&
AsyncClientImpl::threadLocalActiveClient(Upstream::HostConstSharedPtr host) {
  ThreadLocalActiveClientPtr& client = client_map_[host];
  if (!client) {
    client = std::make_unique<ThreadLocalActiveClient>(*this);
    client->host_ = host;
    client->redis_client_ =
        client_factory_.create(host, dispatcher_, config_, redis_command_stats_, *(stats_scope_),
                               auth_username_, auth_password_, params_);
    client->redis_client_->addConnectionCallbacks(*client);
  }
  return client;
}

void AsyncClientImpl::onRequestCompleted() {
  ASSERT(!pending_requests_.empty());

  while (!pending_requests_.empty() && !pending_requests_.front().request_handler_) {
    pending_requests_.pop_front();
  }
}

} // namespace Redis
} // namespace Envoy
