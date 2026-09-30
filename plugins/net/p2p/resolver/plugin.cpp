module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.p2p.publication;
import forge.api.transport.options;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.resolver.api;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.managed_api;
import forge.plugins.net.p2p.resolver.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.exceptions;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"
#include "details/managed_api_impl.hxx"

namespace forge::plugins::net::p2p::resolver {

plugin::plugin() : impl_{std::make_shared<impl>()} {}
plugin::~plugin() = default;

forge::net::p2p::protocol_id default_protocol() {
   return forge::net::p2p::protocol_id{.value = "/forge/api/resolver/2"};
}

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.net.p2p.resolver"};
}

std::string plugin::version() const {
   return "2.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.net.p2p.resolver");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   auto config = decode_config(view);
   validate_config(config);
   impl_->settings = std::move(config);
   impl_->protocol = forge::net::p2p::protocol_id{.value = impl_->settings.protocol_id};
   co_return;
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   provider.install<managed_api>(std::make_shared<managed_api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   auto p2p =
       context.apis()
           .get<forge::plugins::net::p2p::node::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 2, .min_revision = 0})
           .shared();
   auto publications = detail::make_publication_catalog(context.scheduler());
   {
      auto lock = std::scoped_lock{impl_->mutex};
      impl_->p2p = p2p;
      impl_->local_publications = std::move(publications);
      impl_->initialized = false;
      impl_->stopping = false;
   }

   try {
      impl_->install_protocol(p2p);
   } catch (const forge::plugins::net::p2p::node::exceptions::route_conflict& error) {
      auto lock = std::scoped_lock{impl_->mutex};
      impl_->protocol_registry.clear();
      impl_->p2p = nullptr;
      impl_->initialized = false;
      FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "P2P API resolver protocol conflicts with an existing route",
                            forge::exceptions::ctx("protocol", impl_->protocol.value),
                            forge::exceptions::ctx("error", error.message()));
   } catch (...) {
      auto lock = std::scoped_lock{impl_->mutex};
      impl_->protocol_registry.clear();
      impl_->p2p = nullptr;
      impl_->initialized = false;
      throw;
   }

   {
      auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->stopping) {
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P API resolver plugin is stopping");
      }
      impl_->initialized = true;
   }
   co_return;
}

boost::asio::awaitable<void> plugin::startup() {
   co_return;
}

void plugin::request_stop() noexcept {
   impl_->request_stop_managed();
   impl_->request_stop_publications();
}

boost::asio::awaitable<void> plugin::shutdown() {
   request_stop();

   auto failure = std::exception_ptr{};
   try {
      co_await impl_->shutdown_publications();
   } catch (...) {
      failure = std::current_exception();
   }

   try {
      co_await impl_->shutdown_managed();
   } catch (...) {
      if (!failure) {
         failure = std::current_exception();
      }
   }

   {
      auto lock = std::scoped_lock{impl_->mutex};
      impl_->initialized = false;
      impl_->p2p = nullptr;
      impl_->cache.clear();
   }

   if (failure) {
      std::rethrow_exception(failure);
   }
   co_return;
}

forge::app::plugin_descriptor descriptor() {
   return forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.net.p2p.resolver"},
       .dependencies = {forge::app::plugin_id{.value = "forge.plugins.net.p2p.node"}},
       .factory = [] { return std::make_unique<plugin>(); },
   };
}

} // namespace forge::plugins::net::p2p::resolver
