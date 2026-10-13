module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <boost/scope/scope_exit.hpp>

#include <coroutine>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.net.http.server.plugin;

import forge.api.core.registry;
import forge.asio.compute;
import forge.asio.runtime;
import forge.api.http.binding;
import forge.crypto.core.types;
import forge.crypto.core.secret_bytes;
import forge.net.http.assets;
import forge.net.http.server;
import forge.net.tls.context;
import forge.net.tls.options;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.http.server.exceptions;
import forge.plugins.net.http.server.middleware;
import forge.plugins.net.http.server.types;
import forge.plugins.net.http.server.routes;

#include "details/plugin_impl.hxx"
#include "details/tls_secret_material.hxx"

namespace forge::plugins::net::http::server {
plugin::impl::impl(forge::app::plugin_lifecycle value) : lifecycle{std::move(value)} {}

namespace {

constexpr auto tls_certificate_chain_purpose = std::string_view{"http.server.tls.certificate-chain"};
constexpr auto tls_private_key_purpose = std::string_view{"http.server.tls.private-key"};
constexpr auto tls_client_ca_purpose = std::string_view{"http.server.tls.client-ca"};

[[nodiscard]] std::string secret_text(forge::crypto::core::bytes& bytes) {
   auto cleanup = boost::scope::scope_exit{[&bytes] { forge::crypto::core::secure_erase(bytes); }};
   return std::string{bytes.begin(), bytes.end()};
}

void clear_text(std::string& value) noexcept {
   forge::crypto::core::secure_erase(value);
}

void clear_tls_context_options(forge::net::tls::context_options& options) noexcept {
   clear_text(options.certificate_chain_pem);
   clear_text(options.private_key_pem);
   for (auto& authority : options.trust_anchors_pem) {
      clear_text(authority);
   }
   options.trust_anchors_pem.clear();
}

boost::asio::awaitable<std::string> load_secret_text(forge::plugins::crypto::secrets::api& secrets,
                                                     std::string_view secret_id, std::string_view purpose) {
   auto result = co_await secrets.get_bytes({.secret_id = std::string{secret_id}, .purpose = std::string{purpose}});
   co_return secret_text(result.bytes);
}

boost::asio::awaitable<tls_secret_material>
load_tls_secret_material(const config& settings, const std::shared_ptr<forge::plugins::crypto::secrets::api>& secrets) {
   if (settings.tls_mode_value == tls_mode::disabled || !secrets) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP TLS is not configured");
   }

   auto material = tls_secret_material{};
   material.certificate_chain =
       co_await load_secret_text(*secrets, settings.tls_certificate_chain_secret, tls_certificate_chain_purpose);
   material.private_key = co_await load_secret_text(*secrets, settings.tls_private_key_secret, tls_private_key_purpose);
   if (settings.tls_mode_value == tls_mode::mutual) {
      material.client_ca = co_await load_secret_text(*secrets, settings.tls_client_ca_secret, tls_client_ca_purpose);
   }
   co_return material;
}

[[nodiscard]] forge::net::tls::context_options make_tls_context_options(const config& settings,
                                                                        const tls_secret_material& material) {
   return forge::net::tls::context_options{
       .role = forge::net::tls::endpoint_role::server,
       .protocols = forge::net::tls::protocol_policy::tls13_only,
       .verification = settings.tls_mode_value == tls_mode::mutual
                           ? forge::net::tls::peer_verification::require_peer_certificate
                           : forge::net::tls::peer_verification::none,
       .certificate_chain_pem = material.certificate_chain,
       .private_key_pem = material.private_key,
       .trust_anchors_pem = settings.tls_mode_value == tls_mode::mutual ? std::vector<std::string>{material.client_ca}
                                                                        : std::vector<std::string>{},
       .alpn_protocols = {"http/1.1"},
       .use_default_verify_paths = false,
   };
}

} // namespace

void plugin::impl::add(pending_binding value) {
   const auto generation = lifecycle.snapshot().generation;
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      if (value.options.require_mutual_tls && settings.tls_mode_value != tls_mode::mutual) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "API publication requires mutual TLS");
      }
      if (publication_closed) {
         FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
      }
      bindings.push_back(std::move(value));
   })) {
      FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
   }
}

void plugin::impl::add(middleware_descriptor value) {
   const auto generation = lifecycle.snapshot().generation;
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      if (publication_closed) {
         FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
      }
      middleware.push_back(std::move(value));
   })) {
      FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
   }
}

void plugin::impl::add(forge::net::http::asset_mount value) {
   const auto generation = lifecycle.snapshot().generation;
   auto executor = forge::asio::compute::executor{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (publication_closed) {
         FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
      }
      executor = file_read_executor;
   }
   if (!executor.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "HTTP asset mounts require the application compute executor");
   }

   auto bundle = forge::net::http::asset_bundle{std::move(value), std::move(executor)};
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      if (publication_closed) {
         FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
      }
      for (const auto& existing : asset_mounts) {
         if (existing.contains(bundle.path()) || bundle.contains(existing.path())) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "HTTP asset mounts must not overlap");
         }
      }
      asset_mounts.push_back(std::move(bundle));
   })) {
      FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP server publication is closed");
   }
}

