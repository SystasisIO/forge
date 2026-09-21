module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.exceptions;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.quic.options;
import forge.net.quic.security;

#include "details/quic_client_token_cache.hxx"
#include "details/quic_client_options.hxx"

namespace forge::net::p2p::direct::detail {
namespace {

[[nodiscard]] std::string_view host_kind_name(forge::net::p2p::endpoint::host_kind kind) {
   using enum forge::net::p2p::endpoint::host_kind;
   switch (kind) {
   case ip4:
      return "ip4";
   case ip6:
      return "ip6";
   case dns:
      return "dns";
   case dns4:
      return "dns4";
   case dns6:
      return "dns6";
   }
   return "unknown";
}

} // namespace

forge::net::quic::security_options make_quic_peer_verifier(std::optional<peer_id> expected,
                                                           bool allow_insecure_test_mode) {
   if (allow_insecure_test_mode) {
      auto security = forge::net::quic::security_options{.verify_peer = true};
      security.verifier = [](const forge::net::quic::peer_certificate&) { return true; };
      return security;
   }
   auto security = forge::net::quic::security_options{.verify_peer = true};
   security.verifier = [expected = std::move(expected)](const forge::net::quic::peer_certificate& certificate) {
      try {
         const auto remote = make_peer_id_from_certificate_der(certificate.der);
         if (expected) {
            return remote == *expected;
         }
         return valid_peer_id(remote);
      } catch (const forge::exceptions::base&) {
         return false;
      }
   };
   return security;
}

forge::net::quic::client_options
make_quic_client_options(const forge::net::p2p::endpoint& endpoint, std::optional<peer_id> expected,
                         std::chrono::milliseconds timeout, forge::net::quic::transport_limits limits,
                         std::string certificate_pem, std::string private_key_pem, bool allow_insecure_test_mode,
                         const std::shared_ptr<quic_client_token_cache>& client_tokens) {
   auto out = forge::net::quic::client_options{
       .alpn = "libp2p",
       .connect_timeout = timeout,
       .handshake_timeout = timeout,
       .limits = limits,
       .security = make_quic_peer_verifier(expected, allow_insecure_test_mode),
       .certificate_pem = std::move(certificate_pem),
       .private_key_pem = std::move(private_key_pem),
   };
   if (!expected || allow_insecure_test_mode) {
      out.client_tokens = forge::net::quic::client_token_callbacks{};
      return out;
   }

   const auto peer = expected->to_bytes();
   const auto key = quic_client_token_cache::make_key(peer, host_kind_name(endpoint.transport.host_type),
                                                      endpoint.transport.host, endpoint.transport.zone, endpoint.transport.port);
   const auto cache = std::weak_ptr<quic_client_token_cache>{client_tokens};
   out.client_tokens = forge::net::quic::client_token_callbacks{
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
                 owned->store(key, std::move(token));
              }
           },
   };
   return out;
}

} // namespace forge::net::p2p::direct::detail
