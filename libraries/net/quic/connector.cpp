module;

#include <forge/exceptions/macros.hpp>

#include "details/wrapper_handles.hxx"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/system_error.hpp>

#include "details/client_token_cache.hxx"

module forge.net.quic.connector;

import forge.crypto.core.secret_string;
import forge.net.quic.exceptions;
import forge.net.quic.runtime;
import forge.net.quic.security;

#include "details/engine_client_options.hxx"

namespace forge::net::quic {
namespace {

[[nodiscard]] exceptions::code map_error(detail::engine_error_kind kind) noexcept {
   switch (kind) {
   case detail::engine_error_kind::invalid_endpoint:
      return exceptions::code::invalid_endpoint;
   case detail::engine_error_kind::invalid_options:
      return exceptions::code::invalid_options;
   case detail::engine_error_kind::dependency_unavailable:
      return exceptions::code::dependency_unavailable;
   case detail::engine_error_kind::connect_timeout:
      return exceptions::code::connect_timeout;
   case detail::engine_error_kind::handshake_timeout:
      return exceptions::code::handshake_timeout;
   case detail::engine_error_kind::idle_timeout:
      return exceptions::code::idle_timeout;
   case detail::engine_error_kind::tls_failed:
      return exceptions::code::tls_failed;
   case detail::engine_error_kind::peer_verification_failed:
      return exceptions::code::peer_verification_failed;
   case detail::engine_error_kind::alpn_mismatch:
      return exceptions::code::alpn_mismatch;
   case detail::engine_error_kind::frame_too_large:
      return exceptions::code::frame_too_large;
   case detail::engine_error_kind::malformed_frame:
      return exceptions::code::malformed_frame;
   case detail::engine_error_kind::backpressure_rejected:
      return exceptions::code::backpressure_rejected;
   case detail::engine_error_kind::connection_rejected:
      return exceptions::code::connection_rejected;
   case detail::engine_error_kind::connection_closed:
      return exceptions::code::connection_closed;
   case detail::engine_error_kind::stream_closed:
      return exceptions::code::stream_closed;
   case detail::engine_error_kind::stream_reset:
      return exceptions::code::stream_reset;
   case detail::engine_error_kind::canceled:
      return exceptions::code::canceled;
   case detail::engine_error_kind::internal_error:
      return exceptions::code::internal;
   }
   return exceptions::code::internal;
}

[[noreturn]] void raise_engine_failure(const detail::engine_failure& error) {
   FORGE_THROW_CODE(map_error(error.kind()), error.what());
}

[[nodiscard]] detail::engine_transport_limits map_limits(const transport_limits& limits) noexcept {
   return detail::engine_transport_limits{
       .max_connections = limits.max_connections,
       .max_streams_per_connection = limits.max_streams_per_connection,
       .max_queued_bytes = limits.max_queued_bytes,
       .max_inbound_queued_bytes = limits.max_inbound_queued_bytes,
       .max_inbound_queued_packets = limits.max_inbound_queued_packets,
       .max_frame_size = limits.max_frame_size,
   };
}

[[nodiscard]] detail::engine_endpoint::address_family map_address_family(const endpoint& value) noexcept {
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value.host, error);
   if (!error) {
      return address.is_v4() ? detail::engine_endpoint::address_family::ipv4
                             : detail::engine_endpoint::address_family::ipv6;
   }

