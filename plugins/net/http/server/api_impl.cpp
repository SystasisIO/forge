module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <memory>
#include <utility>

module forge.plugins.net.http.server.plugin;

import forge.api.core.registry;
import forge.asio.compute;
import forge.asio.runtime;
import forge.api.http.binding;
import forge.net.http.assets;
import forge.net.http.server;
import forge.net.tls.context;
import forge.plugins.net.http.server.api;
import forge.plugins.net.http.server.exceptions;
import forge.plugins.net.http.server.middleware;
import forge.plugins.net.http.server.types;
import forge.plugins.crypto.secrets.api;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::http::server {

plugin::api_impl::api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

const forge::api::core::registry& plugin::api_impl::registry() const {
   const auto generation = impl_->lifecycle.snapshot().generation;
   auto apis = static_cast<const forge::api::core::registry*>(nullptr);
   if (!impl_->lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{impl_->mutex};
      apis = impl_->apis;
   })) {
      FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
   }
   if (apis == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP server plugin is not initialized");
   }
   return *apis;
}

boost::asio::awaitable<void> plugin::api_impl::publish(std::unique_ptr<binding_spec> binding, publish_options options) {
   impl_->add(pending_binding{.binding = binding->build(registry()), .options = std::move(options)});
   co_return;
}

boost::asio::awaitable<void> plugin::api_impl::use(middleware_descriptor descriptor) {
   impl_->add(std::move(descriptor));
   co_return;
}

boost::asio::awaitable<void> plugin::api_impl::mount_assets(forge::net::http::asset_mount value) {
   impl_->add(std::move(value));
   co_return;
}

boost::asio::awaitable<void> plugin::api_impl::mount_routes(route_mount value) {
   impl_->add(std::move(value));
   co_return;
}

boost::asio::awaitable<void> plugin::api_impl::reload_tls() {
   co_await impl_->reload_tls();
}

} // namespace forge::plugins::net::http::server
