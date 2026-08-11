#include <chrono>

#include "envoy/extensions/filters/http/wasm/v3/wasm.pb.validate.h"

#include "source/common/common/base64.h"
#include "source/common/common/hex.h"
#include "source/common/crypto/utility.h"
#include "source/common/http/message_impl.h"
#include "source/common/stats/isolated_store_impl.h"
#include "source/extensions/common/wasm/wasm.h"
#include "source/extensions/filters/http/wasm/config.h"
#include "source/extensions/filters/http/wasm/wasm_filter.h"

#include "test/extensions/common/wasm/wasm_runtime.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/environment.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {

using Common::Wasm::WasmException;

namespace HttpFilters {
namespace Wasm {

class WasmFilterConfigTest : public Event::TestUsingSimulatedTime,
                             public testing::TestWithParam<std::tuple<std::string, std::string>> {
protected:
  WasmFilterConfigTest() : api_(Api::createApiForTest(stats_store_)) {
    ON_CALL(context_, api()).WillByDefault(ReturnRef(*api_));
    ON_CALL(context_, scope()).WillByDefault(ReturnRef(stats_scope_));
    ON_CALL(context_, listenerMetadata()).WillByDefault(ReturnRef(listener_metadata_));
    EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager_));
    ON_CALL(context_, clusterManager()).WillByDefault(ReturnRef(cluster_manager_));
    ON_CALL(context_, mainThreadDispatcher()).WillByDefault(ReturnRef(dispatcher_));
  }

  void SetUp() override { Envoy::Extensions::Common::Wasm::clearCodeCacheForTesting(); }

  void initializeForRemote() {
    retry_timer_ = new Event::MockTimer();

    EXPECT_CALL(dispatcher_, createTimer_(_))
        .WillOnce(Invoke([this](Event::TimerCb timer_cb) {
          retry_timer_cb_ = timer_cb;
          return retry_timer_;
        }))
        // The Host canary clone initializes its own runtime-stats timer after the remote retry
        // timer. Keep this fixture focused on retaining the retry callback while accepting those
        // worker-local timers.
        .WillRepeatedly(testing::ReturnNew<NiceMock<Event::MockTimer>>());
  }

  NiceMock<Server::Configuration::MockFactoryContext> context_;
  Stats::IsolatedStoreImpl stats_store_;
  Stats::Scope& stats_scope_{*stats_store_.rootScope()};
  Api::ApiPtr api_;
  envoy::config::core::v3::Metadata listener_metadata_;
  Init::ManagerImpl init_manager_{"init_manager"};
  NiceMock<Upstream::MockClusterManager> cluster_manager_;
  Init::ExpectableWatcherImpl init_watcher_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  Event::MockTimer* retry_timer_;
  Event::TimerCb retry_timer_cb_;
};

INSTANTIATE_TEST_SUITE_P(Runtimes, WasmFilterConfigTest,
                         Envoy::Extensions::Common::Wasm::sandbox_runtime_and_cpp_values,
                         Envoy::Extensions::Common::Wasm::wasmTestParamsToString);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(WasmFilterConfigTest);

TEST_P(WasmFilterConfigTest, JsonLoadFromFileWasm) {
  const std::string json =
      TestEnvironment::substitute(absl::StrCat(R"EOF(
  {
  "config" : {
  "vm_config": {
    "runtime": "envoy.wasm.runtime.)EOF",
                                               std::get<0>(GetParam()), R"EOF(",
    "configuration": {
       "@type": "type.googleapis.com/google.protobuf.StringValue",
       "value": "some configuration"
    },
    "code": {
      "local": {
        "filename": "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"
      }
    },
  }}}
  )EOF"));

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromJson(json, proto_config);
  WasmFilterConfig factory;
  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));
  cb(filter_callback);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromFileWasm) {
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      configuration:
         "@type": "type.googleapis.com/google.protobuf.StringValue"
         value: "some configuration"
      code:
        local:
          filename: "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"
  )EOF"));

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);

  // Intentionally we scope the factory here, and make the context outlive it.
  // This case happens when the config is updated by ECDS, and
  // we have to make sure that contexts still hold valid WasmVMs in these cases.
  std::shared_ptr<Envoy::Extensions::Common::Wasm::Context> context = nullptr;
  {
    WasmFilterConfig factory;
    Http::FilterFactoryCb cb =
        factory.createFilterFactoryFromProto(proto_config, "stats", context_);
    EXPECT_CALL(init_watcher_, ready());
    context_.initManager().initialize(init_watcher_);
    EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
    Http::MockFilterChainFactoryCallbacks filter_callback;
    EXPECT_CALL(filter_callback, addStreamFilter(_))
        .WillOnce([&context](Http::StreamFilterSharedPtr filter) {
          context = std::static_pointer_cast<Envoy::Extensions::Common::Wasm::Context>(filter);
        });
    EXPECT_CALL(filter_callback, addAccessLogHandler(_));
    cb(filter_callback);
  }
  // Check if the context still holds a valid Wasm even after the factory is destroyed.
  EXPECT_TRUE(context);
  EXPECT_TRUE(context->wasm());
  // Check if the custom stat namespace is registered during the initialization.
  EXPECT_TRUE(api_->customStatNamespaces().registered("wasmcustom"));
}

