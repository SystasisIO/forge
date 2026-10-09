module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

module forge.plugins.net.http.server.plugin;

import forge.api.core.registry;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.asio.compute;
import forge.asio.runtime;
import forge.config.core.component;
import forge.config.core.decode;
import forge.api.http.binding;
import forge.net.http.assets;
import forge.net.http.middleware;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.types;
import forge.net.tls.context;
import forge.plugins.net.http.server.api;
import forge.plugins.net.http.server.exceptions;
import forge.plugins.net.http.server.middleware;
import forge.plugins.net.http.server.types;
import forge.plugins.crypto.secrets.api;

#include "details/api_impl.hxx"
#include "details/config.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::http::server {
namespace {

namespace http = forge::net::http;

[[nodiscard]] http::middleware_phase to_http_phase(middleware_phase value) noexcept {
   switch (value) {
   case middleware_phase::request_context:
      return http::middleware_phase::request_context;
   case middleware_phase::security:
      return http::middleware_phase::security;
   case middleware_phase::limits:
      return http::middleware_phase::limits;
   case middleware_phase::before_handler:
      return http::middleware_phase::before_handler;
   case middleware_phase::after_handler:
      return http::middleware_phase::after_handler;
   case middleware_phase::error:
      return http::middleware_phase::error;
   }
   return http::middleware_phase::before_handler;
}

[[nodiscard]] std::string_view method_text(http::method value) noexcept {
   switch (value) {
   case http::method::delete_:
      return "DELETE";
   case http::method::get:
      return "GET";
   case http::method::head:
      return "HEAD";
   case http::method::options:
      return "OPTIONS";
   case http::method::patch:
      return "PATCH";
   case http::method::post:
      return "POST";
   case http::method::put:
      return "PUT";
   case http::method::unknown:
      return "UNKNOWN";
   }
   return "UNKNOWN";
}

[[nodiscard]] middleware_request make_request(const forge::net::http::route_context& context) {
   auto headers = std::vector<header_entry>{};
   for (const auto& header : context.request.headers()) {
      headers.push_back(header_entry{.name = header.name, .value = header.text});
   }
   return middleware_request{
       .method = std::string{method_text(context.request.method())},
       .target = std::string{context.request.target()},
       .path = context.parsed_target.path,
       .headers = std::move(headers),
   };
}

[[nodiscard]] middleware_response make_response(forge::net::http::response value) {
   auto result = middleware_response{};
   auto stream_state = forge::net::http::capture_stream_pass_through(value);
   detail::middleware_bridge_access::set_status(result, value.result());
   detail::middleware_bridge_access::set_body(result, std::move(value.body()));
   for (const auto& header : value.headers()) {
      if (http::header_name_equal(header.name, http::field_name(http::field::content_type))) {
         detail::middleware_bridge_access::set_content_type(result, header.text);
         continue;
      }
      if (http::header_name_equal(header.name, http::field_name(http::field::content_length)) ||
          http::header_name_equal(header.name, http::field_name(http::field::transfer_encoding))) {
         continue;
      }
      detail::middleware_bridge_access::headers(result).push_back(
          header_entry{.name = header.name, .value = header.text});
   }
   detail::middleware_bridge_access::set_stream_state(result, std::move(stream_state));
   return result;
}

[[nodiscard]] forge::net::http::response make_http_response(const forge::net::http::request& source,
                                                            middleware_response value) {
   auto result = forge::net::http::response{value.status(), source.version()};
   if (const auto& content_type = detail::middleware_bridge_access::content_type(value);
       content_type.has_value() && !content_type->empty()) {
      result.set(forge::net::http::field::content_type, *content_type);
   }
   result.body() = detail::middleware_bridge_access::take_body(value);
   for (const auto& header : value.headers()) {
      if (http::header_name_equal(header.name, "Content-Length") ||
          http::header_name_equal(header.name, "Transfer-Encoding")) {
         continue;
      }
      if (http::header_name_equal(header.name, "Set-Cookie")) {
         result.insert(std::string_view{header.name}, std::string_view{header.value});
      } else {
         result.set(std::string_view{header.name}, std::string_view{header.value});
      }
   }
   if (!result.body().empty()) {
      forge::net::http::clear_stream_pass_through(result);
   } else {
      forge::net::http::restore_stream_pass_through(result, detail::middleware_bridge_access::stream_state(value));
   }
   result.prepare_payload();
   result.keep_alive(source.keep_alive());
   return result;
}

[[nodiscard]] forge::net::http::middleware_descriptor to_http_middleware(middleware_descriptor descriptor) {
   return forge::net::http::middleware_descriptor{
       .id = std::move(descriptor.id),
       .phase = to_http_phase(descriptor.phase),
       .order = descriptor.order,
       .path_prefix = std::move(descriptor.path_prefix),
       .handler = [descriptor = std::move(descriptor)](
                      forge::net::http::route_context& context,
                      forge::net::http::next_handler next) -> boost::asio::awaitable<forge::net::http::response> {
          if (!descriptor.handler) {
             co_return co_await next();
          }
          auto request = make_request(context);
          auto response = co_await descriptor.handler(
              request, [next = std::move(next)]() mutable -> boost::asio::awaitable<middleware_response> {
                 auto raw = co_await next();
                 co_return make_response(std::move(raw));
              });
          co_return make_http_response(context.request, std::move(response));
       },
   };
}

} // namespace

