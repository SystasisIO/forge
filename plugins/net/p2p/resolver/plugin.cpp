module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <exception>
#include <list>
#include <map>
#include <memory>
#include <stop_token>
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
import forge.asio.notification;
import forge.asio.task;
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
#include "details/publication_catalog.hxx"
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"
#include "details/managed_api_impl.hxx"

namespace forge::plugins::net::p2p::resolver {

plugin::plugin() : impl_{std::make_shared<impl>(lifecycle_handle())} {}
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

boost::asio::awaitable<void> plugin::on_configure(forge::config::core::component_view view) {
   auto config = decode_config(view);
   validate_config(config);
   impl_->settings = std::move(config);
   impl_->protocol = forge::net::p2p::protocol_id{.value = impl_->settings.protocol_id};
   co_return;
}

boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   provider.install<managed_api>(std::make_shared<managed_api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::on_initialize(forge::app::plugin_context& context) {
   auto p2p =
       context.apis()
           .get<forge::plugins::net::p2p::node::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 3, .min_revision = 0})
           .shared();
   auto publications = detail::make_publication_catalog(context.scheduler());
   const auto generation = impl_->lifecycle.snapshot().generation;
   if (!impl_->lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{impl_->mutex};
      impl_->p2p = p2p;
      impl_->local_publications = std::move(publications);
   })) {
      FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "P2P API resolver initialization was stopped");
   }

   try {
      impl_->install_protocol(p2p);
   } catch (const forge::plugins::net::p2p::node::exceptions::route_conflict& error) {
      impl_->protocol_registry.clear();
      auto lock = std::scoped_lock{impl_->mutex};
      impl_->p2p = nullptr;
      FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "P2P API resolver protocol conflicts with an existing route",
                            forge::exceptions::ctx("protocol", impl_->protocol.value),
                            forge::exceptions::ctx("error", error.message()));
   } catch (...) {
      impl_->protocol_registry.clear();
      auto lock = std::scoped_lock{impl_->mutex};
      impl_->p2p = nullptr;
      throw;
   }

   co_return;
}

boost::asio::awaitable<void> plugin::on_startup() {
   co_return;
}

void plugin::on_request_stop() noexcept {
   impl_->request_stop_managed();
   impl_->request_stop_publications();
}

boost::asio::awaitable<void> plugin::on_shutdown() {
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

   auto retired_p2p = std::shared_ptr<forge::plugins::net::p2p::node::api>{};
   if (!failure) {
      auto lock = std::scoped_lock{impl_->mutex};
      retired_p2p.swap(impl_->p2p);
      impl_->cache.clear();
   }

   if (failure) {
      std::rethrow_exception(failure);
   }
   co_return;
}

forge::app::plugin_descriptor plugin::descriptor() {
   return forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.net.p2p.resolver"},
       .dependencies = {forge::app::plugin_id{.value = "forge.plugins.net.p2p.node"}},
       .factory = [] { return std::make_unique<plugin>(); },
   };
}

} // namespace forge::plugins::net::p2p::resolver
