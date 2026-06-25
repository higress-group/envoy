#include "source/extensions/filters/http/wasm/wasm_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Wasm {

FilterConfig::FilterConfig(const envoy::extensions::filters::http::wasm::v3::Wasm& config,
                           Server::Configuration::FactoryContext& context)
    : tls_slot_(ThreadLocal::TypedSlot<Common::Wasm::PluginHandleSharedPtrThreadLocal>::makeUnique(
          context.threadLocal())) {
  const auto plugin = std::make_shared<Common::Wasm::Plugin>(
      config.config(), context.direction(), context.localInfo(), &context.listenerMetadata());

  auto callback = [plugin, this](const Common::Wasm::WasmHandleSharedPtr& base_wasm) {
#if defined(HIGRESS)
    base_wasm_handle_ = base_wasm;
#endif
    // NB: the Slot set() call doesn't complete inline, so all arguments must outlive this call.
    tls_slot_->set([base_wasm, plugin](Event::Dispatcher& dispatcher) {
#if defined(HIGRESS)
      return std::make_shared<PluginHandleSharedPtrThreadLocal>(
          Common::Wasm::getOrCreateThreadLocalPlugin(base_wasm, plugin, dispatcher), base_wasm,
          true);
#else
      return std::make_shared<PluginHandleSharedPtrThreadLocal>(
          Common::Wasm::getOrCreateThreadLocalPlugin(base_wasm, plugin, dispatcher));
#endif
    });
  };

  bool created = false;
#if defined(HIGRESS)
  created = Common::Wasm::createWasm(
      plugin, context.scope().createScope(""), context.clusterManager(), context.initManager(),
      context.mainThreadDispatcher(), context.api(), context.lifecycleNotifier(),
      remote_data_provider_, std::move(callback), nullptr, &context.runtime());
#else
  created = Common::Wasm::createWasm(
      plugin, context.scope().createScope(""), context.clusterManager(), context.initManager(),
      context.mainThreadDispatcher(), context.api(), context.lifecycleNotifier(),
      remote_data_provider_, std::move(callback));
#endif
  if (!created) {
    throw Common::Wasm::WasmException(
        fmt::format("Unable to create Wasm HTTP filter {}", plugin->name_));
  }
}

} // namespace Wasm
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