void plugin::impl::add(route_mount value) {
   const auto generation = lifecycle.snapshot().generation;
   if (!lifecycle.try_admit(generation, [&] {
      const auto lock = std::scoped_lock{mutex};
      if (publication_closed) {
         FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP route publication is closed");
      }
      if (value.id.empty() || !value.apply || route_mounts.size() >= 128 ||
          std::ranges::any_of(route_mounts, [&](const auto& saved) { return saved.id == value.id; })) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Invalid or duplicate HTTP route mount");
      }
      route_mounts.push_back(std::move(value));
   })) {
      FORGE_THROW_EXCEPTION(exceptions::publication_closed, "HTTP route publication is closed");
   }
}

startup_snapshot plugin::impl::close_publication() {
   auto lock = std::scoped_lock{mutex};
   publication_closed = true;
   return startup_snapshot{
       .bindings = std::move(bindings),
       .middleware = std::move(middleware),
       .asset_mounts = std::move(asset_mounts),
       .route_mounts = std::move(route_mounts),
   };
}

boost::asio::awaitable<std::shared_ptr<forge::net::tls::context_provider>> plugin::impl::make_tls_context_provider() {
   auto settings_copy = config{};
   auto secrets_copy = std::shared_ptr<forge::plugins::crypto::secrets::api>{};
   {
      const auto lock = std::scoped_lock{mutex};
      settings_copy = settings;
      secrets_copy = secrets;
   }
   auto material = co_await load_tls_secret_material(settings_copy, secrets_copy);
   auto options = make_tls_context_options(settings_copy, material);
   auto erase_options = boost::scope::scope_exit{[&] { clear_tls_context_options(options); }};
   co_return std::make_shared<forge::net::tls::context_provider>(std::move(options));
}

boost::asio::awaitable<void> plugin::impl::reload_tls() {
   auto settings_copy = config{};
   auto secrets_copy = std::shared_ptr<forge::plugins::crypto::secrets::api>{};
   auto provider = std::shared_ptr<forge::net::tls::context_provider>{};
   const auto reload_state = lifecycle.snapshot();
   const auto reload_generation = reload_state.generation;
   if (reload_state.phase != forge::app::lifecycle_phase::started) {
      FORGE_THROW_EXCEPTION(exceptions::tls_reload_failed, "HTTP TLS reload requires a running server");
   }
   if (!lifecycle.try_admit(reload_generation, forge::app::lifecycle_phase::started, [&] {
      const auto lock = std::scoped_lock{mutex};
      if (settings.tls_mode_value == tls_mode::disabled || !tls_context_provider || !secrets) {
         FORGE_THROW_EXCEPTION(exceptions::tls_reload_failed, "HTTP TLS reload is unavailable when TLS is disabled");
      }
      settings_copy = settings;
      secrets_copy = secrets;
      provider = tls_context_provider;
   })) {
      FORGE_THROW_EXCEPTION(exceptions::tls_reload_failed, "HTTP TLS reload was stopped");
   }

   try {
      auto material = co_await load_tls_secret_material(settings_copy, secrets_copy);
      auto replacement = make_tls_context_options(settings_copy, material);
      auto erase_replacement = boost::scope::scope_exit{[&] { clear_tls_context_options(replacement); }};
      auto prepared = forge::net::tls::make_context(std::move(replacement));
      auto retired = forge::net::tls::context_snapshot_ptr{};

      if (!lifecycle.try_admit(reload_generation, forge::app::lifecycle_phase::started, [&] {
         const auto lock = std::scoped_lock{mutex};
         if (tls_context_provider != provider) {
            FORGE_THROW_EXCEPTION(exceptions::tls_reload_failed, "HTTP TLS provider was retired");
         }
         retired = provider->exchange(std::move(prepared));
      })) {
         FORGE_THROW_EXCEPTION(exceptions::tls_reload_failed, "HTTP TLS reload was stopped");
      }
      // Prepared and retired context owners are destroyed only after both locks.
   } catch (const exceptions::tls_reload_failed&) {
      throw;
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::tls_reload_failed,
                            "HTTP TLS reload failed; the active TLS context was preserved");
   }
   co_return;
}

void plugin::impl::reset_runtime() noexcept {
   auto retired = startup_snapshot{};
   auto retired_secrets = std::shared_ptr<forge::plugins::crypto::secrets::api>{};
   auto retired_tls = std::shared_ptr<forge::net::tls::context_provider>{};
   auto retired_executor = forge::asio::compute::executor{};
   {
      const auto lock = std::scoped_lock{mutex};
      apis = nullptr;
      runtime = nullptr;
      publication_closed = true;
      retired_executor = std::move(file_read_executor);
      retired_secrets.swap(secrets);
      retired_tls.swap(tls_context_provider);
      retired.bindings.swap(bindings);
      retired.middleware.swap(middleware);
      retired.asset_mounts.swap(asset_mounts);
      retired.route_mounts.swap(route_mounts);
   }
}

} // namespace forge::plugins::net::http::server
