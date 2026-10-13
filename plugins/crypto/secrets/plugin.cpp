module;

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.api.core.registry;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.config.core.component;
import forge.config.core.decode;
import forge.crypto.core.secret_bytes;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::crypto::secrets {

plugin::plugin() : impl_{std::make_shared<impl>(lifecycle_handle())} {}

plugin::~plugin() = default;

forge::app::plugin_descriptor plugin::descriptor() {
   return forge::app::plugin_descriptor{
      .id = {.value = "forge.plugins.crypto.secrets"},
      .factory = [] { return std::make_unique<plugin>(); },
   };
}

forge::app::plugin_id plugin::id() const {
   return {.value = "forge.plugins.crypto.secrets"};
}

std::string plugin::version() const {
   return "1.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.crypto.secrets");
}

boost::asio::awaitable<void> plugin::on_configure(forge::config::core::component_view view) {
   apply_config(*impl_, view);
   co_return;
}

boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::on_initialize(forge::app::plugin_context&) {
   co_return;
}

boost::asio::awaitable<void> plugin::on_startup() {
   co_return;
}

void plugin::on_request_stop() noexcept {}

boost::asio::awaitable<void> plugin::on_shutdown() {
   co_return;
}

} // namespace forge::plugins::crypto::secrets
