#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "source/common/redis/async_client_impl.h"
#include "source/common/stats/isolated_store_impl.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/thread_local_cluster.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Invoke;
using testing::NiceMock;

namespace Envoy {
namespace Redis {
namespace {

namespace RedisClient = Extensions::NetworkFilters::Common::Redis::Client;

// Minimal RawClient double. close() emulates the real RawClientImpl by invoking the
// registered ConnectionCallbacks with LocalClose, which is what makes
// AsyncClientImpl::initialize()'s `while (!client_map_.empty())` loop terminate.
class FakeRawClient : public RedisClient::RawClient {
public:
  explicit FakeRawClient(int& close_count) : close_count_(close_count) {}

  void addConnectionCallbacks(Network::ConnectionCallbacks& callbacks) override {
    callbacks_ = &callbacks;
  }
  bool active() override { return false; }
  void close() override {
    ++close_count_;
    // May delete `this` via deferredDelete + client_map_ erase; touch nothing after.
    if (callbacks_ != nullptr) {
      callbacks_->onEvent(Network::ConnectionEvent::LocalClose);
    }
  }
  RedisClient::PoolRequest* makeRawRequest(std::string_view, RedisClient::RawClientCallbacks&) override {
    return nullptr;
  }
  void initialize(const std::string&, const std::string&,
                  const std::map<std::string, std::string>&) override {}

private:
  int& close_count_;
  Network::ConnectionCallbacks* callbacks_{nullptr};
};

class FakeRawClientFactory : public RedisClient::RawClientFactory {
public:
  explicit FakeRawClientFactory(int& close_count) : close_count_(close_count) {}

  RedisClient::RawClientPtr create(Upstream::HostConstSharedPtr, Event::Dispatcher&,
                                   RedisClient::ConfigSharedPtr,
                                   const RedisClient::RedisCommandStatsSharedPtr&, Stats::Scope&,
                                   const std::string&, const std::string&,
                                   const std::map<std::string, std::string>&) override {
    ++created_;
    return std::make_unique<FakeRawClient>(close_count_);
  }

  int created_{0};

private:
  int& close_count_;
};

// Builds an AsyncClientConfig with per-field overrides so tests can flip exactly one field.
AsyncClientConfig makeConfig(std::string username = "user", std::string password = "pass",
                             int op_timeout_ms = 1000,
                             std::map<std::string, std::string> params = {{"a", "1"}, {"b", "2"}}) {
  return AsyncClientConfig(std::move(username), std::move(password), op_timeout_ms,
                           std::move(params));
}

class AsyncClientImplTest : public testing::Test {
public:
  AsyncClientImplTest() {
    host_ = std::make_shared<NiceMock<Upstream::MockHost>>();
    // HostSelectionResponse is move-only, so build a fresh one per call via Invoke.
    ON_CALL(cluster_.lb_, chooseHost(_)).WillByDefault(Invoke([this](Upstream::LoadBalancerContext*) {
      return Upstream::HostSelectionResponse{host_};
    }));
    client_ = std::make_unique<AsyncClientImpl>(
        &cluster_, dispatcher_, factory_, stats_.rootScope(), /*redis_command_stats=*/nullptr,
        /*refresh_manager=*/nullptr);
  }

  // Populates client_map_ with one connection so a subsequent teardown is observable.
  void openOneClient() {
    factory_.created_ = 0;
    AsyncClient::RedisRequestOptions options;
    client_->send("PING", callbacks_, options);
    EXPECT_EQ(factory_.created_, 1);
  }

  NiceMock<Upstream::MockThreadLocalCluster> cluster_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  Stats::IsolatedStoreImpl stats_;
  int close_count_{0};
  FakeRawClientFactory factory_{close_count_};
  std::shared_ptr<Upstream::MockHost> host_;
  MockRedisAsyncClientCallbacks callbacks_;
  std::unique_ptr<AsyncClientImpl> client_;
};

// SPEC-001: identical config on reload keeps healthy connections (no close()).
TEST_F(AsyncClientImplTest, IdenticalConfigSkipsTeardown) {
  client_->initialize(makeConfig());
  openOneClient();

  close_count_ = 0;
  client_->initialize(makeConfig()); // equal to installed config
  EXPECT_EQ(close_count_, 0);        // connection preserved
}

// SPEC-001: the first initialize() always installs, even if it equals ConfigImpl defaults.
TEST_F(AsyncClientImplTest, FirstCallInstallsThenShortCircuits) {
  // Defaults happen to match ConfigImpl's constructed values; the initialized_ flag,
  // not value coincidence, is what forces the first call through the install path.
  auto default_like = makeConfig("", "", 1000, {});
  client_->initialize(default_like);
  openOneClient();

  close_count_ = 0;
  client_->initialize(makeConfig("", "", 1000, {}));
  EXPECT_EQ(close_count_, 0);
}

// SPEC-002/003: changing any single field forces the full teardown path.
TEST_F(AsyncClientImplTest, EachFieldChangeTriggersTeardown) {
  const std::vector<AsyncClientConfig> changed = {
      makeConfig("other-user"),
      makeConfig("user", "other-pass"),
      makeConfig("user", "pass", 2000),
      makeConfig("user", "pass", 1000, {{"a", "1"}, {"b", "changed"}}),
      makeConfig("user", "pass", 1000, {{"a", "1"}}),
  };

  for (const auto& next : changed) {
    // Reset to the baseline config with one open connection.
    client_ = std::make_unique<AsyncClientImpl>(&cluster_, dispatcher_, factory_, stats_.rootScope(),
                                                nullptr, nullptr);
    client_->initialize(makeConfig());
    openOneClient();

    close_count_ = 0;
    client_->initialize(next);
    EXPECT_EQ(close_count_, 1) << "expected teardown when a config field differs";
  }
}

// SPEC-003: same key/value set built in different insertion order compares equal.
TEST_F(AsyncClientImplTest, ReorderedParamsAreEqual) {
  client_->initialize(makeConfig("user", "pass", 1000, {{"a", "1"}, {"b", "2"}}));
  openOneClient();

  close_count_ = 0;
  client_->initialize(makeConfig("user", "pass", 1000, {{"b", "2"}, {"a", "1"}}));
  EXPECT_EQ(close_count_, 0);
}

} // namespace
} // namespace Redis
} // namespace Envoy
