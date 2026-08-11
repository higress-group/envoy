#pragma once

#include <memory>

#include "envoy/extensions/filters/http/wasm/v3/wasm.pb.validate.h"
#include "envoy/http/filter.h"
#include "envoy/server/filter_config.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/extensions/common/wasm/plugin.h"
#include "source/extensions/common/wasm/wasm.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Wasm {

using Envoy::Extensions::Common::Wasm::Context;
using Envoy::Extensions::Common::Wasm::PluginHandleSharedPtr;
using Envoy::Extensions::Common::Wasm::PluginHandleSharedPtrThreadLocal;
using Envoy::Extensions::Common::Wasm::PluginSharedPtr;
using Envoy::Extensions::Common::Wasm::Wasm;

#if defined(HIGRESS)
class UninitializedFailClosedContext final : public Context {
public:
  using Context::Context;

  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap&, bool) override {
    failStream(proxy_wasm::WasmStreamType::Request);
    return Http::FilterHeadersStatus::StopIteration;
  }

  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap&, bool) override {
    return Http::FilterHeadersStatus::Continue;
  }
};
#endif

class FilterConfig : Logger::Loggable<Logger::Id::wasm> {
public:
  FilterConfig(const envoy::extensions::filters::http::wasm::v3::Wasm& config,
               Server::Configuration::FactoryContext& context);

  std::shared_ptr<Context> createFilter() {
    Wasm* wasm = nullptr;
    if (!tls_slot_->currentThreadRegistered()) {
      return nullptr;
    }
    auto opt_ref = tls_slot_->get();
    if (!opt_ref) {
      return nullptr;
    }
#if defined(HIGRESS)
    PluginHandleSharedPtr handle = opt_ref->handle();
    if (opt_ref->initializationState() ==
        Envoy::Extensions::Common::Wasm::PluginInitializationState::Uninitialized) {
      const auto recovery = opt_ref->tryInitialize();
      if (recovery.status ==
          Envoy::Extensions::Common::Wasm::PluginInitializationRecoveryStatus::Recovered) {
        handle = opt_ref->handle();
      } else if (handle != nullptr && handle->plugin()->fail_open_) {
        opt_ref->recordFailOpenSkip();
        return nullptr;
      } else if (handle != nullptr) {
        return std::make_shared<UninitializedFailClosedContext>(nullptr, 0, handle);
      }
    }
#endif
#if !defined(HIGRESS)
    PluginHandleSharedPtr handle = opt_ref->handle();
#endif
    if (!handle) {
      return nullptr;
    }
    if (handle->wasmHandle()) {
      wasm = handle->wasmHandle()->wasm().get();
    }
#if defined(HIGRESS)
    auto failed = false;
    if (!wasm) {
      failed = true;
    } else if (wasm->isFailed()) {
      ENVOY_LOG(info, "wasm vm is crashed, try to recover");
      if (opt_ref->rebuild(true)) {
        ENVOY_LOG(info, "wasm vm recover success");
        wasm = opt_ref->handle()->wasmHandle()->wasm().get();
        handle = opt_ref->handle();
      } else {
        ENVOY_LOG(info, "wasm vm recover failed");
        failed = true;
      }
    }
    if (failed) {
      if (handle->plugin()->fail_open_) {
        return nullptr; // Fail open skips adding this filter to callbacks.
      } else {
        return std::make_shared<Context>(nullptr, 0,
                                         handle); // Fail closed is handled by an empty Context.
      }
    }
#else
    if (!wasm || wasm->isFailed()) {
      if (handle->plugin()->fail_open_) {
        return nullptr; // Fail open skips adding this filter to callbacks.
      } else {
        return std::make_shared<Context>(nullptr, 0,
                                         handle); // Fail closed is handled by an empty Context.
      }
    }
#endif
    return std::make_shared<Context>(wasm, handle->rootContextId(), handle);
  }

private:
  ThreadLocal::TypedSlotPtr<PluginHandleSharedPtrThreadLocal> tls_slot_;
  Config::DataSource::RemoteAsyncDataProviderPtr remote_data_provider_;
#if defined(HIGRESS)
  Envoy::Extensions::Common::Wasm::WasmHandleSharedPtr base_wasm_handle_;
#endif
};

using FilterConfigSharedPtr = std::shared_ptr<FilterConfig>;

} // namespace Wasm
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