TEST_P(WasmFilterConfigTest, YamlLoadFromFileWasmFailOpenOk) {
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    fail_open: true
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      configuration:
         "@type": "type.googleapis.com/google.protobuf.StringValue"
         value: "some configuration"
      code:
        local:
          filename: "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"
  )EOF"));

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));
  cb(filter_callback);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromFileWasmInvalidConfig) {
  const std::string invalid_yaml =
      TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                               std::get<0>(GetParam()), R"EOF("
      configuration:
         "@type": "type.googleapis.com/google.protobuf.StringValue"
         value: "some configuration"
      code:
        local:
          filename: "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"
    configuration:
      "@type": "type.googleapis.com/google.protobuf.StringValue"
      value: "invalid"
  )EOF"));

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(invalid_yaml, proto_config);
  WasmFilterConfig factory;
  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");
  const std::string valid_yaml =
      TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                               std::get<0>(GetParam()), R"EOF("
      configuration:
         "@type": "type.googleapis.com/google.protobuf.StringValue"
         value: "some configuration"
      code:
        local:
          filename: "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"
    configuration:
      "@type": "type.googleapis.com/google.protobuf.StringValue"
      value: "valid"
  )EOF"));
  TestUtility::loadFromYaml(valid_yaml, proto_config);
  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));
  cb(filter_callback);
}

TEST_P(WasmFilterConfigTest, YamlLoadInlineWasm) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  EXPECT_FALSE(code.empty());
  const std::string yaml = absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                        std::get<0>(GetParam()), R"EOF("
      code:
        local: { inline_bytes: ")EOF",
                                        Base64::encode(code.data(), code.size()), R"EOF(" }
                                        )EOF");
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));
  cb(filter_callback);
}

