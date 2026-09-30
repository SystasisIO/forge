module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/ip/address.hpp>

module forge.plugins.net.http.server.plugin;

import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.http.server;
import forge.plugins.net.http.server.exceptions;
import forge.plugins.net.http.server.types;

#include "details/config.hxx"

namespace forge::plugins::net::http::server {
namespace {

void validate_tls_config(config& value) {
   const auto has_server_identity =
       !value.tls_certificate_chain_secret.empty() || !value.tls_private_key_secret.empty();
   switch (value.tls_mode_value) {
   case tls_mode::disabled:
      if (has_server_identity || !value.tls_client_ca_secret.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "HTTP TLS secret identifiers require tls.mode server or mutual");
      }
      return;
   case tls_mode::server:
      if (value.tls_certificate_chain_secret.empty() || value.tls_private_key_secret.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "HTTP TLS server mode requires certificate-chain-secret and private-key-secret");
      }
      if (!value.tls_client_ca_secret.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "HTTP TLS client-ca-secret requires tls.mode mutual");
      }
      return;
   case tls_mode::mutual:
      if (value.tls_certificate_chain_secret.empty() || value.tls_private_key_secret.empty() ||
          value.tls_client_ca_secret.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "HTTP mutual TLS requires certificate-chain-secret, private-key-secret and "
                               "client-ca-secret");
      }
      return;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_config, "unknown HTTP TLS mode");
}

void normalize_bind_address(config& value) {
   if (value.bind_address == "localhost") {
      // The plugin accepts localhost as a deliberate loopback-only spelling and binds IPv4 loopback.
      value.bind_address = "127.0.0.1";
      return;
   }

   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value.bind_address, error);
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "HTTP server bind address is not a numeric address",
                            forge::exceptions::ctx("bind_address", value.bind_address));
   }
   if (value.tls_mode_value == tls_mode::disabled && !address.is_loopback()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "plaintext HTTP server mode requires an IPv4 or IPv6 loopback bind address",
                            forge::exceptions::ctx("bind_address", value.bind_address));
   }
}

} // namespace

config decode_config(const forge::config::core::component_view& view) {
   if (view.source().try_get("plugins.http.server")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "retired plugin configuration: plugins.http.server; use plugins.net.http.server");
   }
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, forge::config::core::format_decode_diagnostics(
                                                            "invalid HTTP server config", decoded.diagnostics));
   }
   decoded.value.api_base_path = normalize_base_path(decoded.value.api_base_path);
   validate_tls_config(decoded.value);
   normalize_bind_address(decoded.value);
   return std::move(decoded.value);
}

std::string normalize_base_path(std::string_view value) {
   if (value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "HTTP server API base path must not be empty");
   }
   if (value.front() != '/') {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "HTTP server API base path must start with /",
                            forge::exceptions::ctx("base_path", std::string{value}));
   }
   while (value.size() > 1U && value.back() == '/') {
      value.remove_suffix(1U);
   }
   return std::string{value};
}

std::string resolve_base_path(const config& settings, std::string_view override_value) {
   if (!override_value.empty()) {
      return normalize_base_path(override_value);
   }
   return normalize_base_path(settings.api_base_path);
}

forge::net::http::server_config to_server_config(const config& value) {
   return forge::net::http::server_config{
       .bind_address = value.bind_address,
       .port = static_cast<std::uint16_t>(value.port),
       .max_request_body_bytes = value.max_request_body_bytes,
       .max_header_bytes = value.max_header_bytes,
       .read_timeout = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value.read_timeout_ms)},
       .idle_timeout = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value.idle_timeout_ms)},
       .handshake_timeout =
           std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value.tls_handshake_timeout_ms)},
       .max_pending_tls_handshakes = value.tls_max_pending_handshakes,
   };
}

} // namespace forge::plugins::net::http::server
