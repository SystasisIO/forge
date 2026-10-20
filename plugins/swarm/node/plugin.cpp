module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.api;
import forge.plugins.swarm.node.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.db.store.api;
import forge.api.p2p.publication;
import forge.config.core.decode;
import forge.config.core.document;
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::swarm::node {

plugin::plugin(plugin_options options) : _impl{std::make_shared<impl>(lifecycle_handle(), std::move(options))} {}
plugin::~plugin() = default;
forge::app::plugin_id plugin::id() const { return {.value = "forge.plugins.swarm.node"}; }
std::string plugin::version() const { return "1.0.0"; }
std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   auto result = forge::config::core::describe_component<config>("plugins.swarm.node");
   const auto defaults = forge::config::core::encode(_impl->options.initial_config, result.section);
   for (auto& field : result.fields) {
      if (const auto* value = defaults.try_get(result.section + "." + field.name)) {
         field.has_default = true;
         field.default_value = *value;
      }
   }
   return result;
}
forge::app::plugin_descriptor plugin::descriptor(plugin_options options) {
   return {.id = {.value = "forge.plugins.swarm.node"},
      .dependencies = {{.value = "forge.plugins.net.p2p.node"}, {.value = "forge.plugins.db.store"}},
      .factory = [options = std::move(options)] { return std::make_unique<plugin>(options); }};
}

boost::asio::awaitable<void> plugin::on_configure(forge::config::core::component_view view) {
   auto document = forge::config::core::merge({forge::config::core::encode(_impl->options.initial_config, view.section()), view.source()});
   auto decoded = forge::config::core::decode<config>(document, view.section());
   if (!decoded.ok()) FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Invalid Swarm configuration");
   validate_config(decoded.value);
   if (decoded.value.free_admission && _impl->options.admission)
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm free configuration conflicts with injected admission");
   const auto lock = std::scoped_lock{_impl->mutex};
   _impl->settings = std::move(decoded.value);
   co_return;
}
boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(_impl));
   co_return;
}
boost::asio::awaitable<void> plugin::on_initialize(forge::app::plugin_context& context) {
   auto cpu = _impl->options.cpu;
   if (!cpu && context.has_compute()) cpu = context.compute();
   if (!cpu || !cpu->valid()) FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm requires caller-owned CPU execution");
   if (_impl->settings.metadata.mode == "object" && !_impl->options.database)
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm named metadata requires an explicit DB executor");
   auto p2p = context.apis().get<forge::plugins::net::p2p::node::api>(
      {.id = {.value = "forge.plugins.net.p2p.node"}, .major = 3}).shared();
   auto dht = context.apis().get<forge::plugins::net::p2p::node::dht_api>(
      {.id = {.value = "forge.plugins.net.p2p.node.dht"}, .major = 1}).shared();
   auto database = std::shared_ptr<forge::plugins::db::store::api>{};
   if (_impl->settings.metadata.mode == "object")
      database = context.apis().get<forge::plugins::db::store::api>(
         {.id = {.value = "forge.plugins.db.store"}, .major = 2}).shared();
   const auto generation = _impl->lifecycle.snapshot().generation;
   if (!_impl->lifecycle.try_admit(generation, [&] {
         const auto lock = std::scoped_lock{_impl->mutex};
         _impl->scheduler = &context.scheduler(); _impl->cpu = *cpu;
         _impl->p2p = std::move(p2p); _impl->dht = std::move(dht); _impl->database = std::move(database);
      })) FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm initialization stopped");
   co_return;
}
boost::asio::awaitable<void> plugin::on_after_initialize() {
   if (_impl->settings.metadata.mode == "object") {
      auto named = co_await _impl->database->store(_impl->settings.metadata.object_name);
      auto objects = named.objects();
      forge::swarm::storage::catalog::register_objects(objects);
      const auto generation = _impl->lifecycle.snapshot().generation;
      if (!_impl->lifecycle.try_admit(generation, [&] {
            const auto lock = std::scoped_lock{_impl->mutex}; _impl->objects = std::move(objects);
         })) FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm metadata registration stopped");
   }
   co_return;
}
boost::asio::awaitable<void> plugin::on_startup() { return impl::start(_impl); }
void plugin::on_request_stop() noexcept { _impl->request_stop(); }
boost::asio::awaitable<void> plugin::on_shutdown() { return impl::close(_impl); }

} // namespace forge::plugins::swarm::node