TEST_P(WasmFilterConfigTest, YamlLoadInlineBadCode) {
  const std::string yaml = absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                        std::get<0>(GetParam()), R"EOF("
      code:
        local:
          inline_string: "bad code"
  )EOF");

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteWasm) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            Http::ResponseMessagePtr response(
                new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
            response->body().add(code);
            callbacks.onSuccess(request, std::move(response));
            return &request;
          }));

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));
  cb(filter_callback);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteWasmFailOnUncachedThenSucceed) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      nack_on_code_cache_miss: true
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            Http::ResponseMessagePtr response(
                new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
            response->body().add(code);
            callbacks.onSuccess(request, std::move(response));
            return &request;
          }));

  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");

  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  Init::ManagerImpl init_manager2{"init_manager2"};
  Init::ExpectableWatcherImpl init_watcher2;

  EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager2));

  auto cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);

  EXPECT_CALL(init_watcher2, ready());
  init_manager2.initialize(init_watcher2);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));

  cb(filter_callback);
  dispatcher_.clearDeferredDeleteList();
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteWasmFailCachedThenSucceed) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      nack_on_code_cache_miss: true
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          retry_policy:
            num_retries: 0
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillRepeatedly(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));

  Http::AsyncClient::Callbacks* async_callbacks = nullptr;
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillRepeatedly(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            // Store the callback the first time through for delayed call.
            if (!async_callbacks) {
              async_callbacks = &callbacks;
            } else {
              // Subsequent send()s happen inline.
              callbacks.onSuccess(
                  request,
                  Http::ResponseMessagePtr{new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                      new Http::TestResponseHeaderMapImpl{{":status", "503"}}})});
            }
            return &request;
          }));

  // Case 1: fail and fetch in the background, got 503, cache failure.
  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");
  // Fail a second time because we are in-progress.
  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");
  async_callbacks->onSuccess(
      request, Http::ResponseMessagePtr{new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                   new Http::TestResponseHeaderMapImpl{{":status", "503"}}})});

  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  // Case 2: fail immediately with negatively cached result.
  Init::ManagerImpl init_manager2{"init_manager2"};
  Init::ExpectableWatcherImpl init_watcher2;

  EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager2));
  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");

  EXPECT_CALL(init_watcher2, ready());
  init_manager2.initialize(init_watcher2);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  // Wait for negative cache to timeout.
  ::Envoy::Extensions::Common::Wasm::setTimeOffsetForCodeCacheForTesting(std::chrono::seconds(10));

  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillRepeatedly(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            Http::ResponseMessagePtr response(
                new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
            response->body().add(code);
            callbacks.onSuccess(request, std::move(response));
            return &request;
          }));

  // Case 3: fail and fetch in the background, got 200, cache success.
  Init::ManagerImpl init_manager3{"init_manager3"};
  Init::ExpectableWatcherImpl init_watcher3;

  EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager3));

  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");

  EXPECT_CALL(init_watcher3, ready());
  init_manager3.initialize(init_watcher3);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  // Case 4: success from cache.
  Init::ManagerImpl init_manager4{"init_manager4"};
  Init::ExpectableWatcherImpl init_watcher4;

  EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager4));

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);

  EXPECT_CALL(init_watcher4, ready());
  init_manager4.initialize(init_watcher4);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));

  cb(filter_callback);

  // Wait for cache to timeout.
  ::Envoy::Extensions::Common::Wasm::setTimeOffsetForCodeCacheForTesting(
      std::chrono::seconds(10 + 24 * 3600));

  // Case 5: flush the stale cache.
  const std::string sha256_2 = sha256 + "new";
  const std::string yaml2 =
      TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      nack_on_code_cache_miss: true
      runtime: "envoy.wasm.runtime.)EOF",
                                               std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          retry_policy:
            num_retries: 0
          sha256: )EOF",
                                               sha256_2));

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config2;
  TestUtility::loadFromYaml(yaml2, proto_config2);

  Init::ManagerImpl init_manager5{"init_manager4"};
  Init::ExpectableWatcherImpl init_watcher5;

  EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager5));

  EXPECT_THROW_WITH_MESSAGE(factory.createFilterFactoryFromProto(proto_config2, "stats", context_),
                            WasmException, "Unable to create Wasm HTTP filter ");

  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  // Case 6: fail and fetch in the background, got 200, cache success.
  Init::ManagerImpl init_manager6{"init_manager6"};
  Init::ExpectableWatcherImpl init_watcher6;

  EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager6));

  factory.createFilterFactoryFromProto(proto_config, "stats", context_);

  EXPECT_CALL(init_watcher6, ready());
  init_manager6.initialize(init_watcher6);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  // Case 7: success from cache.
  Init::ManagerImpl init_manager7{"init_manager7"};
  Init::ExpectableWatcherImpl init_watcher7;

  EXPECT_CALL(context_, initManager()).WillRepeatedly(ReturnRef(init_manager7));

  Http::FilterFactoryCb cb2 = factory.createFilterFactoryFromProto(proto_config, "stats", context_);

  EXPECT_CALL(init_watcher7, ready());
  init_manager7.initialize(init_watcher7);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);

  Http::MockFilterChainFactoryCallbacks filter_callback2;
  EXPECT_CALL(filter_callback2, addStreamFilter(_));
  EXPECT_CALL(filter_callback2, addAccessLogHandler(_));

  cb2(filter_callback2);

  dispatcher_.clearDeferredDeleteList();
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteConnectionReset) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          retry_policy:
            num_retries: 0
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            callbacks.onFailure(request, Envoy::Http::AsyncClient::FailureReason::Reset);
            return &request;
          }));

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteSuccessWith503) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          retry_policy:
            num_retries: 0
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            callbacks.onSuccess(
                request,
                Http::ResponseMessagePtr{new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "503"}}})});
            return &request;
          }));

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteSuccessIncorrectSha256) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          retry_policy:
            num_retries: 0
          sha256: xxxx )EOF"));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            Http::ResponseMessagePtr response(
                new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
            response->body().add(code);
            callbacks.onSuccess(request, std::move(response));
            return &request;
          }));

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteMultipleRetries) {
  initializeForRemote();
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          retry_policy:
            num_retries: 3
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);
  int num_retries = 3;
  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillRepeatedly(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .Times(num_retries)
      .WillRepeatedly(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            Http::ResponseMessagePtr response(
                new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "503"}}}));
            response->body().add(code);
            callbacks.onSuccess(request, std::move(response));
            return &request;
          }));

  EXPECT_CALL(*retry_timer_, enableTimer(_, _))
      .WillRepeatedly(Invoke([&](const std::chrono::milliseconds&, const ScopeTrackedObject*) {
        if (--num_retries == 0) {
          EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
              .WillOnce(Invoke(
                  [&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                      const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
                    Http::ResponseMessagePtr response(
                        new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                            new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
                    response->body().add(code);
                    callbacks.onSuccess(request, std::move(response));
                    return &request;
                  }));
        }

        retry_timer_cb_();
      }));
  EXPECT_CALL(*retry_timer_, disableTimer());

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  EXPECT_CALL(filter_callback, addStreamFilter(_));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));
  cb(filter_callback);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteSuccessBadcode) {
  const std::string code = "foo";
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            Http::ResponseMessagePtr response(
                new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
            response->body().add(code);
            callbacks.onSuccess(request, std::move(response));
            return nullptr;
          }));

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);

  // Fail closed.
  Http::MockFilterChainFactoryCallbacks filter_callback;
  Extensions::Common::Wasm::ContextSharedPtr context;
  EXPECT_CALL(filter_callback, addStreamFilter(_))
      .WillOnce(Invoke([&context](Http::StreamFilterSharedPtr filter) {
        context = std::static_pointer_cast<Extensions::Common::Wasm::Context>(filter);
      }));
  EXPECT_CALL(filter_callback, addAccessLogHandler(_));
  cb(filter_callback);
  EXPECT_EQ(context->wasm(), nullptr);
  EXPECT_TRUE(context->isFailed());

  Http::MockStreamDecoderFilterCallbacks decoder_callbacks;
  NiceMock<Envoy::StreamInfo::MockStreamInfo> stream_info;

  context->setDecoderFilterCallbacks(decoder_callbacks);
  EXPECT_CALL(decoder_callbacks, streamInfo()).WillRepeatedly(ReturnRef(stream_info));
  auto headers = Http::TestResponseHeaderMapImpl{{":status", "503"}};
  EXPECT_CALL(decoder_callbacks, encodeHeaders_(HeaderMapEqualRef(&headers), true));
  EXPECT_CALL(decoder_callbacks,
              sendLocalReply(Envoy::Http::Code::ServiceUnavailable, testing::Eq(""), _,
                             testing::Eq(Grpc::Status::WellKnownGrpcStatus::Unavailable),
                             testing::Eq("wasm_fail_stream")));
  EXPECT_EQ(context->onRequestHeaders(10, false),
            proxy_wasm::FilterHeadersStatus::StopAllIterationAndWatermark);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteSuccessBadcodeFailOpen) {
  const std::string code = "foo";
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    fail_open: true
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            Http::ResponseMessagePtr response(
                new Http::ResponseMessageImpl(Http::ResponseHeaderMapPtr{
                    new Http::TestResponseHeaderMapImpl{{":status", "200"}}}));
            response->body().add(code);
            callbacks.onSuccess(request, std::move(response));
            return nullptr;
          }));

  Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(proto_config, "stats", context_);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  Http::MockFilterChainFactoryCallbacks filter_callback;
  // The filter is not registered.
  cb(filter_callback);
}