plugin::plugin() : impl_{std::make_shared<impl>()} {}

plugin::~plugin() = default;

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.net.http.server"};
}

std::string plugin::version() const {
   return "2.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.net.http.server");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   auto settings = decode_config(view);
   const auto lock = std::scoped_lock{impl_->mutex};
   impl_->settings = std::move(settings);
   co_return;
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   auto requires_tls_secrets = false;
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      requires_tls_secrets = impl_->settings.tls_mode_value != tls_mode::disabled;
   }

   auto secrets = std::shared_ptr<forge::plugins::crypto::secrets::api>{};
   if (requires_tls_secrets) {
      try {
         secrets = context.apis()
                       .get<forge::plugins::crypto::secrets::api>(
                           {.id = {"forge.plugins.crypto.secrets"}, .major = 1, .min_revision = 0})
                       .shared();
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP TLS requires the local Crypto Secrets API");
      }
   }

   {
      const auto lock = std::scoped_lock{impl_->mutex};
      impl_->runtime = &context.scheduler().runtime_context();
      impl_->file_read_executor = context.has_compute() ? context.compute() : forge::asio::compute::executor{};
      impl_->apis = &context.apis().registry_ref();
      impl_->secrets = std::move(secrets);
      impl_->stopping = false;
      ++impl_->lifecycle_generation;
   }
   co_return;
}

boost::asio::awaitable<void> plugin::startup() {
   auto runtime = static_cast<forge::asio::runtime*>(nullptr);
   auto settings = config{};
   auto startup_generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->runtime == nullptr || impl_->apis == nullptr || impl_->stopping) {
         FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP server plugin is not initialized");
      }
      runtime = impl_->runtime;
      settings = impl_->settings;
      startup_generation = impl_->lifecycle_generation;
   }
   auto snapshot = impl_->close_publication();
   auto router = forge::net::http::router{};
   for (auto& middleware : snapshot.middleware) {
      router.use(to_http_middleware(std::move(middleware)));
   }
   for (auto& binding : snapshot.bindings) {
      const auto base_path = resolve_base_path(settings, binding.options.base_path);
      router.reserve_path_prefix(base_path);
      binding.binding.mount(router, base_path);
   }
   for (auto& mount : snapshot.route_mounts) {
      for (auto& path : mount.reserved_paths) {
         router.reserve_path_prefix(std::move(path));
      }
      mount.apply(router);
   }
   for (auto& mount : snapshot.asset_mounts) {
      router.mount_assets(std::move(mount));
   }
   auto server_config = to_server_config(settings);
   auto tls_context_provider = std::shared_ptr<forge::net::tls::context_provider>{};
   if (settings.tls_mode_value != tls_mode::disabled) {
      try {
         tls_context_provider = co_await impl_->make_tls_context_provider();
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP TLS material could not be loaded or validated");
      }
      {
         const auto lock = std::scoped_lock{impl_->mutex};
         if (impl_->stopping || impl_->lifecycle_generation != startup_generation) {
            FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP server startup was superseded by shutdown");
         }
         impl_->tls_context_provider = tls_context_provider;
      }
      server_config.tls_context_provider = std::move(tls_context_provider);
   }
   auto server = std::make_unique<forge::net::http::server>(*runtime, std::move(server_config), std::move(router));
   co_await server->async_start();
   auto accept_server = false;
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      accept_server = !impl_->stopping && impl_->lifecycle_generation == startup_generation;
      if (accept_server) {
         impl_->server = std::move(server);
      }
   }
   if (!accept_server) {
      co_await server->async_stop();
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP server startup was superseded by shutdown");
   }
}

void plugin::request_stop() noexcept {
   auto lock = std::scoped_lock{impl_->mutex};
   impl_->stopping = true;
   ++impl_->lifecycle_generation;
}

boost::asio::awaitable<void> plugin::shutdown() {
   auto server = std::unique_ptr<forge::net::http::server>{};
   {
      auto lock = std::scoped_lock{impl_->mutex};
      impl_->stopping = true;
      ++impl_->lifecycle_generation;
      server = std::move(impl_->server);
   }
   if (server) {
      co_await server->async_stop();
   }
   impl_->reset_runtime();
}

forge::app::plugin_descriptor descriptor() {
   return forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.net.http.server"},
       .factory = [] { return std::make_unique<plugin>(); },
   };
}

} // namespace forge::plugins::net::http::server
