module;

#include <forge/exceptions/macros.hpp>

#include "details/wrapper_handles.hxx"

#include <memory>
#include <utility>

#include <boost/asio/awaitable.hpp>

module forge.net.quic.listener;

import forge.crypto.core.secret_string;
import forge.net.quic.exceptions;
import forge.net.quic.runtime;
import forge.net.quic.security;

#include "details/engine_server_options.hxx"

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

[[nodiscard]] endpoint map_endpoint(const detail::engine_endpoint& value) {
   return endpoint{.host = value.host,
                   .port = value.port,
                   .family =
                       value.family == detail::engine_endpoint::address_family::ipv4   ? endpoint::address_family::ipv4
                       : value.family == detail::engine_endpoint::address_family::ipv6 ? endpoint::address_family::ipv6
                                                                                       : endpoint::address_family::any,
                   .zone = value.zone};
}

[[nodiscard]] detail::engine_server_options map_options(const server_options& options) {
   auto out = detail::engine_server_options{
       .alpn = options.alpn,
       .handshake_timeout = options.handshake_timeout,
       .idle_timeout = options.idle_timeout,
       .limits = map_limits(options.limits),
       .security = map_security(options.security),
       .certificate_pem = options.certificate_pem,
       .private_key_pem = options.private_key_pem,
       .inbound_admission = options.inbound_admission,
   };
   if (options.inbound_connection_filter) {
      out.inbound_connection_filter = [filter = options.inbound_connection_filter](
                                          const detail::engine_endpoint& local, const detail::engine_endpoint& remote) {
         return filter(map_endpoint(local), map_endpoint(remote));
      };
   }
   return out;
}

} // namespace

struct listener::impl {
   impl(forge::asio::runtime& runtime_value, endpoint bind_endpoint_value, server_options options_value)
       : runtime(runtime_value), engine(runtime_value.context(),
                                        detail::engine_endpoint{.host = std::move(bind_endpoint_value.host),
                                                                .port = bind_endpoint_value.port,
                                                                .zone = std::move(bind_endpoint_value.zone)},
                                        map_options(options_value)) {}

   forge::asio::runtime& runtime;
   detail::engine_listener engine;
};

listener::listener(forge::asio::runtime& runtime, endpoint bind_endpoint, server_options options) {
   validate(options);
   const auto capabilities = initialize_runtime();
   if (!capabilities.crypto_ossl_initialized) {
      FORGE_THROW_EXCEPTION(exceptions::dependency_unavailable, "ngtcp2 OpenSSL crypto backend initialization failed");
   }
   impl_ = std::make_unique<impl>(runtime, std::move(bind_endpoint), std::move(options));
}

listener::~listener() = default;

endpoint listener::local_endpoint() const {
   if (!impl_) {
      return endpoint{};
   }
   const auto local = impl_->engine.local_endpoint();
   return map_endpoint(local);
}

boost::asio::awaitable<connection> listener::async_accept() {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::connection_closed, "invalid QUIC listener");
   }
   try {
      auto engine_connection = co_await impl_->engine.async_accept();
      co_return detail::connection_access::make(detail::connection_handle{.engine = std::move(engine_connection)});
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   }
}

boost::asio::awaitable<void> listener::async_stop() {
   if (impl_) {
      co_await impl_->engine.async_stop();
   }
}

void listener::stop() {
   if (impl_) {
      impl_->engine.stop();
   }
}

} // namespace forge::net::quic