TEST_P(WasmFilterConfigTest, YamlLoadFromRemoteWasmCreateFilter) {
  const std::string code = TestEnvironment::readFileToStringForTest(TestEnvironment::substitute(
      "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"));
  const std::string sha256 = Hex::encode(
      Envoy::Common::Crypto::UtilitySingleton::get().getSha256Digest(Buffer::OwnedImpl(code)));
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      code:
        remote:
          http_uri:
            uri: https://example.com/data
            cluster: cluster_1
            timeout: 5s
          sha256: )EOF",
                                                                    sha256));
  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  WasmFilterConfig factory;
  NiceMock<Http::MockAsyncClient> client;
  NiceMock<Http::MockAsyncClientRequest> request(&client);

  cluster_manager_.initializeThreadLocalClusters({"cluster_1"});
  EXPECT_CALL(cluster_manager_.thread_local_cluster_, httpAsyncClient())
      .WillOnce(ReturnRef(cluster_manager_.thread_local_cluster_.async_client_));
  Http::AsyncClient::Callbacks* async_callbacks = nullptr;
  EXPECT_CALL(cluster_manager_.thread_local_cluster_.async_client_, send_(_, _, _))
      .WillOnce(
          Invoke([&](Http::RequestMessagePtr&, Http::AsyncClient::Callbacks& callbacks,
                     const Http::AsyncClient::RequestOptions&) -> Http::AsyncClient::Request* {
            if (!async_callbacks) {
              async_callbacks = &callbacks;
            }
            return &request;
          }));
  NiceMock<Envoy::ThreadLocal::MockInstance> threadlocal;
  EXPECT_CALL(context_, threadLocal()).WillRepeatedly(ReturnRef(threadlocal));
  threadlocal.registered_ = false;
  auto filter_config = std::make_unique<FilterConfig>(proto_config, context_);
  EXPECT_EQ(filter_config->createFilter(), nullptr);
  EXPECT_CALL(init_watcher_, ready());
  context_.initManager().initialize(init_watcher_);
  auto response = Http::ResponseMessagePtr{new Http::ResponseMessageImpl(
      Http::ResponseHeaderMapPtr{new Http::TestResponseHeaderMapImpl{{":status", "200"}}})};
  response->body().add(code);
  async_callbacks->onSuccess(request, std::move(response));
  EXPECT_EQ(context_.initManager().state(), Init::Manager::State::Initialized);
  threadlocal.registered_ = true;
  EXPECT_NE(filter_config->createFilter(), nullptr);
}