   switch (value.family) {
   case endpoint::address_family::any:
      return detail::engine_endpoint::address_family::any;
   case endpoint::address_family::ipv4:
      return detail::engine_endpoint::address_family::ipv4;
   case endpoint::address_family::ipv6:
      return detail::engine_endpoint::address_family::ipv6;
   }
   return detail::engine_endpoint::address_family::any;
}

[[nodiscard]] detail::engine_security_options map_security(const security_options& security) {
   auto mapped = detail::engine_security_options{
       .verify_peer = security.verify_peer,
       .expected_sha256_fingerprint = security.expected_sha256_fingerprint,
       .trusted_ca_pem = security.trusted_ca_pem,
   };
   if (security.verifier) {
      mapped.verifier = [verifier = security.verifier](const detail::engine_peer_certificate& certificate) {
         return verifier(peer_certificate{
             .der = certificate.der,
             .sha256_fingerprint = certificate.sha256_fingerprint,
         });
      };
   }
   return mapped;
}

[[nodiscard]] detail::engine_client_options map_options(const client_options& options) {
   auto out = detail::engine_client_options{
       .alpn = options.alpn,
       .connect_timeout = options.connect_timeout,
       .handshake_timeout = options.handshake_timeout,
       .idle_timeout = options.idle_timeout,
       .limits = map_limits(options.limits),
       .security = map_security(options.security),
       .certificate_pem = options.certificate_pem,
       .private_key_pem = options.private_key_pem,
       .test_failpoint = options.test_failpoint,
       .connection_lifetime = options.connection_lifetime,
   };
   if (options.client_tokens && options.client_tokens->take && options.client_tokens->store) {
      out.client_tokens = detail::engine_client_options::token_callbacks{
          .take = options.client_tokens->take,
          .store = options.client_tokens->store,
      };
   }
   return out;
}

[[nodiscard]] std::string normalized_host(std::string_view value) {
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value, error);
   if (!error) {
      return address.to_string();
   }
   auto out = std::string{value};
   std::ranges::transform(out, out.begin(),
                          [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
   return out;
}

void append_cache_key_component(std::string& out, std::string_view value) {
   out += std::to_string(value.size());
   out.push_back(':');
   out.append(value);
}

[[nodiscard]] std::string standalone_cache_key(const endpoint& remote, detail::engine_endpoint::address_family family) {
   auto out = std::string{"forge-quic-v1"};
   append_cache_key_component(out, family == detail::engine_endpoint::address_family::ipv4   ? "ipv4"
                                   : family == detail::engine_endpoint::address_family::ipv6 ? "ipv6"
                                                                                             : "any");
   append_cache_key_component(out, normalized_host(remote.host));
   append_cache_key_component(out, remote.zone);
   append_cache_key_component(out, std::to_string(remote.port));
   return out;
}

} // namespace

struct connector::impl {
   explicit impl(forge::asio::runtime& runtime_value)
       : runtime(runtime_value), engine(runtime_value.context()),
         client_tokens(std::make_shared<detail::client_token_cache>()) {}

   forge::asio::runtime& runtime;
   detail::engine_connector engine;
   std::shared_ptr<detail::client_token_cache> client_tokens;
};

connector::connector(forge::asio::runtime& runtime) : impl_(std::make_unique<impl>(runtime)) {}

connector::~connector() = default;

boost::asio::awaitable<connection> connector::async_connect(endpoint remote, client_options options) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "invalid QUIC connector");
   }
   validate(options);
   const auto capabilities = initialize_runtime();
   if (!capabilities.crypto_ossl_initialized) {
      FORGE_THROW_EXCEPTION(exceptions::dependency_unavailable, "ngtcp2 OpenSSL crypto backend initialization failed");
   }
   try {
      const auto family = map_address_family(remote);
      auto engine_options = map_options(options);
      if (!options.client_tokens) {
         const auto key = standalone_cache_key(remote, family);
         const auto cache = std::weak_ptr<detail::client_token_cache>{impl_->client_tokens};
         engine_options.client_tokens = detail::engine_client_options::token_callbacks{
             .take =
                 [cache, key] {
                    if (const auto owned = cache.lock()) {
                       return owned->take(key);
                    }
                    return std::optional<std::vector<std::uint8_t>>{};
                 },
             .store =
                 [cache, key](std::vector<std::uint8_t> token) {
                    if (const auto owned = cache.lock()) {
                       static_cast<void>(owned->store(key, std::move(token)));
                    }
                 },
         };
      }
      auto engine_connection = co_await impl_->engine.async_connect(
          detail::engine_endpoint{
              .host = std::move(remote.host), .port = remote.port, .family = family, .zone = std::move(remote.zone)},
          std::move(engine_options));
      co_return detail::connection_access::make(detail::connection_handle{.engine = std::move(engine_connection)});
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   } catch (const boost::system::system_error& error) {
      if (error.code() == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "QUIC client connect canceled");
      }
      throw;
   }
}

void connector::cancel() {
   if (impl_) {
      impl_->engine.cancel();
   }
}

} // namespace forge::net::quic