#if defined(HIGRESS)
TEST_P(WasmFilterConfigTest, MainThreadTlsSlotUsesNeutralRoleOnHealthyPath) {
  NiceMock<Envoy::ThreadLocal::MockInstance> threadlocal;
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    name: main_role_test
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      configuration:
         "@type": "type.googleapis.com/google.protobuf.StringValue"
         value: "some configuration"
      code:
        local:
          filename: "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"
  )EOF"));

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
  EXPECT_CALL(context_, threadLocal()).WillOnce(ReturnRef(threadlocal));
  EXPECT_CALL(context_, mainThreadDispatcher()).WillRepeatedly(ReturnRef(threadlocal.dispatcher_));
  auto filter_config = std::make_unique<FilterConfig>(proto_config, context_);
  ASSERT_EQ(threadlocal.current_slot_, 1);
  auto wrapper = std::dynamic_pointer_cast<PluginHandleSharedPtrThreadLocal>(threadlocal.data_[0]);
  ASSERT_NE(wrapper, nullptr);
  EXPECT_FALSE(wrapper->participatesInWorkerInitialization());
  EXPECT_EQ(Envoy::Extensions::Common::Wasm::PluginInitializationState::Ready,
            wrapper->initializationState());
  EXPECT_NE(wrapper->handle(), nullptr);
  EXPECT_NE(wrapper->handle()->wasmHandle(), nullptr);

  const std::string stats_prefix =
      absl::StrCat("wasm.", proto_config.config().vm_config().runtime(), ".plugin.main_role_test.");
  EXPECT_EQ(
      0,
      stats_scope_.counterFromString(stats_prefix + "worker_init_retryable_failure_total").value());
  EXPECT_EQ(
      0,
      stats_scope_.counterFromString(stats_prefix + "worker_init_terminal_failure_total").value());
  EXPECT_EQ(0, stats_scope_
                   .gaugeFromString(stats_prefix + "worker_uninitialized",
                                    Stats::Gauge::ImportMode::NeverImport)
                   .value());
}
#endif

TEST_P(WasmFilterConfigTest, FailedToGetThreadLocalPlugin) {
  NiceMock<Envoy::ThreadLocal::MockInstance> threadlocal;
  const std::string yaml = TestEnvironment::substitute(absl::StrCat(R"EOF(
  config:
    fail_open: true
    vm_config:
      runtime: "envoy.wasm.runtime.)EOF",
                                                                    std::get<0>(GetParam()), R"EOF("
      configuration:
         "@type": "type.googleapis.com/google.protobuf.StringValue"
         value: "some configuration"
      code:
        local:
          filename: "{{ test_rundir }}/test/extensions/filters/http/wasm/test_data/test_cpp.wasm"
  )EOF"));

  envoy::extensions::filters::http::wasm::v3::Wasm proto_config;
  TestUtility::loadFromYaml(yaml, proto_config);
#if defined(HIGRESS)
  proto_config.mutable_config()->set_name("worker_init_test");
#endif
  EXPECT_CALL(context_, threadLocal()).WillOnce(ReturnRef(threadlocal));
  threadlocal.registered_ = true;
  auto filter_config = std::make_unique<FilterConfig>(proto_config, context_);
  ASSERT_EQ(threadlocal.current_slot_, 1);
  ASSERT_NE(filter_config->createFilter(), nullptr);
#if defined(HIGRESS)
  auto ready_wrapper =
      std::dynamic_pointer_cast<PluginHandleSharedPtrThreadLocal>(threadlocal.data_[0]);
  ASSERT_NE(ready_wrapper, nullptr);
  EXPECT_TRUE(ready_wrapper->participatesInWorkerInitialization());
  const auto recovered_handle = ready_wrapper->handle();
  ASSERT_NE(recovered_handle, nullptr);
#endif

  // If the thread local plugin handle returns nullptr, `createFilter` should return nullptr
  threadlocal.data_[0] = std::make_shared<PluginHandleSharedPtrThreadLocal>(nullptr);
  EXPECT_EQ(filter_config->createFilter(), nullptr);

#if defined(HIGRESS)
  auto worker_scope = Stats::ScopeSharedPtr(stats_store_.createScope(""));
  auto worker_stats = std::make_shared<Envoy::Extensions::Common::Wasm::WorkerInitStatsHandler>(
      worker_scope, proto_config.config().vm_config().runtime(), proto_config.config().name());
  auto fail_open_plugin = std::make_shared<Envoy::Extensions::Common::Wasm::Plugin>(
      proto_config.config(), context_.direction(), context_.localInfo(),
      &context_.listenerMetadata());
  const std::string stats_prefix = absl::StrCat(
      "wasm.", proto_config.config().vm_config().runtime(), ".plugin.worker_init_test.");
  auto& fail_open_skip =
      worker_scope->counterFromString(stats_prefix + "worker_fail_open_skip_total");
  auto& retry_total = worker_scope->counterFromString(stats_prefix + "worker_init_retry_total");
  auto& recovered_total =
      worker_scope->counterFromString(stats_prefix + "worker_init_recovered_total");
  auto& uninitialized = worker_scope->gaugeFromString(stats_prefix + "worker_uninitialized",
                                                      Stats::Gauge::ImportMode::NeverImport);

  uint64_t factory_calls = 0;
  auto retryable_wrapper = std::make_shared<PluginHandleSharedPtrThreadLocal>(
      Envoy::Extensions::Common::Wasm::ThreadLocalPluginResult{nullptr,
                                                               proxy_wasm::FailState::RuntimeError},
      fail_open_plugin, nullptr, dispatcher_, worker_stats, false,
      [&factory_calls, &recovered_handle]() {
        ++factory_calls;
        if (factory_calls == 1) {
          return Envoy::Extensions::Common::Wasm::ThreadLocalPluginResult{
              nullptr, proxy_wasm::FailState::RuntimeError};
        }
        return Envoy::Extensions::Common::Wasm::ThreadLocalPluginResult{recovered_handle,
                                                                        proxy_wasm::FailState::Ok};
      });
  retryable_wrapper->setRecoveryVmKeyForTesting(
      std::string(recovered_handle->wasmHandle()->wasm()->vm_key()));
  threadlocal.data_[0] = retryable_wrapper;
  EXPECT_EQ(filter_config->createFilter(), nullptr);
  EXPECT_EQ(1, factory_calls);
  EXPECT_EQ(1, retry_total.value());
  EXPECT_EQ(1, fail_open_skip.value());
  EXPECT_EQ(1, uninitialized.value());

  EXPECT_EQ(filter_config->createFilter(), nullptr);
  EXPECT_EQ(1, factory_calls);
  EXPECT_EQ(1, retry_total.value());
  EXPECT_EQ(2, fail_open_skip.value());

  Envoy::Extensions::Common::Wasm::setTimeOffsetForCodeCacheForTesting(std::chrono::seconds(1));
  auto recovered_filter = filter_config->createFilter();
  EXPECT_NE(recovered_filter, nullptr);
  EXPECT_EQ(2, factory_calls);
  EXPECT_EQ(2, retry_total.value());
  EXPECT_EQ(1, recovered_total.value());
  EXPECT_EQ(0, uninitialized.value());
  EXPECT_FALSE(retryable_wrapper->rebuild(true));

  threadlocal.data_[0] = std::make_shared<PluginHandleSharedPtrThreadLocal>(
      Envoy::Extensions::Common::Wasm::ThreadLocalPluginResult{
          nullptr, proxy_wasm::FailState::ConfigureFailed},
      fail_open_plugin, nullptr, dispatcher_, worker_stats);
  EXPECT_EQ(filter_config->createFilter(), nullptr);
  EXPECT_EQ(3, fail_open_skip.value());
  EXPECT_EQ(1, uninitialized.value());

  auto fail_closed_config = proto_config.config();
  fail_closed_config.set_fail_open(false);
  auto fail_closed_plugin = std::make_shared<Envoy::Extensions::Common::Wasm::Plugin>(
      fail_closed_config, context_.direction(), context_.localInfo(), &context_.listenerMetadata());
  uint64_t fail_closed_factory_calls = 0;
  auto fail_closed_retryable = std::make_shared<PluginHandleSharedPtrThreadLocal>(
      Envoy::Extensions::Common::Wasm::ThreadLocalPluginResult{nullptr,
                                                               proxy_wasm::FailState::RuntimeError},
      fail_closed_plugin, nullptr, dispatcher_, worker_stats, false,
      [&fail_closed_factory_calls]() {
        ++fail_closed_factory_calls;
        return Envoy::Extensions::Common::Wasm::ThreadLocalPluginResult{
            nullptr, proxy_wasm::FailState::RuntimeError};
      });
  fail_closed_retryable->setRecoveryVmKeyForTesting("config-fail-closed-recovery");
  threadlocal.data_[0] = fail_closed_retryable;
  EXPECT_NE(filter_config->createFilter(), nullptr);
  EXPECT_EQ(1, fail_closed_factory_calls);
  EXPECT_EQ(3, retry_total.value());
  EXPECT_EQ(1, uninitialized.value());

  threadlocal.data_[0] = std::make_shared<PluginHandleSharedPtrThreadLocal>(
      Envoy::Extensions::Common::Wasm::ThreadLocalPluginResult{
          nullptr, proxy_wasm::FailState::ConfigureFailed},
      fail_closed_plugin, nullptr, dispatcher_, worker_stats);
  auto fail_closed_filter = filter_config->createFilter();
  ASSERT_NE(fail_closed_filter, nullptr);
  Http::MockStreamDecoderFilterCallbacks decoder_callbacks;
  fail_closed_filter->setDecoderFilterCallbacks(decoder_callbacks);
  EXPECT_CALL(decoder_callbacks,
              sendLocalReply(Envoy::Http::Code::ServiceUnavailable, testing::Eq(""), _,
                             testing::Eq(Grpc::Status::WellKnownGrpcStatus::Unavailable),
                             testing::Eq("wasm_fail_stream")))
      .WillOnce(testing::InvokeWithoutArgs([&fail_closed_filter]() {
        Http::TestResponseHeaderMapImpl response_headers{{":status", "503"}};
        EXPECT_EQ(Http::FilterHeadersStatus::Continue,
                  fail_closed_filter->encodeHeaders(response_headers, true));
      }));
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/"}, {":scheme", "http"}, {":authority", "host"}};
  EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
            fail_closed_filter->decodeHeaders(request_headers, true));
  EXPECT_EQ(3, fail_open_skip.value());
  EXPECT_EQ(2, uninitialized.value());

  fail_closed_retryable.reset();
  EXPECT_EQ(1, uninitialized.value());

  threadlocal.data_[0] = std::make_shared<PluginHandleSharedPtrThreadLocal>(nullptr);
  EXPECT_EQ(0, uninitialized.value());
#endif
}

} // namespace Wasm
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
