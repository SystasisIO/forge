module;

#include <boost/test/unit_test.hpp>
#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "libp2p_identity_fixture.hxx"
#include "../fixtures/local_dns_server.hxx"

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.gate;
import forge.asio.notification;
import forge.asio.runtime;
import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.duplex_stream;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.core.server_supplied;
import forge.api.core.trusted_invocation;
import forge.api.core.types;
import forge.api.auth.authenticated_caller;
import forge.api.p2p.authenticated_peer;
import forge.api.p2p.binding;
import forge.api.transport.connection;
import forge.crypto.asymmetric;
import forge.crypto.pki.der;
import forge.crypto.asymmetric.p256;
import forge.crypto.pki.pem;
import forge.crypto.asymmetric.rsa;
import forge.crypto.asymmetric.secp256k1;
import forge.crypto.symmetric.xsalsa20;
import forge.crypto.pki.x509;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.connection_gater;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dialing;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.chrono.timestamp;
import forge.net.p2p.ipns;
import forge.net.p2p.lifecycle;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.rendezvous;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.quic.options;
import forge.net.quic.security;
import forge.net.p2p.topology;
import forge.net.quic.endpoint;
import forge.net.quic.libp2p;
import forge.net.quic.transport;
import forge.net.stcp.connection;
import forge.net.pnet.protector;
import forge.net.transport.endpoint;
import forge.net.transport.exceptions;
import forge.net.transport.frame;
import forge.net.transport.stream;
import forge.net.tcp.connection;
import forge.net.tcp.listener;
import forge.net.yamux.session;
import forge.multiformats.exceptions;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.multicodec;
import forge.multiformats.multihash;
import forge.multiformats.multibase;
import forge.multiformats.multiaddr;

#include "../../libraries/net/p2p/details/connection_manager.hxx"

namespace forge::net::p2p::detail {
void fail_next_connection_manager_prepare_for_test(node& owner);
void fail_next_connection_manager_peer_session_prepare_for_test() noexcept;
}

namespace p2p_live_types {

struct caller_peer {
   forge::net::p2p::peer_id id;
};

BOOST_DESCRIBE_STRUCT(caller_peer, (), (id))

} // namespace p2p_live_types

namespace forge::api::core {

template <> struct server_supplied<p2p_live_types::caller_peer> {
   static constexpr bool required = true;

   static void reset(p2p_live_types::caller_peer& value) {
      value = {};
   }

   [[nodiscard]] static bool apply(p2p_live_types::caller_peer& value, const trusted_invocation& trusted) {
      const auto* peer = trusted.find<forge::api::p2p::authenticated_peer>();
      if (peer == nullptr) {
         return false;
      }
      value.id = peer->id;
      return true;
   }
};

} // namespace forge::api::core

namespace p2p_live_types {

class live_api : public forge::api::core::contract<live_api, forge::api::core::surface::local |
                                                                 forge::api::core::surface::remote> {
 public:
   virtual ~live_api() = default;

   virtual boost::asio::awaitable<void> exchange(caller_peer, forge::api::auth::authenticated_caller,
                                                 forge::api::core::duplex_stream<std::uint32_t, std::uint32_t>) = 0;
};

} // namespace p2p_live_types

FORGE_API(::p2p_live_types::live_api, FORGE_API_CONTRACT("test.p2p.live", 1, 0),
          FORGE_API_METHOD(exchange, authenticated, caller))

#include "../../libraries/net/p2p/details/session_lifecycle.hxx"
#include "../../libraries/net/p2p/details/libp2p_tls.hxx"
#include "../../libraries/net/p2p/details/dht_exchange.hxx"
#include "../../libraries/net/p2p/details/dht_fanout.hxx"
#include "../../libraries/net/p2p/details/dht_query.hxx"
#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/host_addresses.hxx"
#include "../../libraries/net/p2p/details/relay_budget.hxx"
#include "../../libraries/net/p2p/details/relay_pair.hxx"
#include "../../libraries/net/p2p/details/resource_stream.hxx"
#include "../../libraries/net/p2p/details/connection_singleflight_registry.hxx"
#include "../../libraries/net/p2p/details/quic_client_token_cache.hxx"
#include "../../libraries/net/p2p/details/quic_client_options.hxx"
#include "../../libraries/net/p2p/details/operation_deadline.hxx"
#include "../../libraries/net/p2p/details/owner_cancellation.hxx"
#include "../../libraries/net/p2p/details/relay_hop_exchange.hxx"
#include "../../libraries/net/p2p/details/peer_exchange_codec.hxx"
#include "../../libraries/net/p2p/details/peer_exchange_cancellation.hxx"
#include "../../libraries/net/p2p/details/peer_exchange_learning.hxx"
#include "../../libraries/net/p2p/details/peer_exchange_scheduler.hxx"
#include "../../libraries/net/p2p/details/worker_stop_bridge.hxx"
#include "../../libraries/net/p2p/details/relay_discovery.hxx"
#include "../../libraries/net/p2p/details/pubsub_outbound_budget.hxx"
#include "../../libraries/net/p2p/details/peer_failure.hxx"
#include "../../libraries/net/p2p/details/lifecycle_tracker.hxx"
#include "../../libraries/net/p2p/details/topology_dht_fanout.hxx"
#include "../../libraries/net/p2p/details/topology_peer_exchange_claims.hxx"
#include "../../libraries/net/p2p/details/stream_upgrade.hxx"

namespace forge::net::p2p {

[[nodiscard]] std::chrono::system_clock::time_point
dht_value_expiry(const dht::record& value, std::chrono::system_clock::time_point now, const dht::profile& profile);

namespace {

using live_api = p2p_live_types::live_api;

void cancel_timer_noexcept(const std::shared_ptr<boost::asio::steady_timer>& timer) noexcept {
   try {
      static_cast<void>(timer->cancel());
   } catch (...) {
   }
}

class live_impl final : public live_api {
 public:
   boost::asio::awaitable<void>
   exchange(p2p_live_types::caller_peer authenticated, forge::api::auth::authenticated_caller authenticated_caller,
            forge::api::core::duplex_stream<std::uint32_t, std::uint32_t> stream) override {
      calls.fetch_add(1U, std::memory_order_release);
      caller = std::move(authenticated.id);
      transport_caller = std::move(authenticated_caller);
      while (const auto value = co_await stream.async_read()) {
         co_await stream.async_write(*value * 2U);
      }
      co_await stream.async_close();
   }

   peer_id caller;
   forge::api::auth::authenticated_caller transport_caller;
   std::atomic_size_t calls{0};
};

struct product_announce {
   std::string ref;

   bool operator==(const product_announce&) const = default;
};

BOOST_DESCRIBE_STRUCT(product_announce, (), (ref))

struct bio_deleter {
   void operator()(BIO* value) const noexcept {
      BIO_free(value);
   }
};

struct evp_pkey_deleter {
   void operator()(EVP_PKEY* value) const noexcept {
      EVP_PKEY_free(value);
   }
};

struct evp_pkey_ctx_deleter {
   void operator()(EVP_PKEY_CTX* value) const noexcept {
      EVP_PKEY_CTX_free(value);
   }
};

struct x509_deleter {
   void operator()(X509* value) const noexcept {
      X509_free(value);
   }
};

struct asn1_object_deleter {
   void operator()(ASN1_OBJECT* value) const noexcept {
      ASN1_OBJECT_free(value);
   }
};

struct asn1_octet_string_deleter {
   void operator()(ASN1_OCTET_STRING* value) const noexcept {
      ASN1_OCTET_STRING_free(value);
   }
};

struct x509_extension_deleter {
   void operator()(X509_EXTENSION* value) const noexcept {
      X509_EXTENSION_free(value);
   }
};

struct test_identity {
   public_key key;
   forge::crypto::asymmetric::private_key private_key;
   std::string private_key_pem;
   peer_id peer;
};

struct test_certificate_identity {
   std::string certificate_pem;
   std::string private_key_pem;
   peer_id peer;
};

std::string bio_to_string(BIO* value) {
   BUF_MEM* buffer = nullptr;
   BIO_get_mem_ptr(value, &buffer);
   if (buffer == nullptr || buffer->data == nullptr) {
      throw std::runtime_error{"failed to read BIO buffer"};
   }
   return {buffer->data, buffer->length};
}

test_identity make_test_identity() {
   auto key = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>{EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519")};
   if (!key) {
      throw std::runtime_error{"failed to generate Ed25519 identity"};
   }

   auto public_size = std::size_t{};
   if (EVP_PKEY_get_raw_public_key(key.get(), nullptr, &public_size) != 1 || public_size == 0) {
      throw std::runtime_error{"failed to size Ed25519 public key"};
   }
   auto public_bytes = std::vector<std::uint8_t>(public_size);
   if (EVP_PKEY_get_raw_public_key(key.get(), public_bytes.data(), &public_size) != 1) {
      throw std::runtime_error{"failed to read Ed25519 public key"};
   }
   public_bytes.resize(public_size);

   auto private_key_bio = std::unique_ptr<BIO, bio_deleter>{BIO_new(BIO_s_mem())};
   if (!private_key_bio ||
       PEM_write_bio_PrivateKey(private_key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
      throw std::runtime_error{"failed to write Ed25519 private key PEM"};
   }

   const auto private_key_pem = bio_to_string(private_key_bio.get());
   auto out = test_identity{
       .key = public_key{.type = public_key::type::ed25519, .data = std::move(public_bytes)},
       .private_key = forge::crypto::pki::pem::read_private_key(private_key_pem),
       .private_key_pem = private_key_pem,
   };
   out.peer = make_peer_id(out.key);
   return out;
}

template <typename Range> std::vector<std::uint8_t> bytes_from_range(const Range& value) {
   auto out = std::vector<std::uint8_t>{};
   out.reserve(value.size());
   for (const auto byte : value) {
      out.push_back(static_cast<std::uint8_t>(byte));
   }
   return out;
}

std::vector<std::uint8_t> certificate_public_key_der(X509* certificate);
std::vector<std::uint8_t> signed_key_der(std::span<const std::uint8_t> public_key,
                                         std::span<const std::uint8_t> signature);
std::vector<std::uint8_t> tls_identity_message(std::span<const std::uint8_t> certificate_public_key);

test_identity make_secp256k1_identity() {
   auto private_key =
       forge::crypto::asymmetric::private_key::generate<forge::crypto::asymmetric::secp256k1::private_key>();
   auto key = public_key{
       .type = public_key::type::secp256k1,
       .data = bytes_from_range(
           std::get<forge::crypto::asymmetric::k1_public_key>(private_key.get_public_key()).serialize()),
   };
   auto out = test_identity{.key = std::move(key), .private_key = private_key};
   out.peer = make_peer_id(out.key);
   return out;
}

test_identity make_p256_identity() {
   auto private_key =
       forge::crypto::asymmetric::private_key::generate_p256<forge::crypto::asymmetric::p256::private_key>();
   auto key = public_key{
       .type = public_key::type::ecdsa,
       .data = forge::crypto::pki::der::write_public_key(private_key.get_public_key()),
   };
   auto out = test_identity{.key = std::move(key), .private_key = private_key};
   out.peer = make_peer_id(out.key);
   return out;
}

[[nodiscard]] forge::multiformats::multiaddr address_for(const endpoint& value) {
   return value.to_multiaddr();
}

[[nodiscard]] std::vector<forge::multiformats::multiaddr> addresses_for(std::vector<endpoint> values) {
   auto addresses = std::vector<forge::multiformats::multiaddr>{};
   addresses.reserve(values.size());
   for (const auto& value : values) {
      addresses.push_back(address_for(value));
   }
   return addresses;
}

[[nodiscard]] std::vector<forge::multiformats::multiaddr>
addresses_for(std::initializer_list<endpoint> values) {
   auto addresses = std::vector<forge::multiformats::multiaddr>{};
   addresses.reserve(values.size());
   for (const auto& value : values) {
      addresses.push_back(address_for(value));
   }
   return addresses;
}

test_identity make_rsa_identity() {
   auto private_key = forge::crypto::asymmetric::private_key::generate<forge::crypto::asymmetric::rsa::private_key>();
   auto key = public_key{
       .type = public_key::type::rsa,
       .data = std::get<forge::crypto::asymmetric::rsa_public_key>(private_key.get_public_key()).serialize(),
   };
   auto out = test_identity{.key = std::move(key), .private_key = private_key};
   out.peer = make_peer_id(out.key);
   return out;
}

std::vector<std::uint8_t> make_signed_rendezvous_peer_record(const test_identity& identity,
                                                             std::vector<endpoint> endpoints = {},
                                                             std::uint64_t sequence = 1) {
   if (endpoints.empty()) {
      endpoints.push_back(parse_endpoint("/ip4/127.0.0.1/udp/4401/quic-v1/p2p/" + identity.peer.to_string()));
   }
   return rendezvous::codec::seal_peer_record(
              rendezvous::peer_record{
                  .peer = identity.peer,
                  .endpoints = addresses_for(std::move(endpoints)),
                  .sequence = sequence,
              },
              identity.key, forge::crypto::pki::pem::read_private_key(identity.private_key_pem))
       .encode();
}

std::vector<std::uint8_t> make_signed_rendezvous_peer_record(const test_certificate_identity& identity,
                                                             std::vector<endpoint> endpoints = {},
                                                             std::uint64_t sequence = 1) {
   const auto private_key = forge::crypto::pki::pem::read_private_key(identity.private_key_pem);
   const auto key = public_key{
       .type = public_key::type::rsa,
       .data = std::get<forge::crypto::asymmetric::rsa_public_key>(private_key.get_public_key()).serialize(),
   };
   if (endpoints.empty()) {
      endpoints.push_back(parse_endpoint("/ip4/127.0.0.1/udp/4401/quic-v1/p2p/" + identity.peer.to_string()));
   }
   return rendezvous::codec::seal_peer_record(
              rendezvous::peer_record{
                  .peer = identity.peer,
                  .endpoints = addresses_for(std::move(endpoints)),
                  .sequence = sequence,
              },
              key, private_key)
       .encode();
}

public_key public_key_for(const test_certificate_identity& identity) {
   const auto private_key = forge::crypto::pki::pem::read_private_key(identity.private_key_pem);
   return public_key{
       .type = public_key::type::rsa,
       .data = std::get<forge::crypto::asymmetric::rsa_public_key>(private_key.get_public_key()).serialize(),
   };
}

[[nodiscard]] std::vector<std::uint8_t> identify_peer_record_payload_type() {
   return {0x03, 0x01};
}

std::vector<std::uint8_t> make_signed_identify_peer_record(const test_identity& identity,
                                                           std::vector<endpoint> endpoints = {},
                                                           std::uint64_t sequence = 1) {
   if (endpoints.empty()) {
      endpoints.push_back(parse_endpoint("/ip4/127.0.0.1/udp/4401/quic-v1/p2p/" + identity.peer.to_string()));
   }
   const auto payload = rendezvous::codec::encode_peer_record(rendezvous::peer_record{
       .peer = identity.peer,
       .endpoints = addresses_for(std::move(endpoints)),
       .sequence = sequence,
   });
   return signed_envelope::seal(identity.key, forge::crypto::pki::pem::read_private_key(identity.private_key_pem),
                                "libp2p-peer-record", identify_peer_record_payload_type(), payload)
       .encode();
}

std::vector<std::uint8_t> make_signed_identify_peer_record(const test_certificate_identity& identity,
                                                           std::vector<endpoint> endpoints = {},
                                                           std::uint64_t sequence = 1) {
   if (endpoints.empty()) {
      endpoints.push_back(parse_endpoint("/ip4/127.0.0.1/udp/4401/quic-v1/p2p/" + identity.peer.to_string()));
   }
   const auto payload = rendezvous::codec::encode_peer_record(rendezvous::peer_record{
       .peer = identity.peer,
       .endpoints = addresses_for(std::move(endpoints)),
       .sequence = sequence,
   });
   return signed_envelope::seal(public_key_for(identity),
                                forge::crypto::pki::pem::read_private_key(identity.private_key_pem),
                                "libp2p-peer-record", identify_peer_record_payload_type(), payload)
       .encode();
}

[[nodiscard]] std::uint64_t identify_peer_record_sequence(std::span<const std::uint8_t> bytes) {
   return rendezvous::codec::decode_peer_record(signed_envelope::decode(bytes).payload).sequence;
}

test_certificate_identity make_test_certificate_identity(std::string_view common_name) {
   auto fixture = forge::tests::p2p::make_identity_fixture(common_name);
   auto out = test_certificate_identity{
       .certificate_pem = std::move(fixture.certificate_pem),
       .private_key_pem = std::move(fixture.private_key_pem),
   };
   out.peer = make_peer_id_from_certificate_pem(out.certificate_pem);
   return out;
}

std::string_view test_certificate() {
   return "-----BEGIN CERTIFICATE-----\n"
          "MIICpDCCAYwCCQCJjaEDxrQqBzANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAkx\n"
          "MjcuMC4wLjEwHhcNMjYwNDI5MDgwMTMzWhcNMjYwNDMwMDgwMTMzWjAUMRIwEAYD\n"
          "VQQDDAkxMjcuMC4wLjEwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDy\n"
          "sbPH/R4QUz725sY376knXjSDCA+O5+Udwqfl4qaXHTAooWfplVY/WFRCnnMV6+TX\n"
          "gl9tHkNpKmI92s4O/LuJ5xnCCPX8k5i70gSnaGpClYSx+0gix8QgddDDsbLbIU/+\n"
          "x7MRWXfKYd/ArGNelPMadlvmcoEhumVUAwjYSV26GhNAmUacJlho3ltyujYSGFOS\n"
          "lI/lDqIjZxo7jbAGMMpiyu1omQ5nxjTm+bfOTcksBRMQP8mDz0vYXHXirA+xDfuv\n"
          "M+mTj6eO4UQ42w+iVLqhSPEhfLURmR4NULtPmq9hT7d1wS/Ys9q4Hj/j+kcXRCXj\n"
          "nPOZzBinLRTDnE59HbDZAgMBAAEwDQYJKoZIhvcNAQELBQADggEBAHSOUQTEDgjC\n"
          "uwza9ayfThJTs43j+TziWHLlowqCiHt/ipRNFEW7L0ibTnbMdQBFGfaLkTAhc5Rd\n"
          "6O6x+9o76pgEYxEg0rDkgNXmprNmS+nL7Are+iiF6R+X8dts3MQgtONPApAXE96P\n"
          "/n5K4GDQTd3WCI37hkmJA6rmwziFDTlwqtKWts39g8PqAbXac27rVR/iD0gWdOws\n"
          "qiaoGj/0WW9qcgjYGdCc0/CbbnyiWbi48VVf0yyfm7wgcz90byaKIQchHdb/qjyU\n"
          "wy7nfU5TJ5MKQ5yeqPTWmPYZZp9TKa5VD6wZD/IH7jH3GdJ/fSyroVLZktVnmxJa\n"
          "dmG/9wwivwQ=\n"
          "-----END CERTIFICATE-----\n";
}

std::string_view test_private_key() {
   return "-----BEGIN PRIVATE KEY-----\n"
          "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDysbPH/R4QUz72\n"
          "5sY376knXjSDCA+O5+Udwqfl4qaXHTAooWfplVY/WFRCnnMV6+TXgl9tHkNpKmI9\n"
          "2s4O/LuJ5xnCCPX8k5i70gSnaGpClYSx+0gix8QgddDDsbLbIU/+x7MRWXfKYd/A\n"
          "rGNelPMadlvmcoEhumVUAwjYSV26GhNAmUacJlho3ltyujYSGFOSlI/lDqIjZxo7\n"
          "jbAGMMpiyu1omQ5nxjTm+bfOTcksBRMQP8mDz0vYXHXirA+xDfuvM+mTj6eO4UQ4\n"
          "2w+iVLqhSPEhfLURmR4NULtPmq9hT7d1wS/Ys9q4Hj/j+kcXRCXjnPOZzBinLRTD\n"
          "nE59HbDZAgMBAAECggEBAIWVjHhy+V5RA+JRCh/12ayirNLG2BF30OP9pf7iL4IT\n"
          "/dMPbKvkmDGLw+1bW8tgKXj5+N6N/trfCm4zhqI3OF7ihooH9qYM88/F/OvMjFiU\n"
          "BhMVVhJW1LxtPPjKUcFN58M8VnMhRM9v6gIaoSOJZvpU1abVtgBDocyJUxAB6gYp\n"
          "i7MzoRwHGsL5mW/luE5H92/S8NNwLWBDA7DIGfrTZ6POf92h5I5W3CuTcqR5FICz\n"
          "3pfU3i443yZmsmkc9duH2gZ9cb9j4pRtNLbbsGmRVrBlgnkVFk8JWbikc8MpLeKO\n"
          "VKP7A2NvxJIrc7oFYrf4hbw8P70YL7S9B3W3yBPPzJECgYEA+Y3nG8CtvVTE/Keo\n"
          "qb5Rljlnj9DEffrylLyYUYfSSNR4Olc2WCPBiz0rPCDdO0VGeXAwqLf2VP7IEyAx\n"
          "kvrnqhzHWMhiLv+k4tIVyKCwpuofN0JsoUCi7CwRf+H2Pg+t6ewLV116THKsd41H\n"
          "IRElWyEvZsmbbhlLrsxUtfFZWnUCgYEA+PZwXUn+cb8kRmfG959gMawTtcfvnBUX\n"
          "sIn7LQl/ZWUIiLMWCaS3FbqkiGjaEYo6om1invYNJNA9zp/ECauSDp58NICCL0ie\n"
          "L7z26sEa6Ocg2VdR4ezpN3cM6dyAKfTFGb9V6qjyqNIPCE4eey6ZJ+CU/mpEfSDu\n"
          "+RGMzfdDCFUCgYEA5FRUn0zk6jU0YyMXq+9pgLSXL7vI/Kdt6m7AQuCto1tbga2o\n"
          "GG7mt/pIo6RCJufUemoO62AeL1hKQU2UbjHJYxkfv/jf9LaM68dijQWRe7b8xres\n"
          "4sFcEBCmFkbt4YzBCCWjntT1gBrv+Ba4fOXOMxoi374Yy1yzpYRpAWuI4L0CgYAn\n"
          "u1SlXrivuHx2i/tR62pzou2mVhkkRK16LBsczeY57UzWXBZJRbM+UYIOjwU2RWQk\n"
          "JebWTZg9ZspmXlLv5CS0FpDl5BhiqWktXy/cuSKtRq2UYf4cWy3A/0vdSqZdi8Wk\n"
          "3Uc94uaPEK77eVQd/orMtWexzo3NlmLs9uMMv8g/3QKBgQCbik0UoJkkqNRMmWG8\n"
          "dKQzj58eRI8fmKdJlWNfj2QMspd2vXMbsWYgAbFbU1QcVs1n8PxNydM+cfy77w8q\n"
          "NWMlYP7rUFQ3ekYWqrRlshZdJ/h24PALd1nPCvhc4C9dvn+zW3BLVez1lBuFO8n8\n"
          "0YkgmTgW7Ieibqnf4DqYp//nkw==\n"
          "-----END PRIVATE KEY-----\n";
}

peer_id legacy_cert_hash_peer_id(std::string_view certificate_pem) {
   const auto certificate = forge::crypto::pki::x509::certificate::from_pem(certificate_pem);
   const auto der = certificate.der();
   return peer_id::from_bytes(forge::multiformats::multihash::sha2_256(der).encode());
}

peer_id peer(std::uint8_t value) {
   const auto payload = forge::multiformats::bytes{value};
   return peer_id::from_bytes(forge::multiformats::multihash::identity(payload).encode());
}

std::uint8_t hex_value(char value) {
   if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
   }
   if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(10 + value - 'a');
   }
   if (value >= 'A' && value <= 'F') {
      return static_cast<std::uint8_t>(10 + value - 'A');
   }
   throw std::runtime_error{"bad hex"};
}

std::vector<std::uint8_t> bytes_from_hex(std::string_view hex) {
   if ((hex.size() % 2) != 0) {
      throw std::runtime_error{"odd hex"};
   }
   auto out = std::vector<std::uint8_t>{};
   out.reserve(hex.size() / 2);
   for (std::size_t i = 0; i < hex.size(); i += 2) {
      out.push_back(static_cast<std::uint8_t>((hex_value(hex[i]) << 4U) | hex_value(hex[i + 1])));
   }
   return out;
}

void append_der_length(std::vector<std::uint8_t>& out, std::size_t value) {
   if (value < 128) {
      out.push_back(static_cast<std::uint8_t>(value));
      return;
   }
   auto bytes = std::vector<std::uint8_t>{};
   while (value != 0) {
      bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
      value >>= 8U;
   }
   out.push_back(static_cast<std::uint8_t>(0x80U | bytes.size()));
   for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
      out.push_back(*it);
   }
}

void append_der_octet_string(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> value) {
   out.push_back(0x04);
   append_der_length(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> signed_key_der(std::span<const std::uint8_t> public_key,
                                         std::span<const std::uint8_t> signature) {
   auto content = std::vector<std::uint8_t>{};
   append_der_octet_string(content, public_key);
   append_der_octet_string(content, signature);
   auto out = std::vector<std::uint8_t>{0x30};
   append_der_length(out, content.size());
   out.insert(out.end(), content.begin(), content.end());
   return out;
}

std::vector<std::uint8_t> signed_key_der_with_overflowing_octet_length() {
   auto out = std::vector<std::uint8_t>{0x30, 0x0a, 0x04, 0x88};
   out.insert(out.end(), 8, 0xff);
   return out;
}

std::vector<std::uint8_t> certificate_public_key_der(X509* certificate) {
   auto key = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>{X509_get_pubkey(certificate)};
   if (!key) {
      throw std::runtime_error{"failed to get certificate public key"};
   }
   const auto length = i2d_PUBKEY(key.get(), nullptr);
   if (length <= 0) {
      throw std::runtime_error{"failed to size certificate public key DER"};
   }
   auto out = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
   auto* cursor = out.data();
   if (i2d_PUBKEY(key.get(), &cursor) != length) {
      throw std::runtime_error{"failed to write certificate public key DER"};
   }
   return out;
}

template <typename ExtensionFactory>
std::vector<std::uint8_t> make_certificate_der_with_libp2p_extension_from_factory(ExtensionFactory make_extension) {
   auto key = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>{EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519")};
   if (!key) {
      throw std::runtime_error{"failed to generate certificate key"};
   }
   auto certificate = std::unique_ptr<X509, x509_deleter>{X509_new()};
   if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
       ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 77) != 1 ||
       X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) == nullptr ||
       X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 24 * 60 * 60) == nullptr ||
       X509_set_pubkey(certificate.get(), key.get()) != 1) {
      throw std::runtime_error{"failed to configure certificate"};
   }
   auto* name = X509_get_subject_name(certificate.get());
   const auto common_name = std::string_view{"forge-libp2p-identity-test"};
   if (name == nullptr ||
       X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name.data()),
                                  static_cast<int>(common_name.size()), -1, 0) != 1 ||
       X509_set_issuer_name(certificate.get(), name) != 1) {
      throw std::runtime_error{"failed to configure certificate subject"};
   }
   const auto public_key_der = certificate_public_key_der(certificate.get());
   const auto extension_value = make_extension(std::span<const std::uint8_t>{public_key_der});
   auto object = std::unique_ptr<ASN1_OBJECT, asn1_object_deleter>{OBJ_txt2obj("1.3.6.1.4.1.53594.1.1", 1)};
   auto octets = std::unique_ptr<ASN1_OCTET_STRING, asn1_octet_string_deleter>{ASN1_OCTET_STRING_new()};
   if (!object || !octets ||
       ASN1_OCTET_STRING_set(octets.get(), extension_value.data(), static_cast<int>(extension_value.size())) != 1) {
      throw std::runtime_error{"failed to create certificate extension value"};
   }
   auto extension = std::unique_ptr<X509_EXTENSION, x509_extension_deleter>{
       X509_EXTENSION_create_by_OBJ(nullptr, object.get(), 1, octets.get())};
   if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1 ||
       X509_sign(certificate.get(), key.get(), nullptr) <= 0) {
      throw std::runtime_error{"failed to sign certificate"};
   }
   const auto length = i2d_X509(certificate.get(), nullptr);
   if (length <= 0) {
      throw std::runtime_error{"failed to size certificate DER"};
   }
   auto out = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
   auto* cursor = out.data();
   if (i2d_X509(certificate.get(), &cursor) != length) {
      throw std::runtime_error{"failed to write certificate DER"};
   }
   return out;
}

std::vector<std::uint8_t> make_certificate_der_with_libp2p_extension(std::span<const std::uint8_t> extension_value) {
   return make_certificate_der_with_libp2p_extension_from_factory([&](std::span<const std::uint8_t>) {
      return std::vector<std::uint8_t>{extension_value.begin(), extension_value.end()};
   });
}

std::vector<std::uint8_t> tls_identity_message(std::span<const std::uint8_t> certificate_public_key) {
   auto out = std::vector<std::uint8_t>{};
   constexpr auto prefix = std::string_view{"libp2p-tls-handshake:"};
   out.insert(out.end(), prefix.begin(), prefix.end());
   out.insert(out.end(), certificate_public_key.begin(), certificate_public_key.end());
   return out;
}

std::vector<std::uint8_t> sign_test_identity(const test_identity& identity, std::span<const std::uint8_t> message) {
   return identity.private_key.visit([&](const auto& key) -> std::vector<std::uint8_t> {
      using key_type = std::decay_t<decltype(key)>;
      if constexpr (std::is_same_v<key_type, forge::crypto::asymmetric::secp256k1::private_key>) {
         return forge::crypto::asymmetric::secp256k1::sign_der(key, message);
      } else if constexpr (std::is_same_v<key_type, forge::crypto::asymmetric::p256::private_key>) {
         return forge::crypto::asymmetric::p256::sign_der(key, message);
      } else {
         return bytes_from_range(key.sign(message));
      }
   });
}

std::vector<std::uint8_t> signed_tls_extension(const test_identity& identity,
                                               std::span<const std::uint8_t> certificate_public_key) {
   const auto message = tls_identity_message(certificate_public_key);
   const auto signature = sign_test_identity(identity, message);
   return signed_key_der(encode_public_key(identity.key), signature);
}

const auto content_swarm_test_dht = protocol_id{.value = "/forge/test/content-swarm/kad/1.0.0"};
const auto content_swarm_value_test_dht = protocol_id{.value = "/forge/test/content-swarm/value-kad/1.0.0"};

[[nodiscard]] dht::profile custom_test_dht_profile(dht::mode operating_mode = dht::mode::client,
                                                   dht::options limits = {}) {
   return custom_dht_profile(content_swarm_test_dht, operating_mode,
                             dht::profile_capabilities{.peers = true, .providers = true, .values = false}, {},
                             std::move(limits));
}

[[nodiscard]] dht::profile custom_test_value_dht_profile(dht::mode operating_mode = dht::mode::client,
                                                         dht::options limits = {}) {
   return custom_dht_profile(
       content_swarm_value_test_dht, operating_mode,
       dht::profile_capabilities{.peers = true, .providers = false, .values = true},
       {dht::value_policy{
           .key_prefix = {'/'},
           .validate =
               [](const dht::record& value, dht::value_validation_context) {
                  if (value.value.empty()) {
                     FORGE_THROW_CODE(exceptions::code::record_rejected, "empty test DHT value");
                  }
               },
           .select = [](std::span<const dht::record>) { return std::size_t{}; },
           .expiry = [](const dht::record&, dht::value_expiry_context context) { return context.supplied_expires_at; },
       }},
       std::move(limits));
}

[[nodiscard]] dht::key amino_provider_key(std::span<const std::uint8_t> value) {
   const auto multihash = forge::multiformats::multihash::sha2_256(value).encode();
   return make_dht_key(multihash);
}

node::options options_for(peer_id id, capability_set capabilities = capability_set{
                                          .bits = capabilities::direct_quic | capabilities::peer_exchange}) {
   return node::options{
       .certificate_pem = std::string{test_certificate()},
       .private_key_pem = std::string{test_private_key()},
       .explicit_peer_id = std::move(id),
       .capabilities = capabilities,
       .allow_insecure_test_mode = true,
   };
}

[[nodiscard]] std::size_t active_stream_count(const resource_manager::snapshot& snapshot) {
   return snapshot.streams.inbound_streams + snapshot.streams.outbound_streams;
}

[[nodiscard]] bool bind_matches(resource_manager::stream_reservation::bind_result actual,
                                resource_manager::stream_reservation::bind_result expected) noexcept {
   return actual == expected;
}

[[nodiscard]] bool transition_matches(resource_manager::transition_result actual,
                                      resource_manager::transition_result expected) noexcept {
   return actual == expected;
}

enum class session_admission_headroom {
   none,
   one_session,
};

void set_resource_session_limits(node::options& options, std::size_t total, std::size_t inbound, std::size_t outbound,
                                 std::size_t per_peer,
                                 session_admission_headroom headroom = session_admission_headroom::none) {
   options.limits.max_sessions = total;
   const auto admission_limit = [headroom](std::size_t value) {
      return headroom == session_admission_headroom::one_session && value != (std::numeric_limits<std::size_t>::max)()
                 ? value + 1
                 : value;
   };
   auto& system = options.limits.resources.system;
   system.max_connections = admission_limit(total);
   system.max_inbound_connections = admission_limit(inbound);
   system.max_outbound_connections = admission_limit(outbound);
   auto& peer = options.limits.resources.peer;
   peer.max_connections = per_peer;
   peer.max_inbound_connections = per_peer;
   peer.max_outbound_connections = per_peer;
}

enum class recorded_gater_stage {
   peer_dial,
   address_dial,
   accept,
   secured,
   upgraded,
};

struct recorded_gater_call {
   recorded_gater_stage stage;
   std::optional<peer_id> peer;
   std::optional<connection_endpoints> endpoints;
};

class recording_connection_gater final : public connection_gater {
 public:
   explicit recording_connection_gater(std::optional<recorded_gater_stage> rejected = {},
                                       std::optional<peer_id> rejected_peer = {})
       : rejected_(rejected), rejected_peer_(std::move(rejected_peer)) {}

   [[nodiscard]] std::vector<recorded_gater_stage> stages() const {
      auto lock = std::scoped_lock{mutex_};
      auto result = std::vector<recorded_gater_stage>{};
      result.reserve(calls_.size());
      for (const auto& call : calls_) {
         result.push_back(call.stage);
      }
      return result;
   }

   [[nodiscard]] std::vector<recorded_gater_stage> stages_for(const peer_id& peer) const {
      auto lock = std::scoped_lock{mutex_};
      auto result = std::vector<recorded_gater_stage>{};
      for (const auto& call : calls_) {
         if (call.peer == peer) {
            result.push_back(call.stage);
         }
      }
      return result;
   }

   [[nodiscard]] std::vector<recorded_gater_call> calls() const {
      auto lock = std::scoped_lock{mutex_};
      return calls_;
   }

   void clear() {
      auto lock = std::scoped_lock{mutex_};
      calls_.clear();
   }

   void allow() {
      auto lock = std::scoped_lock{mutex_};
      rejected_.reset();
   }

   bool intercept_peer_dial(const peer_id& peer) noexcept override {
      return record(recorded_gater_stage::peer_dial, peer);
   }

   bool intercept_address_dial(const peer_id& peer, const endpoint& address) noexcept override {
      return record(recorded_gater_stage::address_dial, peer, connection_endpoints{.local = {}, .remote = address});
   }

   bool intercept_accept(const connection_endpoints& endpoints) noexcept override {
      return record(recorded_gater_stage::accept, {}, endpoints);
   }

   bool intercept_secured(connection_direction, const peer_id& peer,
                          const connection_endpoints& endpoints) noexcept override {
      return record(recorded_gater_stage::secured, peer, endpoints);
   }

   bool intercept_upgraded(connection_direction, const peer_id& peer,
                           const connection_endpoints& endpoints) noexcept override {
      return record(recorded_gater_stage::upgraded, peer, endpoints);
   }

 private:
   [[nodiscard]] bool record(recorded_gater_stage stage, std::optional<peer_id> peer = {},
                             std::optional<connection_endpoints> endpoints = {}) noexcept {
      auto lock = std::scoped_lock{mutex_};
      const auto rejected = rejected_ == stage && (!rejected_peer_ || peer == rejected_peer_);
      calls_.push_back(recorded_gater_call{.stage = stage, .peer = std::move(peer), .endpoints = std::move(endpoints)});
      return !rejected;
   }

   std::optional<recorded_gater_stage> rejected_;
   std::optional<peer_id> rejected_peer_;
   mutable std::mutex mutex_;
   std::vector<recorded_gater_call> calls_;
};

[[nodiscard]] std::uint64_t gater_rejections(const node::metrics_snapshot& metrics, recorded_gater_stage stage) {
   switch (stage) {
   case recorded_gater_stage::peer_dial:
      return metrics.gater_peer_dial_rejections;
   case recorded_gater_stage::address_dial:
      return metrics.gater_address_dial_rejections;
   case recorded_gater_stage::accept:
      return metrics.gater_accept_rejections;
   case recorded_gater_stage::secured:
      return metrics.gater_secured_rejections;
   case recorded_gater_stage::upgraded:
      return metrics.gater_upgraded_rejections;
   }
   return 0;
}

[[nodiscard]] std::size_t inbound_gater_stage_count(recorded_gater_stage stage) {
   switch (stage) {
   case recorded_gater_stage::accept:
      return 1;
   case recorded_gater_stage::secured:
      return 2;
   case recorded_gater_stage::upgraded:
      return 3;
   case recorded_gater_stage::peer_dial:
   case recorded_gater_stage::address_dial:
      return 0;
   }
   return 0;
}

void check_gater_stages(const std::vector<recorded_gater_stage>& actual,
                        std::initializer_list<recorded_gater_stage> expected) {
   BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
   auto index = std::size_t{};
   for (const auto stage : expected) {
      BOOST_TEST(static_cast<int>(actual[index]) == static_cast<int>(stage));
      ++index;
   }
}

node::options options_for(const test_certificate_identity& identity,
                          capability_set capabilities = capability_set{.bits = capabilities::direct_quic |
                                                                               capabilities::peer_exchange}) {
   return node::options{
       .certificate_pem = identity.certificate_pem,
       .private_key_pem = identity.private_key_pem,
       .capabilities = capabilities,
       .allow_insecure_test_mode = true,
   };
}

node::options options_for(const test_identity& identity,
                          capability_set capabilities = capability_set{.bits = capabilities::direct_quic |
                                                                               capabilities::peer_exchange}) {
   auto out = options_for(identity.peer, capabilities);
   out.private_key_pem = identity.private_key_pem;
   out.public_key = encode_public_key(identity.key);
   return out;
}

[[nodiscard]] std::shared_ptr<const forge::net::pnet::protector>
private_network_protector(std::uint8_t first_byte = 0) {
   auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
   for (auto index = std::size_t{}; index < key.size(); ++index) {
      key[index] = static_cast<std::uint8_t>(first_byte + index);
   }
   return std::make_shared<const forge::net::pnet::protector>(
       forge::net::pnet::protector{forge::net::pnet::pre_shared_key{key}});
}

[[nodiscard]] node::options private_network_options_for(const test_identity& identity,
                                                        std::uint8_t first_key_byte = 0) {
   auto out = options_for(identity, capability_set{.bits = capabilities::peer_exchange});
   out.relay_policy.service_enabled = false;
   out.relay_policy.client_enabled = false;
   out.relay_policy.auto_discovery_enabled = false;
   out.path_policy.allow_direct = true;
   out.path_policy.allow_relay = false;
   out.path_policy.allow_hole_punch = false;
   out.private_network = private_network::options{.protector = private_network_protector(first_key_byte)};
   return out;
}

node::options dht_options_for(peer_id id, dht::profile profile,
                              capability_set capabilities = capability_set{.bits = capabilities::direct_quic}) {
   auto out = options_for(std::move(id), capabilities);
   out.dht_profiles.push_back(std::move(profile));
   return out;
}

node::options dht_options_for(const test_certificate_identity& identity, dht::profile profile,
                              capability_set capabilities = capability_set{.bits = capabilities::direct_quic}) {
   auto out = options_for(identity, capabilities);
   out.dht_profiles.push_back(std::move(profile));
   return out;
}

node::options dht_options_for(const test_identity& identity, dht::profile profile,
                              capability_set capabilities = capability_set{.bits = capabilities::direct_quic}) {
   auto out = options_for(identity, capabilities);
   out.dht_profiles.push_back(std::move(profile));
   return out;
}

public_key test_rsa_public_key() {
   return public_key{
       .type = public_key::type::rsa,
       .data = forge::crypto::pki::der::write_public_key(
           forge::crypto::pki::pem::read_private_key(test_private_key()).get_public_key()),
   };
}

node::options pubsub_options_for(capability_set capabilities = capability_set{.bits = capabilities::direct_quic |
                                                                                      capabilities::pubsub}) {
   const auto key = test_rsa_public_key();
   auto out = options_for(make_peer_id(key), capabilities);
   out.public_key = encode_public_key(key);
   return out;
}

node::options pubsub_options_for(const test_certificate_identity& identity,
                                 capability_set capabilities = capability_set{.bits = capabilities::direct_quic |
                                                                                      capabilities::pubsub}) {
   return options_for(identity, capabilities);
}

void register_echo(node& value) {
   value.register_protocol_handler(builtins::echo,
                                   [](node::incoming_protocol_stream incoming) mutable -> boost::asio::awaitable<void> {
                                      auto payload = co_await incoming.stream.async_read_frame();
                                      co_await incoming.stream.async_write_frame(payload);
                                      co_await incoming.stream.async_close();
                                   });
}

void register_echo(node& value, protocol_id protocol) {
   value.register_protocol_handler(std::move(protocol),
                                   [](node::incoming_protocol_stream incoming) mutable -> boost::asio::awaitable<void> {
                                      auto payload = co_await incoming.stream.async_read_frame();
                                      co_await incoming.stream.async_write_frame(payload);
                                      co_await incoming.stream.async_close();
                                   });
}

[[nodiscard]] endpoint make_quic_endpoint(std::uint16_t port, std::string host = "127.0.0.1") {
   return endpoint{.transport = {.host_type = endpoint::host_kind::ip4,
                                 .protocol = endpoint::protocol_kind::quic_v1,
                                 .host = std::move(host),
                                 .port = port}};
}

[[nodiscard]] endpoint make_tcp_endpoint(std::uint16_t port, std::string host = "127.0.0.1") {
   return endpoint{.transport = {.host_type = endpoint::host_kind::ip4,
                                 .protocol = endpoint::protocol_kind::tcp,
                                 .host = std::move(host),
                                 .port = port}};
}

[[nodiscard]] endpoint make_dns_tcp_endpoint(std::uint16_t port, std::string host) {
   return endpoint{.transport = {.host_type = endpoint::host_kind::dns,
                                 .protocol = endpoint::protocol_kind::tcp,
                                 .host = std::move(host),
                                 .port = port}};
}

endpoint listen(node& value, forge::asio::runtime& runtime) {
   forge::asio::blocking::run(runtime, value.async_listen(make_quic_endpoint(0)));
   auto endpoint = value.local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   return *endpoint;
}

std::optional<forge::tests::dns::bytes> hidden_dns_response(const std::uint8_t* request, std::size_t size) {
   const auto question = forge::tests::dns::parse_question(request, size);
   if (!question) {
      return std::nullopt;
   }
   if (question->name != "hidden.test") {
      return forge::tests::dns::make_failure_response(request, *question, 3);
   }
   const auto answers = question->type == 1
                            ? std::vector<forge::tests::dns::response_answer>{{.type = 1, .value = {127, 0, 0, 1}}}
                            : std::vector<forge::tests::dns::response_answer>{};
   return forge::tests::dns::make_response(request, *question, answers);
}

endpoint listen_quic_with_advertised_dns4(node& value, forge::asio::runtime& runtime) {
   forge::asio::blocking::run(runtime, value.async_listen(make_quic_endpoint(0, "0.0.0.0")));
   const auto local_endpoint = value.local_endpoint();
   BOOST_REQUIRE(local_endpoint.has_value());

   auto advertised = endpoint{.transport = {.host_type = endpoint::host_kind::dns4,
                                            .protocol = endpoint::protocol_kind::quic_v1,
                                            .host = "hidden.test",
                                            .port = local_endpoint->transport.port}};
   advertised.peer = value.local_peer();
   value.set_advertised_endpoints({advertised});
   return advertised;
}

endpoint listen_tcp(node& value, forge::asio::runtime& runtime) {
   forge::asio::blocking::run(runtime, value.async_listen(make_tcp_endpoint(0)));
   auto endpoint = value.local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   return *endpoint;
}

void wait_on_runtime(forge::asio::runtime& runtime, std::chrono::milliseconds delay, std::string_view label);

void verify_dht_server(forge::asio::runtime& runtime, node& client, const node& server, const endpoint& server_endpoint,
                       const protocol_id& protocol, const std::optional<endpoint>& advertised_endpoint = std::nullopt) {
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   if (advertised_endpoint) {
      client.peers().learn_endpoint(server.local_peer(), *advertised_endpoint,
                                    capability_set{.bits = capabilities::direct_quic});
   }
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   for (auto attempt = 0U; attempt < 20U && client.routing_status(protocol).active == 0U; ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "DHT Identify admission");
   }
   if (client.routing_status(protocol).active == 0U) {
      if (const auto record = client.peers().find(server.local_peer())) {
         BOOST_TEST_MESSAGE("DHT peer record protocols=" << record->protocols.size());
         for (const auto& current : record->protocols) {
            BOOST_TEST_MESSAGE("DHT peer protocol=" << current.value);
         }
      }
      for (const auto& session : client.diagnostics().sessions) {
         BOOST_TEST_MESSAGE("DHT session identify_state=" << static_cast<int>(session.identify_state)
                                                          << " error=" << session.identify_error);
      }
   }
   BOOST_REQUIRE(client.routing_status(protocol).active > 0U);
}

[[nodiscard]] bool has_identified_session(const node& value, const peer_id& remote) {
   return std::ranges::any_of(value.diagnostics().sessions, [&](const auto& session) {
      return !session.closed && session.remote_peer == remote && session.identify_state == identify::state::identified;
   });
}

boost::asio::awaitable<void> exercise_live_api(forge::api::transport::connection& connection) {
   auto remote = co_await connection.get_remote_api<live_api>();
   auto call = co_await remote.async_open<&live_api::exchange>(
       p2p_live_types::caller_peer{.id = peer(244)},
       forge::api::auth::authenticated_caller{
           forge::api::auth::caller_source::tls_certificate,
           forge::crypto::digest::sha256::hash(std::string{"spoofed-p2p-caller"}),
       });
   co_await call.async_write(3U);
   auto first = co_await call.async_read();
   BOOST_REQUIRE(first.has_value());
   BOOST_TEST(*first == 6U);
   co_await call.async_write(5U);
   auto second = co_await call.async_read();
   BOOST_REQUIRE(second.has_value());
   BOOST_TEST(*second == 10U);
   co_await call.async_close();
   BOOST_TEST(!(co_await call.async_read()).has_value());
   co_await call.async_finish();
   co_await connection.async_close();
}

boost::asio::awaitable<void> exercise_unverified_live_api(forge::api::transport::connection& connection) {
   auto remote = co_await connection.get_remote_api<live_api>();
   auto call = co_await remote.async_open<&live_api::exchange>(
       p2p_live_types::caller_peer{.id = peer(244)},
       forge::api::auth::authenticated_caller{
           forge::api::auth::caller_source::tls_certificate,
           forge::crypto::digest::sha256::hash(std::string{"spoofed-unverified-caller"}),
       });
   auto rejected = false;
   try {
      co_await call.async_finish();
   } catch (const forge::api::core::exceptions::server_supplied_unavailable&) {
      rejected = true;
   }
   BOOST_TEST(rejected);
   co_await connection.async_close();
}

void run_live_api_over(endpoint::protocol_kind transport) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = transport == endpoint::protocol_kind::quic_v1
                             ? options_for(make_test_certificate_identity("api-live-server"))
                             : options_for(make_test_identity());
   auto client_options = transport == endpoint::protocol_kind::quic_v1
                             ? options_for(make_test_certificate_identity("api-live-client"))
                             : options_for(make_test_identity());
   if (transport == endpoint::protocol_kind::quic_v1) {
      server_options.allow_insecure_test_mode = false;
      client_options.allow_insecure_test_mode = false;
      server_options.peer_state.persistence = peer_store::make_memory_persistence();
      client_options.peer_state.persistence = peer_store::make_memory_persistence();
   }
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   auto implementation = std::make_shared<live_impl>();
   auto registry = forge::api::core::registry{};
   registry.install<live_api>(live_api::describe(), implementation);
   auto binding = forge::api::p2p::api(server).use(forge::api::core::binding().serve(registry).build()).build();
   BOOST_TEST(binding.protocol().value == "/forge/api/2");
   server.register_protocol_handler(binding.protocol(), binding.handler());

   const auto server_endpoint =
       transport == endpoint::protocol_kind::quic_v1 ? listen(server, runtime) : listen_tcp(server, runtime);
   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{
                                                          .expected_peer = server.local_peer(),
                                                          .allow_relay = false,
                                                      }));
   BOOST_TEST(session.remote_peer.value == server.local_peer().value);
   const auto expected_authentication =
       transport == endpoint::protocol_kind::quic_v1 ? peer_authentication::quic_tls : peer_authentication::libp2p_tls;
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), binding.protocol(),
                                                                             node::open_options{.allow_relay = false}));
   BOOST_TEST(static_cast<std::uint8_t>(stream.authentication()) == static_cast<std::uint8_t>(expected_authentication));
   auto connection = forge::api::transport::connection{std::move(stream).into_transport_stream(), binding.options()};
   forge::asio::blocking::run(runtime, exercise_live_api(connection));
   BOOST_TEST(implementation->caller.value == client.local_peer().value);
   BOOST_TEST(implementation->transport_caller.transport_authenticated());
   BOOST_CHECK(implementation->transport_caller.source == forge::api::auth::caller_source::p2p_peer);
   BOOST_TEST(implementation->transport_caller.fingerprint ==
              forge::crypto::digest::sha256::hash(client.local_peer().to_bytes()));

   const auto diagnostics = client.diagnostics();
   const auto found = std::ranges::find_if(diagnostics.sessions, [&](const auto& value) {
      return value.remote_peer == server.local_peer() && value.direct_endpoint.has_value();
   });
   BOOST_REQUIRE(found != diagnostics.sessions.end());
   BOOST_TEST(static_cast<int>(found->direct_endpoint->transport.protocol) == static_cast<int>(transport));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

[[nodiscard]] bool contains_protocol(const std::vector<endpoint>& endpoints, endpoint::protocol_kind protocol) {
   return std::ranges::any_of(endpoints,
                              [protocol](const endpoint& value) { return value.transport.protocol == protocol; });
}

[[nodiscard]] bool contains_protocol(const std::vector<forge::multiformats::multiaddr>& addresses,
                                     endpoint::protocol_kind protocol) {
   return std::ranges::any_of(addresses, [protocol](const auto& address) {
      return parse_endpoint(address.to_string()).transport.protocol == protocol;
   });
}

[[nodiscard]] endpoint require_endpoint_for(const std::vector<endpoint>& endpoints, endpoint::protocol_kind protocol) {
   auto found = std::ranges::find_if(
       endpoints, [protocol](const endpoint& value) { return value.transport.protocol == protocol; });
   BOOST_REQUIRE(found != endpoints.end());
   return *found;
}

[[nodiscard]] peer_id peer_for(const forge::multiformats::multiaddr& address) {
   const auto& components = address.components();
   const auto component = std::ranges::find_if(components.rbegin(), components.rend(), [](const auto& value) {
      return value.code == forge::multiformats::protocol_code::p2p;
   });
   BOOST_REQUIRE(component != components.rend());
   return peer_id::from_string(component->value);
}

[[nodiscard]] std::uint16_t transport_port_for(const forge::multiformats::multiaddr& address) {
   return parse_endpoint(address.to_string()).transport.port;
}

endpoint start_stalling_tcp_peer(forge::asio::runtime& runtime,
                                 std::chrono::milliseconds hold = std::chrono::seconds{2},
                                 std::shared_ptr<std::promise<void>> accepted = {},
                                 std::shared_ptr<std::atomic_size_t> accepted_count = {}) {
   namespace asio = boost::asio;
   using asio_tcp = asio::ip::tcp;
   auto acceptor = std::make_shared<asio_tcp::acceptor>(runtime.context(), asio_tcp::endpoint{asio_tcp::v4(), 0});
   auto socket = std::make_shared<asio_tcp::socket>(runtime.context());
   const auto port = acceptor->local_endpoint().port();

   asio::co_spawn(
       runtime.context(),
       [acceptor, socket, hold, accepted = std::move(accepted), accepted_count = std::move(accepted_count)]()
           -> asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await acceptor->async_accept(*socket, asio::redirect_error(asio::use_awaitable, error));
          if (!error) {
             if (accepted_count) {
                accepted_count->fetch_add(1U, std::memory_order_relaxed);
             }
             if (accepted) {
                accepted->set_value();
             }
             const auto executor = co_await asio::this_coro::executor;
             if (accepted_count) {
                auto stop_accepting = std::make_shared<asio::steady_timer>(executor);
                stop_accepting->expires_after(hold);
                asio::co_spawn(
                    executor,
                    [acceptor, stop_accepting]() -> asio::awaitable<void> {
                       auto wait_error = boost::system::error_code{};
                       co_await stop_accepting->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
                       auto ignored = boost::system::error_code{};
                       acceptor->cancel(ignored);
                    },
                    asio::detached);
                auto retained = std::vector<std::shared_ptr<asio_tcp::socket>>{socket};
                while (true) {
                   auto additional = std::make_shared<asio_tcp::socket>(acceptor->get_executor());
                   co_await acceptor->async_accept(*additional, asio::redirect_error(asio::use_awaitable, error));
                   if (error) {
                      break;
                   }
                   accepted_count->fetch_add(1U, std::memory_order_relaxed);
                   retained.push_back(std::move(additional));
                }
                for (const auto& value : retained) {
                   auto ignored = boost::system::error_code{};
                   value->close(ignored);
                }
             } else {
                auto timer = asio::steady_timer{executor};
                timer.expires_after(hold);
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
             }
          }
          auto ignored = boost::system::error_code{};
          socket->close(ignored);
          acceptor->close(ignored);
       },
       asio::detached);

   return make_tcp_endpoint(port);
}

endpoint start_pnet_nonce_rejecting_tcp_peer(forge::asio::runtime& runtime, std::size_t returned_nonce_bytes) {
   namespace asio = boost::asio;
   using asio_tcp = asio::ip::tcp;
   auto acceptor = std::make_shared<asio_tcp::acceptor>(runtime.context(), asio_tcp::endpoint{asio_tcp::v4(), 0});
   auto socket = std::make_shared<asio_tcp::socket>(runtime.context());
   const auto port = acceptor->local_endpoint().port();

   asio::co_spawn(
       runtime.context(),
       [acceptor, socket, returned_nonce_bytes]() -> asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await acceptor->async_accept(*socket, asio::redirect_error(asio::use_awaitable, error));
          if (!error) {
             auto local_nonce = std::array<std::uint8_t, forge::crypto::symmetric::xsalsa20::nonce_size>{};
             co_await asio::async_read(*socket, asio::buffer(local_nonce),
                                       asio::redirect_error(asio::use_awaitable, error));
             if (!error && returned_nonce_bytes != 0) {
                auto peer_nonce = std::array<std::uint8_t, forge::crypto::symmetric::xsalsa20::nonce_size>{};
                co_await asio::async_write(*socket, asio::buffer(peer_nonce.data(), returned_nonce_bytes),
                                           asio::redirect_error(asio::use_awaitable, error));
             }
          }
          auto ignored = boost::system::error_code{};
          socket->shutdown(asio_tcp::socket::shutdown_both, ignored);
          socket->close(ignored);
          acceptor->close(ignored);
       },
       asio::detached);

   return make_tcp_endpoint(port);
}

std::pair<endpoint, std::future<void>>
start_pnet_security_closing_tcp_peer(forge::asio::runtime& runtime, protocol_id security_protocol) {
   auto listener = std::make_shared<forge::net::tcp::listener>(runtime.context().get_executor(),
                                                               make_tcp_endpoint(0).transport);
   auto protector = private_network_protector();
   auto completed = std::make_shared<std::promise<void>>();
   auto future = completed->get_future();
   auto local = endpoint{.transport = listener->local_endpoint()};

   boost::asio::co_spawn(
       runtime.context(),
       [listener, protector, security_protocol = std::move(security_protocol)]() -> boost::asio::awaitable<void> {
          auto connection = co_await listener->async_accept();
          auto protected_connection = co_await protector->async_protect(std::move(connection));
          auto protected_stream = forge::net::p2p::stream{std::move(protected_connection.stream)};
          auto selected =
              co_await protocol_negotiation::async_accept(std::move(protected_stream), {security_protocol});
          if (selected.protocol != security_protocol) {
             throw std::runtime_error{"PNET closing peer selected an unexpected security protocol"};
          }
          co_await listener->async_close();
          co_await selected.stream.async_close();
       },
       [completed](std::exception_ptr error) {
          if (error) {
             completed->set_exception(std::move(error));
          } else {
             completed->set_value();
          }
       });

   return {std::move(local), std::move(future)};
}

boost::asio::awaitable<std::vector<std::uint8_t>> read_raw_multistream_frame(boost::asio::ip::tcp::socket& socket) {
   auto size = std::uint64_t{};
   auto shift = unsigned{};
   while (shift < 64U) {
      auto byte = std::array<std::uint8_t, 1>{};
      co_await boost::asio::async_read(socket, boost::asio::buffer(byte), boost::asio::use_awaitable);
      size |= static_cast<std::uint64_t>(byte.front() & 0x7fU) << shift;
      if ((byte.front() & 0x80U) == 0) {
         if (size > 16U * 1024U) {
            throw std::runtime_error{"raw multistream frame is too large"};
         }
         auto payload = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
         co_await boost::asio::async_read(socket, boost::asio::buffer(payload), boost::asio::use_awaitable);
         co_return payload;
      }
      shift += 7U;
   }
   throw std::runtime_error{"raw multistream frame has an invalid length"};
}

boost::asio::awaitable<void> write_raw_multistream_message(boost::asio::ip::tcp::socket& socket,
                                                           const protocol_negotiation::message& message) {
   const auto frame = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(message));
   co_await boost::asio::async_write(socket, boost::asio::buffer(frame), boost::asio::use_awaitable);
}

endpoint start_stalling_security_tcp_peer(forge::asio::runtime& runtime, protocol_id selected_protocol,
                                          const std::shared_ptr<std::promise<void>>& security_started) {
   namespace asio = boost::asio;
   using asio_tcp = asio::ip::tcp;
   auto acceptor = std::make_shared<asio_tcp::acceptor>(runtime.context(), asio_tcp::endpoint{asio_tcp::v4(), 0});
   auto socket = std::make_shared<asio_tcp::socket>(runtime.context());
   const auto port = acceptor->local_endpoint().port();

   asio::co_spawn(
       runtime.context(),
       [acceptor, socket, selected_protocol = std::move(selected_protocol),
        security_started]() -> asio::awaitable<void> {
          auto reported = false;
          try {
             co_await acceptor->async_accept(*socket, asio::use_awaitable);
             auto ignored = boost::system::error_code{};
             acceptor->close(ignored);

             const auto header = protocol_negotiation::decode_message(co_await read_raw_multistream_frame(*socket));
             if (header.kind != protocol_negotiation::message_kind::header) {
                throw std::runtime_error{"raw peer expected multistream header"};
             }
             co_await write_raw_multistream_message(
                 *socket, protocol_negotiation::message{.kind = protocol_negotiation::message_kind::header,
                                                        .protocol = protocol_negotiation::multistream_v1});

             while (true) {
                const auto proposal =
                    protocol_negotiation::decode_message(co_await read_raw_multistream_frame(*socket));
                if (proposal.kind != protocol_negotiation::message_kind::protocol) {
                   throw std::runtime_error{"raw peer expected security proposal"};
                }
                if (proposal.protocol.value == selected_protocol.value) {
                   co_await write_raw_multistream_message(*socket, proposal);
                   break;
                }
                co_await write_raw_multistream_message(
                    *socket, protocol_negotiation::message{.kind = protocol_negotiation::message_kind::not_available,
                                                           .protocol = protocol_negotiation::not_available});
             }

             auto byte = std::array<std::uint8_t, 1>{};
             co_await asio::async_read(*socket, asio::buffer(byte), asio::use_awaitable);
             security_started->set_value();
             reported = true;
             while (co_await socket->async_read_some(asio::buffer(byte), asio::use_awaitable)) {
             }
          } catch (...) {
             if (!reported) {
                security_started->set_exception(std::current_exception());
             }
          }
          auto ignored = boost::system::error_code{};
          socket->close(ignored);
          acceptor->close(ignored);
       },
       asio::detached);

   return make_tcp_endpoint(port);
}

endpoint start_stalling_tls_yamux_tcp_peer(forge::asio::runtime& runtime, const node::options& identity_options,
                                           const std::shared_ptr<std::promise<void>>& yamux_started,
                                           const std::shared_ptr<std::promise<void>>& peer_finished) {
   namespace asio = boost::asio;
   using asio_tcp = asio::ip::tcp;
   auto acceptor = std::make_shared<asio_tcp::acceptor>(runtime.context(), asio_tcp::endpoint{asio_tcp::v4(), 0});
   auto socket = std::make_shared<asio_tcp::socket>(runtime.context());
   auto identity = std::make_shared<libp2p_identity_material>(make_libp2p_identity_material(identity_options));
   const auto port = acceptor->local_endpoint().port();

   asio::co_spawn(
       runtime.context(),
       [acceptor, socket, identity = std::move(identity), yamux_started, peer_finished]() -> asio::awaitable<void> {
          auto reported_yamux = false;
          try {
             co_await acceptor->async_accept(*socket, asio::use_awaitable);
             auto ignored = boost::system::error_code{};
             acceptor->close(ignored);

             const auto header = protocol_negotiation::decode_message(co_await read_raw_multistream_frame(*socket));
             if (header.kind != protocol_negotiation::message_kind::header) {
                throw std::runtime_error{"TLS Yamux peer expected multistream header"};
             }
             co_await write_raw_multistream_message(
                 *socket, protocol_negotiation::message{.kind = protocol_negotiation::message_kind::header,
                                                        .protocol = protocol_negotiation::multistream_v1});

             const auto tls_protocol = protocol_id{.value = "/tls/1.0.0"};
             while (true) {
                const auto proposal =
                    protocol_negotiation::decode_message(co_await read_raw_multistream_frame(*socket));
                if (proposal.kind != protocol_negotiation::message_kind::protocol) {
                   throw std::runtime_error{"TLS Yamux peer expected security proposal"};
                }
                if (proposal.protocol.value == tls_protocol.value) {
                   co_await write_raw_multistream_message(*socket, proposal);
                   break;
                }
                co_await write_raw_multistream_message(
                    *socket, protocol_negotiation::message{.kind = protocol_negotiation::message_kind::not_available,
                                                           .protocol = protocol_negotiation::not_available});
             }

             auto tls_options = make_libp2p_tls_server_options(*identity);
             tls_options.alpn_protocols = {"libp2p"};
             auto tls = co_await forge::net::stcp::async_upgrade_server(
                 forge::net::tcp::connection{std::move(*socket)}, std::move(tls_options), std::chrono::seconds{2});
             if (tls.selected_alpn() != "libp2p") {
                throw std::runtime_error{"TLS Yamux peer did not negotiate legacy libp2p ALPN"};
             }

             auto first_yamux_byte = std::array<std::uint8_t, 1>{};
             if (co_await tls.async_read_some(first_yamux_byte) != first_yamux_byte.size()) {
                throw std::runtime_error{"TLS Yamux peer did not receive Yamux negotiation"};
             }
             yamux_started->set_value();
             reported_yamux = true;
             while (true) {
                static_cast<void>(co_await tls.async_read());
             }
          } catch (...) {
             if (!reported_yamux) {
                yamux_started->set_exception(std::current_exception());
             }
          }
          auto ignored = boost::system::error_code{};
          socket->close(ignored);
          acceptor->close(ignored);
          peer_finished->set_value();
       },
       asio::detached);

   return make_tcp_endpoint(port);
}

void wait_for_server(std::future<void>& future, std::chrono::milliseconds timeout, std::string_view label) {
   if (future.wait_for(timeout) != std::future_status::ready) {
      throw std::runtime_error{std::string{label} + " did not finish"};
   }
   future.get();
}

void wait_on_runtime(forge::asio::runtime& runtime, std::chrono::milliseconds delay, std::string_view label) {
   auto future = boost::asio::co_spawn(
       runtime.context(),
       [delay]() -> boost::asio::awaitable<void> {
          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
          timer.expires_after(delay);
          boost::system::error_code ec;
          co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
       },
       boost::asio::use_future);
   wait_for_server(future, delay + std::chrono::milliseconds{1'000}, label);
}

template <typename Predicate>
peer_store::record wait_for_peer_record(node& value, const peer_id& peer, forge::asio::runtime& runtime,
                                        std::string_view label, Predicate predicate) {
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (std::chrono::steady_clock::now() < deadline) {
      auto record = value.peers().find(peer);
      if (record && predicate(*record)) {
         return *record;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, label);
   }
   throw std::runtime_error{std::string{label} + " did not produce the expected peer record"};
}

peer_store::record wait_for_identified_peer(node& value, const peer_id& peer, forge::asio::runtime& runtime,
                                            std::string_view label) {
   return wait_for_peer_record(value, peer, runtime, label,
                               [](const auto& record) { return !record.signed_peer_record.empty(); });
}

std::shared_ptr<std::promise<void>> block_runtime(forge::asio::runtime& runtime, std::string_view label) {
   auto entered = std::make_shared<std::promise<void>>();
   auto entered_future = entered->get_future();
   auto release = std::make_shared<std::promise<void>>();
   auto released = release->get_future().share();
   boost::asio::post(runtime.context(), [entered, released = std::move(released)] {
      entered->set_value();
      released.wait();
   });
   wait_for_server(entered_future, std::chrono::seconds{2}, label);
   return release;
}

std::vector<std::uint8_t> wrap_length_delimited(std::span<const std::uint8_t> payload) {
   auto out = forge::multiformats::varint_encode(payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

std::vector<std::uint8_t> unwrap_length_delimited(std::span<const std::uint8_t> bytes, std::size_t max_payload_size) {
   const auto decoded = forge::multiformats::varint_decode(bytes);
   BOOST_REQUIRE(decoded.value <= max_payload_size);
   const auto total = decoded.size + static_cast<std::size_t>(decoded.value);
   BOOST_REQUIRE_EQUAL(total, bytes.size());
   return {bytes.begin() + static_cast<std::ptrdiff_t>(decoded.size), bytes.end()};
}

boost::asio::awaitable<std::vector<std::uint8_t>>
read_length_delimited(stream& value, std::size_t max_payload_size = 4 * 1024 * 1024) {
   auto buffer = std::vector<std::uint8_t>{};
   while (true) {
      try {
         const auto decoded = forge::multiformats::varint_decode(buffer);
         BOOST_REQUIRE(decoded.value <= max_payload_size);
         const auto total = decoded.size + static_cast<std::size_t>(decoded.value);
         if (buffer.size() >= total) {
            auto frame = std::vector<std::uint8_t>{buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(total)};
            co_return unwrap_length_delimited(frame, max_payload_size);
         }
      } catch (const forge::multiformats::exceptions::invalid_format& error) {
         if (std::string_view{error.what()}.find("unterminated") == std::string_view::npos) {
            throw;
         }
      }
      auto chunk = co_await value.async_read();
      buffer.insert(buffer.end(), chunk.begin(), chunk.end());
   }
}

dht::message exchange_find_node(forge::asio::runtime& runtime, node& client, const peer_id& server,
                                const dht::profile& profile, dht::key key) {
   auto stream = forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server, profile.protocol));
   forge::asio::blocking::run(runtime, stream.async_write(dht::codec::encode(
                                           dht::message{
                                               .type = dht::message_type::find_node,
                                               .key_value = std::move(key),
                                           },
                                           profile)));
   return dht::codec::decode(wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream))),
                             profile);
}

class queued_transport_stream final : public forge::net::transport::detail::stream_concept {
 public:
   explicit queued_transport_stream(std::int64_t stream_id) : stream_id_{stream_id} {}

   [[nodiscard]] bool valid() const noexcept override {
      return true;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return stream_id_;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override {
      writes.push_back({bytes.begin(), bytes.end()});
      co_return;
   }

   boost::asio::awaitable<void> async_write_chunk(forge::net::transport::chunk bytes) override {
      ++chunk_writes;
      writes.push_back(bytes.to_vector());
      co_return;
   }

   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      BOOST_REQUIRE(!reads.empty());
      auto out = std::move(reads.front());
      reads.pop_front();
      co_return out;
   }

   boost::asio::awaitable<forge::net::transport::chunk> async_read_chunk() override {
      ++chunk_reads;
      BOOST_REQUIRE(!reads.empty());
      auto out = std::move(reads.front());
      reads.pop_front();
      co_return forge::net::transport::chunk{std::move(out)};
   }

   boost::asio::awaitable<void> async_close() override {
      if (close_entered) {
         close_entered->arrive_and_wait();
      }
      if (close_release) {
         close_release->arrive_and_wait();
      }
      ++close_calls;
      closed = true;
      if (throw_on_close) {
         throw std::runtime_error{"injected stream close failure"};
      }
      co_return;
   }

   void cancel() override {
      ++cancel_calls;
      closed = true;
      if (throw_on_cancel) {
         throw std::runtime_error{"injected stream cancel failure"};
      }
   }

   std::deque<std::vector<std::uint8_t>> reads;
   std::vector<std::vector<std::uint8_t>> writes;
   std::size_t chunk_reads = 0;
   std::size_t chunk_writes = 0;
   std::size_t close_calls = 0;
   std::size_t cancel_calls = 0;
   bool throw_on_close = false;
   bool throw_on_cancel = false;
   bool closed = false;
   std::barrier<>* close_entered = nullptr;
   std::barrier<>* close_release = nullptr;

 private:
   std::int64_t stream_id_ = 0;
};

class stalling_transport_stream final : public forge::net::transport::detail::stream_concept {
 public:
   explicit stalling_transport_stream(std::int64_t stream_id, std::shared_ptr<forge::asio::notification> entered = {})
       : stream_id_{stream_id}, entered_{std::move(entered)} {}

   [[nodiscard]] bool valid() const noexcept override {
      return true;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return stream_id_;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t>) override {
      co_return;
   }

   boost::asio::awaitable<void> async_write_chunk(forge::net::transport::chunk) override {
      co_return;
   }

   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
      timer->expires_at(std::chrono::steady_clock::time_point::max());
      {
         auto lock = std::scoped_lock{mutex_};
         timer_ = timer;
      }
      if (entered_) {
         entered_->notify();
      }
      auto error = boost::system::error_code{};
      co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      if (error) {
         throw boost::system::system_error{error};
      }
      co_return std::vector<std::uint8_t>{};
   }

   boost::asio::awaitable<forge::net::transport::chunk> async_read_chunk() override {
      co_return forge::net::transport::chunk{co_await async_read()};
   }

   boost::asio::awaitable<void> async_close() override {
      closed = true;
      co_return;
   }

   void cancel() override {
      closed = true;
      auto lock = std::scoped_lock{mutex_};
      if (timer_) {
         timer_->cancel();
      }
   }

   bool closed = false;

 private:
   std::int64_t stream_id_ = 0;
   std::mutex mutex_;
   std::shared_ptr<boost::asio::steady_timer> timer_;
   std::shared_ptr<forge::asio::notification> entered_;
};

class tracking_peer_store_persistence final : public peer_store::persistence {
 public:
   boost::asio::awaitable<peer_store::hydration_page> async_hydrate(peer_store::hydration_request request) override {
      hydration_requests.push_back(request);
      if (fail_hydrate) {
         throw std::runtime_error{"injected peer hydration failure"};
      }
      if (block_hydrate && request.kind == peer_store::hydration_kind::peers) {
         auto executor = co_await boost::asio::this_coro::executor;
         auto timer = std::shared_ptr<boost::asio::steady_timer>{};
         {
            auto lock = std::scoped_lock{block_mutex};
            if (!hydrate_blocked_once) {
               timer = std::make_shared<boost::asio::steady_timer>(executor);
               timer->expires_at(std::chrono::steady_clock::time_point::max());
               hydrate_timer = timer;
               hydrate_blocked = true;
               hydrate_blocked_once = true;
            }
         }
         if (timer) {
            block_changed.notify_all();
            auto error = boost::system::error_code{};
            co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
         }
      }
      co_return co_await delegate->async_hydrate(std::move(request));
   }

   boost::asio::awaitable<peer_store::apply_result> async_apply(peer_store::mutation_batch batch) override {
      ++apply_attempts;
      if (fail_apply) {
         throw std::runtime_error{"injected peer apply failure"};
      }
      const auto peer_upserts = batch.peer_upserts.size();
      const auto peer_removals = batch.peer_removals.size();
      const auto rendezvous_upserts = batch.rendezvous_upserts.size();
      const auto rendezvous_removals = batch.rendezvous_removals.size();
      if (block_apply && (!block_rendezvous_apply_only || rendezvous_upserts != 0 || rendezvous_removals != 0)) {
         auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
         timer->expires_at(std::chrono::steady_clock::time_point::max());
         {
            auto lock = std::scoped_lock{block_mutex};
            apply_timer = timer;
            apply_blocked = true;
         }
         block_changed.notify_all();
         auto error = boost::system::error_code{};
         co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      const auto durable = batch.durable;
      auto rendezvous_high_watermark = batch.rendezvous_sequence_high_watermark;
      for (const auto& value : batch.rendezvous_upserts) {
         rendezvous_high_watermark = std::max(rendezvous_high_watermark, value.sequence);
      }
      auto result = co_await delegate->async_apply(std::move(batch));
      applied_peer_upserts += peer_upserts;
      applied_peer_removals += peer_removals;
      applied_rendezvous_upserts += rendezvous_upserts;
      applied_rendezvous_removals += rendezvous_removals;
      applied_rendezvous_high_watermark = std::max(applied_rendezvous_high_watermark, rendezvous_high_watermark);
      if (rendezvous_removals != 0) {
         {
            const auto lock = std::scoped_lock{block_mutex};
            rendezvous_removal_count_ += rendezvous_removals;
         }
         block_changed.notify_all();
      }
      durable_apply_attempts += durable ? 1U : 0U;
      if (durable && uncertain_durable_apply) {
         result.durability_confirmed = false;
         result.durability_failure = "injected uncertain durable acknowledgement";
      }
      co_return result;
   }

   boost::asio::awaitable<peer_store::prune_result> async_prune_expired(std::chrono::system_clock::time_point now,
                                                                        std::size_t limit) override {
      prune_limits.push_back(limit);
      if (fail_prune) {
         throw std::runtime_error{"injected peer prune failure"};
      }
      if (block_prune) {
         auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
         timer->expires_at(std::chrono::steady_clock::time_point::max());
         {
            auto lock = std::scoped_lock{block_mutex};
            prune_timer = timer;
            prune_blocked = true;
         }
         block_changed.notify_all();
         auto error = boost::system::error_code{};
         co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      co_return co_await delegate->async_prune_expired(now, limit);
   }

   boost::asio::awaitable<void> async_flush() override {
      ++flush_attempts;
      if (fail_flush) {
         throw std::runtime_error{"injected peer flush failure"};
      }
      co_await delegate->async_flush();
   }

   boost::asio::awaitable<void> async_close() override {
      ++close_attempts;
      if (fail_close) {
         throw std::runtime_error{"injected peer close failure"};
      }
      if (block_close) {
         auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
         timer->expires_at(std::chrono::steady_clock::time_point::max());
         {
            auto lock = std::scoped_lock{block_mutex};
            close_timer = timer;
            close_blocked = true;
         }
         block_changed.notify_all();
         auto error = boost::system::error_code{};
         co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      if (!retain_delegate_on_close) {
         co_await delegate->async_close();
      }
   }

   [[nodiscard]] bool wait_until_apply_blocked() {
      auto lock = std::unique_lock{block_mutex};
      return block_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return apply_blocked; });
   }

   [[nodiscard]] bool wait_until_hydrate_blocked() {
      auto lock = std::unique_lock{block_mutex};
      return block_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return hydrate_blocked; });
   }

   [[nodiscard]] bool wait_until_prune_blocked() {
      auto lock = std::unique_lock{block_mutex};
      return block_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return prune_blocked; });
   }

   [[nodiscard]] bool wait_until_close_blocked() {
      auto lock = std::unique_lock{block_mutex};
      return block_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return close_blocked; });
   }

   [[nodiscard]] bool wait_until_rendezvous_removal(std::size_t count = 1) {
      auto lock = std::unique_lock{block_mutex};
      return block_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return rendezvous_removal_count_ >= count; });
   }

   void release_apply() {
      auto timer = std::shared_ptr<boost::asio::steady_timer>{};
      {
         auto lock = std::scoped_lock{block_mutex};
         timer = apply_timer;
         apply_blocked = false;
         apply_timer.reset();
      }
      if (timer) {
         boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
      }
   }

   void release_hydrate() {
      auto timer = std::shared_ptr<boost::asio::steady_timer>{};
      {
         auto lock = std::scoped_lock{block_mutex};
         timer = hydrate_timer;
         hydrate_blocked = false;
         hydrate_timer.reset();
      }
      if (timer) {
         boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
      }
   }

   void release_prune() {
      auto timer = std::shared_ptr<boost::asio::steady_timer>{};
      {
         auto lock = std::scoped_lock{block_mutex};
         timer = prune_timer;
         prune_blocked = false;
         prune_timer.reset();
      }
      if (timer) {
         boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
      }
   }

   void release_close() {
      auto timer = std::shared_ptr<boost::asio::steady_timer>{};
      {
         auto lock = std::scoped_lock{block_mutex};
         timer = close_timer;
         close_blocked = false;
         close_timer.reset();
      }
      if (timer) {
         boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
      }
   }

   std::shared_ptr<peer_store::persistence> delegate = peer_store::make_memory_persistence();
   std::vector<peer_store::hydration_request> hydration_requests;
   std::vector<std::size_t> prune_limits;
   std::size_t apply_attempts = 0;
   std::size_t applied_peer_upserts = 0;
   std::size_t applied_peer_removals = 0;
   std::size_t applied_rendezvous_upserts = 0;
   std::size_t applied_rendezvous_removals = 0;
   std::uint64_t applied_rendezvous_high_watermark = 0;
   std::size_t durable_apply_attempts = 0;
   std::size_t flush_attempts = 0;
   std::size_t close_attempts = 0;
   bool fail_hydrate = false;
   bool fail_apply = false;
   bool uncertain_durable_apply = false;
   bool fail_prune = false;
   bool fail_flush = false;
   bool fail_close = false;
   bool block_apply = false;
   bool block_rendezvous_apply_only = false;
   bool block_hydrate = false;
   bool block_prune = false;
   bool block_close = false;
   bool retain_delegate_on_close = false;
   std::mutex block_mutex;
   std::condition_variable block_changed;
   std::shared_ptr<boost::asio::steady_timer> apply_timer;
   std::shared_ptr<boost::asio::steady_timer> hydrate_timer;
   std::shared_ptr<boost::asio::steady_timer> prune_timer;
   std::shared_ptr<boost::asio::steady_timer> close_timer;
   bool apply_blocked = false;
   bool hydrate_blocked = false;
   bool hydrate_blocked_once = false;
   bool prune_blocked = false;
   bool close_blocked = false;
   std::size_t rendezvous_removal_count_ = 0;
};

class tracking_dht_record_store_persistence final : public dht::record_store::persistence {
 public:
   boost::asio::awaitable<dht::record_store::hydration_page>
   async_hydrate(dht::record_store::hydration_request request) override {
      co_return co_await delegate->async_hydrate(std::move(request));
   }

   boost::asio::awaitable<dht::record_store::apply_result>
   async_apply(dht::record_store::mutation_batch batch) override {
      const auto previous_upserts =
          provider_upsert_attempts.fetch_add(batch.provider_upserts.size(), std::memory_order_relaxed);
      value_upsert_attempts.fetch_add(batch.value_upserts.size(), std::memory_order_relaxed);
      provider_remove_attempts.fetch_add(batch.provider_removals.size(), std::memory_order_relaxed);
      if (!batch.provider_removals.empty() && reject_next_provider_removal.exchange(false, std::memory_order_relaxed)) {
         throw std::runtime_error{"injected DHT provider removal failure"};
      }
      if (!batch.provider_removals.empty() && block_next_provider_removal.exchange(false, std::memory_order_relaxed)) {
         auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
         timer->expires_at(std::chrono::steady_clock::time_point::max());
         {
            auto lock = std::scoped_lock{provider_removal_mutex};
            provider_removal_timer = timer;
            provider_removal_blocked = true;
         }
         provider_removal_changed.notify_all();
         auto error = boost::system::error_code{};
         co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      if (!batch.provider_upserts.empty() && before_provider_apply) {
         auto callback = std::exchange(before_provider_apply, {});
         callback();
      }
      if (reject_provider_upserts && !batch.provider_upserts.empty()) {
         FORGE_THROW_CODE(exceptions::code::backpressure_rejected, "injected DHT provider persistence backpressure");
      }
      if (reject_provider_upserts_after_first && previous_upserts != 0 && !batch.provider_upserts.empty()) {
         FORGE_THROW_CODE(exceptions::code::backpressure_rejected,
                          "injected DHT provider republish persistence backpressure");
      }
      if (!batch.value_upserts.empty() && reject_next_value_upsert.exchange(false, std::memory_order_relaxed)) {
         FORGE_THROW_CODE(exceptions::code::backpressure_rejected, "injected DHT value persistence backpressure");
      }
      const auto uncertain =
          !batch.value_upserts.empty() && uncertain_next_value_upsert.exchange(false, std::memory_order_relaxed);
      auto result = co_await delegate->async_apply(std::move(batch));
      if (uncertain) {
         result.durability_confirmed = false;
         result.durability_failure = "injected post-commit DHT value durability failure";
      }
      co_return result;
   }

   boost::asio::awaitable<dht::record_store::prune_result>
   async_prune_expired(std::chrono::system_clock::time_point now, std::size_t limit) override {
      prune_attempts.fetch_add(1, std::memory_order_relaxed);
      co_return co_await delegate->async_prune_expired(now, limit);
   }

   boost::asio::awaitable<void> async_flush() override {
      co_await delegate->async_flush();
   }

   boost::asio::awaitable<void> async_close() override {
      close_attempts.fetch_add(1, std::memory_order_relaxed);
      co_await delegate->async_close();
   }

   [[nodiscard]] bool wait_until_provider_removal_blocked() {
      auto lock = std::unique_lock{provider_removal_mutex};
      return provider_removal_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return provider_removal_blocked; });
   }

   void release_provider_removal() {
      auto timer = std::shared_ptr<boost::asio::steady_timer>{};
      {
         auto lock = std::scoped_lock{provider_removal_mutex};
         timer = provider_removal_timer;
         provider_removal_timer.reset();
         provider_removal_blocked = false;
      }
      if (timer) {
         boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
      }
   }

   std::shared_ptr<dht::record_store::persistence> delegate = dht::record_store::make_memory_persistence();
   std::atomic_size_t provider_upsert_attempts = 0;
   std::atomic_size_t value_upsert_attempts = 0;
   std::atomic_size_t provider_remove_attempts = 0;
   std::atomic_size_t prune_attempts = 0;
   std::atomic_size_t close_attempts = 0;
   std::atomic_bool reject_next_provider_removal = false;
   std::atomic_bool block_next_provider_removal = false;
   std::atomic_bool reject_next_value_upsert = false;
   std::atomic_bool uncertain_next_value_upsert = false;
   std::function<void()> before_provider_apply;
   bool reject_provider_upserts = false;
   bool reject_provider_upserts_after_first = false;
   std::mutex provider_removal_mutex;
   std::condition_variable provider_removal_changed;
   std::shared_ptr<boost::asio::steady_timer> provider_removal_timer;
   bool provider_removal_blocked = false;
};

void seed_dht_provider_records(forge::asio::runtime& runtime, const dht::profile& profile,
                               const std::shared_ptr<dht::record_store::persistence>& persistence,
                               std::vector<dht::record_store::provider_record> providers) {
   auto store = dht::record_store{profile, dht::record_store::options{.persistence = persistence}};
   for (auto& provider : providers) {
      forge::asio::blocking::run(runtime, store.async_upsert_provider(std::move(provider)));
   }
   forge::asio::blocking::run(runtime, store.async_flush());
}

[[nodiscard]] std::size_t dht_bucket_for(const peer_id& local, const peer_id& candidate) {
   const auto distance = distance_between(local.to_bytes(), candidate.to_bytes());
   auto prefix_bits = std::size_t{};
   for (const auto byte : distance.bytes) {
      if (byte == 0) {
         prefix_bits += 8;
         continue;
      }
      prefix_bits += static_cast<std::size_t>(std::countl_zero(byte));
      break;
   }
   return std::min(prefix_bits, std::size_t{255});
}

[[nodiscard]] std::vector<peer_id> peers_in_same_dht_bucket(const peer_id& local, std::size_t count) {
   auto buckets = std::map<std::size_t, std::vector<peer_id>>{};
   for (auto value = std::uint16_t{}; value <= 255; ++value) {
      auto candidate = peer(static_cast<std::uint8_t>(value));
      if (candidate == local) {
         continue;
      }
      auto& bucket = buckets[dht_bucket_for(local, candidate)];
      bucket.push_back(std::move(candidate));
      if (bucket.size() == count) {
         return bucket;
      }
   }
   throw std::runtime_error{"not enough deterministic peers in one DHT bucket"};
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_session_info_preserves_legacy_structured_binding_shape) {
   auto [remote_peer, capabilities, path, relay_peer, identify_state] = node::session_info{};
   BOOST_TEST(remote_peer.value.empty());
   BOOST_TEST(capabilities.bits == 0U);
   BOOST_TEST(static_cast<std::uint8_t>(path) == static_cast<std::uint8_t>(forge::net::p2p::path::kind::direct));
   BOOST_TEST(!relay_peer.has_value());
   BOOST_TEST(static_cast<std::uint8_t>(identify_state) ==
              static_cast<std::uint8_t>(forge::net::p2p::identify::state::unknown));
}

BOOST_AUTO_TEST_CASE(p2p_quic_client_token_cache_close_rejects_late_callbacks) {
   auto cache = std::make_shared<direct::detail::quic_client_token_cache>(2);
   const auto first_peer = peer(1);
   const auto second_peer = peer(2);
   const auto remote = parse_endpoint("/ip4/127.0.0.1/udp/4001/quic-v1");
   auto first_options = direct::detail::make_quic_client_options(remote, first_peer, std::chrono::seconds{1},
                                                                 forge::net::quic::transport_limits{}, "certificate",
                                                                 "key", false, cache);
   auto second_options = direct::detail::make_quic_client_options(remote, second_peer, std::chrono::seconds{1},
                                                                  forge::net::quic::transport_limits{}, "certificate",
                                                                  "key", false, cache);
   BOOST_REQUIRE(first_options.client_tokens);
   BOOST_REQUIRE(second_options.client_tokens);
   first_options.client_tokens->store({4, 5, 6});
   BOOST_TEST(!second_options.client_tokens->take());
   BOOST_REQUIRE(first_options.client_tokens->take().has_value());

   first_options.client_tokens->store({7, 8, 9});
   cache->close();
   BOOST_TEST(!first_options.client_tokens->take());
   first_options.client_tokens->store({10, 11, 12});
   BOOST_TEST(!first_options.client_tokens->take());
}

BOOST_AUTO_TEST_CASE(p2p_identity_uses_libp2p_multihash_shape) {
   const auto identity = make_test_certificate_identity("p2p-identity-shape");
   const auto id = make_peer_id_from_certificate_pem(identity.certificate_pem);

   BOOST_TEST(id.to_string() == identity.peer.to_string());
   BOOST_TEST(valid_peer_id(id));
   auto decoded = forge::multiformats::multihash::decode(id.to_bytes());
   BOOST_TEST(decoded.code == forge::multiformats::code_value(forge::multiformats::multicodec_code::sha2_256));
   BOOST_TEST(decoded.digest.size() == 32U);
}

BOOST_AUTO_TEST_CASE(p2p_certificate_without_libp2p_extension_is_rejected) {
   BOOST_CHECK_THROW((void)make_peer_id_from_certificate_pem(test_certificate()), exceptions::invalid_identity);
   const auto certificate = forge::crypto::pki::x509::certificate::from_pem(test_certificate());
   BOOST_CHECK_THROW((void)make_peer_id_from_certificate_der(certificate.der()), exceptions::invalid_identity);
}

BOOST_AUTO_TEST_CASE(p2p_public_key_encoding_matches_libp2p_vectors) {
   const auto ed25519_public_key = bytes_from_hex("1ed1e8fae2c4a144b8be8fd4b47bf3d3b34b871c3cacf6010f0e42d474fce27e");
   const auto ed25519_encoded = encode_public_key({.type = public_key::type::ed25519, .data = ed25519_public_key});
   BOOST_CHECK_EQUAL(forge::multiformats::multihash::identity(ed25519_encoded).digest_hex(),
                     "080112201ed1e8fae2c4a144b8be8fd4b47bf3d3b34b871c3cacf6010f0e42d474fce27e");

   auto ed25519_peer = make_peer_id({.type = public_key::type::ed25519, .data = ed25519_public_key});
   auto ed25519_hash = forge::multiformats::multihash::decode(ed25519_peer.to_bytes());
   BOOST_TEST(ed25519_hash.code == forge::multiformats::code_value(forge::multiformats::multicodec_code::identity));

   const auto ecdsa_public_key =
       bytes_from_hex("3059301306072a8648ce3d020106082a8648ce3d03010703420004de3d300fa36ae0e8f5d530899d83abab44ab"
                      "f3161f162a4bc901d8e6ecda020e8b6d5f8da30525e71d6851510c098e5c47c646a597fb4dcec034e9f77c409e62");
   auto ecdsa_peer = make_peer_id({.type = public_key::type::ecdsa, .data = ecdsa_public_key});
   auto ecdsa_hash = forge::multiformats::multihash::decode(ecdsa_peer.to_bytes());
   BOOST_TEST(ecdsa_hash.code == forge::multiformats::code_value(forge::multiformats::multicodec_code::sha2_256));
}

BOOST_AUTO_TEST_CASE(p2p_certificate_extension_rejects_unverified_non_ed25519_identity) {
   const auto identity = make_secp256k1_identity();
   auto bogus_signature = std::vector<std::uint8_t>(72, 0x42);
   const auto extension = signed_key_der(encode_public_key(identity.key), bogus_signature);
   const auto certificate = make_certificate_der_with_libp2p_extension(extension);

   BOOST_CHECK_THROW((void)make_peer_id_from_certificate_der(certificate), exceptions::invalid_identity);
}

BOOST_AUTO_TEST_CASE(p2p_certificate_extension_rejects_overflowing_identity_octet_length) {
   const auto extension = signed_key_der_with_overflowing_octet_length();
   const auto certificate = make_certificate_der_with_libp2p_extension(extension);

   BOOST_CHECK_THROW((void)make_peer_id_from_certificate_der(certificate), exceptions::invalid_identity);
}

BOOST_AUTO_TEST_CASE(p2p_certificate_extension_verifies_supported_identities) {
   const auto identities = std::vector<test_identity>{make_test_identity(), make_rsa_identity(),
                                                      make_secp256k1_identity(), make_p256_identity()};
   for (const auto& identity : identities) {
      const auto certificate = make_certificate_der_with_libp2p_extension_from_factory(
          [&](std::span<const std::uint8_t> certificate_public_key) {
             return signed_tls_extension(identity, certificate_public_key);
          });

      BOOST_TEST(make_peer_id_from_certificate_der(certificate).to_string() == identity.peer.to_string());
   }
}

BOOST_AUTO_TEST_CASE(p2p_identity_signatures_support_supported_key_types) {
   const auto identities = std::vector<test_identity>{make_test_identity(), make_rsa_identity(),
                                                      make_secp256k1_identity(), make_p256_identity()};
   for (const auto& identity : identities) {
      auto message = pubsub::message{
          .from = identity.peer,
          .data = std::vector<std::uint8_t>{'m', 'u', 'l', 't', 'i', '-', 'k', 'e', 'y'},
          .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 9},
          .subject = pubsub::topic{.value = "forge.identity"},
          .key = encode_public_key(identity.key),
      };
      pubsub::codec::sign_message(message, identity.private_key);
      BOOST_TEST(pubsub::codec::verify_message(message));

      auto tampered = message;
      tampered.signature.back() ^= 0x01U;
      BOOST_TEST(!pubsub::codec::verify_message(tampered));

      const auto payload_type = forge::multiformats::varint_encode(0x0302);
      const auto payload = std::vector<std::uint8_t>{7, 8, 9};
      const auto envelope =
          signed_envelope::seal(identity.key, identity.private_key, "libp2p-relay-rsvp", payload_type, payload);
      BOOST_CHECK_NO_THROW(envelope.verify("libp2p-relay-rsvp", identity.peer));

      auto tampered_envelope = envelope;
      tampered_envelope.payload.back() ^= 0x01U;
      BOOST_CHECK_THROW(tampered_envelope.verify("libp2p-relay-rsvp", identity.peer), exceptions::invalid_identity);

      auto malformed_envelope = envelope;
      malformed_envelope.signature.pop_back();
      BOOST_CHECK_THROW(malformed_envelope.verify("libp2p-relay-rsvp", identity.peer), exceptions::invalid_identity);
   }
}

BOOST_AUTO_TEST_CASE(p2p_peer_id_legacy_and_cid_strings_roundtrip) {
   auto id =
       make_peer_id({.type = public_key::type::secp256k1,
                     .data = bytes_from_hex("037777e994e452c21604f91de093ce415f5432f701dd8cd1a7a6fea0e630bfca99")});

   auto legacy = id.to_string();
   BOOST_TEST(peer_id::from_string(legacy).to_string() == id.to_string());

   auto cid = id.to_cid_string();
   BOOST_TEST(cid.front() == 'b');
   BOOST_TEST(peer_id::from_string(cid).to_string() == id.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_endpoint_parses_libp2p_quic_address_format) {
   static_assert(std::is_same_v<decltype(endpoint{}.transport.host_type), endpoint::host_kind>);

   const auto id = peer(42);
   auto parsed = parse_endpoint("/ip4/127.0.0.1/udp/4001/quic-v1/p2p/" + id.to_string());

   BOOST_TEST(static_cast<int>(parsed.transport.host_type) == static_cast<int>(endpoint::host_kind::ip4));

   BOOST_TEST(parsed.transport.host == "127.0.0.1");
   BOOST_TEST(parsed.transport.port == 4001);
   BOOST_REQUIRE(parsed.peer.has_value());
   BOOST_TEST(parsed.peer->to_string() == id.to_string());
   BOOST_TEST(parsed.to_string() == "/ip4/127.0.0.1/udp/4001/quic-v1/p2p/" + id.to_string());
   BOOST_TEST(parsed.is_direct_quic());
}

BOOST_AUTO_TEST_CASE(p2p_endpoint_roundtrips_scoped_ipv6_and_rejects_misplaced_zones) {
   const auto first = parse_endpoint("/ip6zone/en0/ip6/fe80::1/tcp/4001");
   const auto second = parse_endpoint("/ip6zone/en1/ip6/fe80::1/tcp/4001");

   BOOST_TEST(static_cast<int>(first.transport.host_type) == static_cast<int>(endpoint::host_kind::ip6));
   BOOST_TEST(first.transport.host == "fe80::1");
   BOOST_TEST(first.transport.zone == "en0");
   BOOST_TEST(first.transport.authority() == "[fe80::1%en0]:4001");
   BOOST_TEST(first.to_string() == "/ip6zone/en0/ip6/fe80::1/tcp/4001");
   BOOST_TEST(first.to_multiaddr().to_bytes() ==
              forge::multiformats::multiaddr::parse("/ip6zone/en0/ip6/fe80::1/tcp/4001").to_bytes(),
              boost::test_tools::per_element());
   BOOST_TEST(first.to_string() != second.to_string());

   const auto quic = parse_endpoint("/ip6zone/en0/ip6/fe80::1/udp/4001/quic-v1");
   BOOST_TEST(quic.is_direct_quic());
   BOOST_TEST(quic.transport.zone == "en0");
   BOOST_TEST(quic.to_string() == "/ip6zone/en0/ip6/fe80::1/udp/4001/quic-v1");

   const auto expected = peer(44).to_bytes();
   BOOST_TEST(direct::detail::quic_client_token_cache::make_key(expected, "ip6", "fe80::1", "en0", 4001) !=
              direct::detail::quic_client_token_cache::make_key(expected, "ip6", "fe80::1", "en1", 4001));

   BOOST_CHECK_THROW((void)parse_endpoint("/ip6zone/en0/ip6zone/en1/ip6/fe80::1/tcp/4001"),
                     exceptions::invalid_options);
   BOOST_CHECK_THROW((void)parse_endpoint("/ip6/fe80::1/ip6zone/en0/tcp/4001"), exceptions::invalid_options);
   BOOST_CHECK_THROW((void)parse_endpoint("/ip6zone/en0/ip4/192.0.2.1/tcp/4001"), exceptions::invalid_options);

   const auto invalid = endpoint{.transport = {.host_type = endpoint::host_kind::ip4,
                                                .protocol = endpoint::protocol_kind::tcp,
                                                .host = "192.0.2.1",
                                                .port = 4001,
                                                .zone = "en0"}};
   BOOST_CHECK_THROW((void)invalid.to_multiaddr(), exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_endpoint_uses_multiaddr_for_tcp_wss_and_relay_views) {
   const auto id = peer(43);

   auto tcp = parse_endpoint("/dns4/example.com/tcp/4001/p2p/" + id.to_string());
   BOOST_TEST(static_cast<int>(tcp.transport.host_type) == static_cast<int>(endpoint::host_kind::dns4));
   BOOST_TEST(static_cast<int>(tcp.transport.protocol) == static_cast<int>(endpoint::protocol_kind::tcp));
   BOOST_TEST(tcp.transport.host == "example.com");
   BOOST_TEST(tcp.transport.port == 4001);
   BOOST_TEST(tcp.is_direct_tcp());
   BOOST_TEST(tcp.to_string() == "/dns4/example.com/tcp/4001/p2p/" + id.to_string());

   auto wss = parse_endpoint("/dns4/example.com/tcp/443/wss/p2p/" + id.to_string());
   BOOST_TEST(!wss.is_direct_tcp());
   BOOST_TEST(!wss.is_direct_quic());
   BOOST_TEST(wss.to_string() == "/dns4/example.com/tcp/443/wss/p2p/" + id.to_string());

   auto relayed = parse_endpoint("/ip4/127.0.0.1/tcp/9090/p2p-circuit/p2p/" + id.to_string());
   BOOST_TEST(!relayed.peer.has_value());
   BOOST_REQUIRE(relayed.relayed.has_value());
   BOOST_TEST(relayed.relayed->target.to_string() == id.to_string());
   BOOST_TEST(relayed.to_string() == "/ip4/127.0.0.1/tcp/9090/p2p-circuit/p2p/" + id.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_websocket_multiaddr_is_parseable_but_not_dialable) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto value = node{runtime, options_for(peer(44))};
   const auto endpoints = std::vector<endpoint>{
       parse_endpoint("/dns4/example.com/tcp/80/ws/p2p/" + peer(46).to_string()),
       parse_endpoint("/dns4/example.com/tcp/443/wss/p2p/" + peer(47).to_string()),
   };

   for (const auto& endpoint : endpoints) {
      try {
         forge::asio::blocking::run(runtime, value.async_listen(endpoint));
         BOOST_FAIL("expected unsupported listen endpoint");
      } catch (const forge::exceptions::base& error) {
         BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
         BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                    static_cast<int>(exceptions::code::unsupported_protocol));
      }

      try {
         forge::asio::blocking::run(runtime, value.async_connect(endpoint));
         BOOST_FAIL("expected unsupported connect endpoint");
      } catch (const forge::exceptions::base& error) {
         BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
         BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                    static_cast<int>(exceptions::code::unsupported_protocol));
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_node_listens_on_quic_and_tcp_and_identify_advertises_both) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(210))};
   auto client = node{runtime, options_for(peer(211))};

   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));

   const auto local = server.local_endpoints();
   BOOST_REQUIRE_EQUAL(local.size(), 2U);
   BOOST_TEST(contains_protocol(local, endpoint::protocol_kind::quic_v1));
   BOOST_TEST(contains_protocol(local, endpoint::protocol_kind::tcp));
   for (const auto& item : local) {
      BOOST_REQUIRE(item.peer.has_value());
      BOOST_TEST(item.peer->value == server.local_peer().value);
   }
   BOOST_REQUIRE(server.local_endpoint().has_value());

   const auto quic = require_endpoint_for(local, endpoint::protocol_kind::quic_v1);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(quic, node::connect_options{.expected_peer = server.local_peer()}));

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify));
   const auto payload = forge::asio::blocking::run(runtime, read_length_delimited(stream));
   const auto doc = identify::decode(payload);
   BOOST_TEST(contains_protocol(doc.listen_endpoints, endpoint::protocol_kind::quic_v1));
   BOOST_TEST(contains_protocol(doc.listen_endpoints, endpoint::protocol_kind::tcp));
   for (const auto& item : doc.listen_endpoints) {
      BOOST_TEST(peer_for(item).value == server.local_peer().value);
   }

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_response_contains_canonical_signed_record_and_observed_address) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-standard-record-server");
   const auto client_identity = make_test_certificate_identity("identify-standard-record-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify));
   const auto document = identify::decode(forge::asio::blocking::run(runtime, read_length_delimited(stream)));

   BOOST_REQUIRE(document.observed_endpoint.has_value());
   BOOST_TEST(document.observed_endpoint->transport.port != 0U);
   BOOST_REQUIRE(!document.signed_peer_record.empty());
   const auto envelope = signed_envelope::decode(document.signed_peer_record);
   BOOST_TEST(envelope.payload_type == identify_peer_record_payload_type(), boost::test_tools::per_element());
   BOOST_CHECK_NO_THROW(envelope.verify("libp2p-peer-record", server.local_peer()));
   const auto record = rendezvous::codec::decode_peer_record(envelope.payload);
   BOOST_TEST(record.peer.to_string() == server.local_peer().to_string());
   BOOST_TEST(record.sequence > 0U);
   BOOST_REQUIRE(!record.endpoints.empty());

   auto second_stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify));
   const auto second_document =
       identify::decode(forge::asio::blocking::run(runtime, read_length_delimited(second_stream)));
   BOOST_TEST(second_document.signed_peer_record == document.signed_peer_record, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_completes_for_multiple_inbound_sessions) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto server_identity = make_test_certificate_identity("identify-multiple-server");
   const auto first_identity = make_test_certificate_identity("identify-multiple-first");
   const auto second_identity = make_test_certificate_identity("identify-multiple-second");
   auto server = node{runtime, options_for(server_identity)};
   auto first = node{runtime, options_for(first_identity)};
   auto second = node{runtime, options_for(second_identity)};
   const auto server_endpoint = listen(server, runtime);

   const auto connect = [&](node& client) {
      return forge::asio::blocking::run(runtime,
                                        client.async_connect(server_endpoint, node::connect_options{
                                                                                  .expected_peer = server.local_peer(),
                                                                                  .timeout = std::chrono::seconds{2},
                                                                              }));
   };
   const auto first_session = connect(first);
   const auto second_session = connect(second);

   BOOST_TEST(static_cast<int>(first_session.identify_state) == static_cast<int>(identify::state::identified));
   BOOST_TEST(static_cast<int>(second_session.identify_state) == static_cast<int>(identify::state::identified));

   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_response_is_trimmed_to_rust_message_limit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-trim-server");
   const auto client_identity = make_test_certificate_identity("identify-trim-client");
   auto advertised = std::vector<endpoint>{};
   for (auto port = std::uint16_t{4300}; port < 4364; ++port) {
      advertised.push_back(parse_endpoint("/ip4/127.0.0.1/udp/" + std::to_string(port) + "/quic-v1/p2p/" +
                                          server_identity.peer.to_string()));
   }
   const auto advertised_count = advertised.size();
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   server.set_advertised_endpoints(std::move(advertised));
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify));
   const auto payload = forge::asio::blocking::run(runtime, read_length_delimited(stream));
   const auto document = identify::decode(payload);

   BOOST_TEST(payload.size() <= identify::limits{}.max_own_message_size);
   BOOST_TEST(document.listen_endpoints.size() < advertised_count);
   BOOST_REQUIRE(!document.signed_peer_record.empty());
   BOOST_TEST(rendezvous::codec::decode_peer_record(signed_envelope::decode(document.signed_peer_record).payload)
                  .endpoints.size() == document.listen_endpoints.size());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_peer_record_sequence_advances_across_node_restart) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto restarted_identity = make_test_certificate_identity("identify-restart-peer");
   const auto observer_identity = make_test_certificate_identity("identify-restart-observer");
   auto observer = std::make_unique<node>(runtime, options_for(observer_identity));

   auto first = std::make_unique<node>(runtime, options_for(restarted_identity));
   const auto first_endpoint = listen(*first, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, observer->async_connect(first_endpoint, node::connect_options{.expected_peer = first->local_peer()})));
   const auto first_record = observer->peers().find(first->local_peer());
   BOOST_REQUIRE(first_record);
   const auto first_sequence = identify_peer_record_sequence(first_record->signed_peer_record);

   forge::asio::blocking::run(runtime, observer->async_stop());
   observer.reset();
   forge::asio::blocking::run(runtime, first->async_stop());
   first.reset();

   observer = std::make_unique<node>(runtime, options_for(observer_identity));
   observer->peers().upsert(*first_record);
   auto second = std::make_unique<node>(runtime, options_for(restarted_identity));
   const auto second_endpoint = listen(*second, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime,
       observer->async_connect(second_endpoint, node::connect_options{.expected_peer = second->local_peer()})));
   const auto second_record = observer->peers().find(second->local_peer());
   BOOST_REQUIRE(second_record);
   BOOST_TEST(identify_peer_record_sequence(second_record->signed_peer_record) > first_sequence);

   forge::asio::blocking::run(runtime, observer->async_stop());
   forge::asio::blocking::run(runtime, second->async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_idle_listeners_do_not_consume_pending_inbound_session_budget) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = options_for(peer(198));
   server_options.limits.resources.transient.max_inbound_connections = 1;
   server_options.limits.resources.transient.max_connections = 1;
   auto server = node{runtime, std::move(server_options)};

   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));
   wait_on_runtime(runtime, std::chrono::milliseconds{20}, "idle P2P listeners");

   BOOST_TEST(server.diagnostics().resources.transient.inbound_connections == 0U);
   BOOST_TEST(server.diagnostics().resources.transient.file_descriptors == 0U);

   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_gater_orders_all_direct_stages_for_tcp_and_quic) {
   for (const auto transport : {endpoint::protocol_kind::tcp, endpoint::protocol_kind::quic_v1}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto server_gater = std::make_shared<recording_connection_gater>();
      auto client_gater = std::make_shared<recording_connection_gater>();
      auto server_options = options_for(peer(193));
      auto client_options = options_for(peer(194));
      server_options.connection_gater = server_gater;
      client_options.connection_gater = client_gater;
      auto server = node{runtime, std::move(server_options)};
      auto client = node{runtime, std::move(client_options)};

      forge::asio::blocking::run(runtime, server.async_listen(transport == endpoint::protocol_kind::tcp
                                                                  ? make_tcp_endpoint(0)
                                                                  : make_quic_endpoint(0)));
      const auto server_endpoint = require_endpoint_for(server.local_endpoints(), transport);
      static_cast<void>(forge::asio::blocking::run(
          runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (server_gater->stages().size() != 3U && std::chrono::steady_clock::now() < deadline) {
         wait_on_runtime(runtime, std::chrono::milliseconds{1}, "P2P connection gater inbound stages");
      }
      check_gater_stages(client_gater->stages(), {recorded_gater_stage::peer_dial, recorded_gater_stage::address_dial,
                                                  recorded_gater_stage::secured, recorded_gater_stage::upgraded});
      check_gater_stages(server_gater->stages(),
                         {recorded_gater_stage::accept, recorded_gater_stage::secured, recorded_gater_stage::upgraded});
      BOOST_TEST(client.metrics().active_sessions == 1U);
      BOOST_TEST(server.metrics().active_sessions == 1U);

      forge::asio::blocking::run(runtime, client.async_stop());
      forge::asio::blocking::run(runtime, server.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_suffixless_literal_tcp_connect_gates_authenticated_stages_without_late_peer_dial) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(make_test_identity())};
   auto client_gater = std::make_shared<recording_connection_gater>(recorded_gater_stage::peer_dial);
   auto client_options = options_for(make_test_identity());
   client_options.connection_gater = client_gater;
   auto client = node{runtime, std::move(client_options)};
   const auto listening = listen_tcp(server, runtime);
   const auto root = forge::multiformats::multiaddr::parse(
       "/ip4/127.0.0.1/tcp/" + std::to_string(listening.transport.port));

   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(root, node::connect_options{.allow_relay = false}));
   BOOST_TEST(session.remote_peer.to_string() == server.local_peer().to_string());
   check_gater_stages(client_gater->stages(), {recorded_gater_stage::secured, recorded_gater_stage::upgraded});
   check_gater_stages(client_gater->stages_for(server.local_peer()),
                      {recorded_gater_stage::secured, recorded_gater_stage::upgraded});
   BOOST_TEST(client.metrics().active_sessions == 1U);
   BOOST_TEST(client.diagnostics().resources.active_dials == 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_gater_local_denials_are_typed_and_leave_no_client_state_for_tcp_and_quic) {
   for (const auto transport : {endpoint::protocol_kind::tcp, endpoint::protocol_kind::quic_v1}) {
      for (const auto rejected : {recorded_gater_stage::peer_dial, recorded_gater_stage::address_dial,
                                  recorded_gater_stage::secured, recorded_gater_stage::upgraded}) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
         auto client_gater = std::make_shared<recording_connection_gater>(rejected);
         auto server_options = options_for(peer(195));
         auto client_options = options_for(peer(196));
         client_options.connection_gater = client_gater;
         auto server = node{runtime, std::move(server_options)};
         auto client = node{runtime, std::move(client_options)};

         forge::asio::blocking::run(runtime, server.async_listen(transport == endpoint::protocol_kind::tcp
                                                                     ? make_tcp_endpoint(0)
                                                                     : make_quic_endpoint(0)));
         const auto server_endpoint = require_endpoint_for(server.local_endpoints(), transport);
         auto rejected_typed = false;
         try {
            static_cast<void>(forge::asio::blocking::run(
                runtime,
                client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
         } catch (const forge::exceptions::base& error) {
            rejected_typed = exceptions::code_of(error) == exceptions::code::connection_rejected;
         }

         BOOST_TEST(rejected_typed);
         BOOST_TEST(gater_rejections(client.metrics(), rejected) == 1U);
         BOOST_TEST(client.metrics().active_sessions == 0U);
         BOOST_TEST(client.diagnostics().sessions.empty());
         BOOST_TEST(client.diagnostics().resources.transient.outbound_connections == 0U);
         BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 0U);
         BOOST_TEST(client.diagnostics().resources.denied_connections == 0U);
         BOOST_TEST(!client.peers().find(server.local_peer()).has_value());
         const auto descriptor_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
         while (client.diagnostics().resources.system.file_descriptors != 0U &&
                std::chrono::steady_clock::now() < descriptor_deadline) {
            wait_on_runtime(runtime, std::chrono::milliseconds{1}, "P2P locally rejected session descriptor release");
         }
         BOOST_TEST(client.diagnostics().resources.system.file_descriptors == 0U);

         forge::asio::blocking::run(runtime, client.async_stop());
         forge::asio::blocking::run(runtime, server.async_stop());
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_authenticated_peer_limit_rejects_direct_outbound_before_upgraded_for_tcp_and_quic) {
   for (const auto transport : {endpoint::protocol_kind::tcp, endpoint::protocol_kind::quic_v1}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto client_gater = std::make_shared<recording_connection_gater>();
      auto server_options = options_for(peer(199));
      auto client_options = options_for(peer(200));
      client_options.connection_gater = client_gater;
      client_options.limits.resources.peer.max_outbound_connections = 0;
      client_options.limits.resources.peer.max_connections = 0;
      auto server = node{runtime, std::move(server_options)};
      auto client = node{runtime, std::move(client_options)};

      forge::asio::blocking::run(runtime, server.async_listen(transport == endpoint::protocol_kind::tcp
                                                                  ? make_tcp_endpoint(0)
                                                                  : make_quic_endpoint(0)));
      const auto server_endpoint = require_endpoint_for(server.local_endpoints(), transport);
      auto rejected = false;
      try {
         static_cast<void>(forge::asio::blocking::run(
             runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
      } catch (const forge::exceptions::base& error) {
         rejected = exceptions::code_of(error) == exceptions::code::backpressure_rejected;
      }

      BOOST_TEST(rejected);
      check_gater_stages(client_gater->stages(),
                         {recorded_gater_stage::peer_dial, recorded_gater_stage::address_dial,
                          recorded_gater_stage::secured});
      const auto descriptor_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (client.diagnostics().resources.system.file_descriptors != 0U &&
             std::chrono::steady_clock::now() < descriptor_deadline) {
         wait_on_runtime(runtime, std::chrono::milliseconds{1}, "P2P rejected outbound session descriptor release");
      }
      const auto resources = client.diagnostics().resources;
      BOOST_TEST(client.metrics().active_sessions == 0U);
      BOOST_TEST(resources.transient.outbound_connections == 0U);
      BOOST_TEST(resources.peers.outbound_connections == 0U);
      BOOST_TEST(resources.system.outbound_connections == 0U);
      BOOST_TEST(resources.system.file_descriptors == 0U);
      BOOST_TEST(resources.denied_scope_migrations >= 1U);

      forge::asio::blocking::run(runtime, client.async_stop());
      forge::asio::blocking::run(runtime, server.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_known_peer_dial_limit_rejects_second_tcp_attempt_before_transport_effects) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto target_peer = peer(248);
   auto client_gater = std::make_shared<recording_connection_gater>();
   auto client_options = options_for(peer(247));
   client_options.connection_gater = client_gater;
   client_options.limits.resources.max_dial_attempts_per_peer = 1;
   auto client = node{runtime, std::move(client_options)};
   auto first_accepted = std::make_shared<std::promise<void>>();
   auto first_accepted_future = first_accepted->get_future();
   auto accepted_count = std::make_shared<std::atomic_size_t>(0U);
   const auto endpoint = start_stalling_tcp_peer(runtime, std::chrono::seconds{2}, first_accepted, accepted_count);
   const auto options = node::connect_options{
       .expected_peer = target_peer,
       .allow_relay = false,
       .timeout = std::chrono::seconds{5},
   };

   auto first = boost::asio::co_spawn(runtime.context(), client.async_connect(endpoint, options), boost::asio::use_future);
   BOOST_REQUIRE(first_accepted_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto staged = client.diagnostics().resources;
   BOOST_TEST(staged.active_dials == 1U);
   BOOST_TEST(staged.transient.outbound_connections == 1U);
   BOOST_TEST(staged.system.outbound_connections == 1U);
   BOOST_TEST(staged.system.file_descriptors == 1U);

   auto rejected = false;
   try {
      static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(endpoint, options)));
   } catch (const forge::exceptions::base& error) {
      rejected = exceptions::code_of(error) == exceptions::code::backpressure_rejected;
   }

   wait_on_runtime(runtime, std::chrono::milliseconds{20}, "second TCP accept observation");
   BOOST_TEST(rejected);
   BOOST_TEST(accepted_count->load(std::memory_order_relaxed) == 1U);
   // Peer gating precedes per-peer permit binding; a rejected binding never
   // launches a numeric candidate or reaches its address gater.
   check_gater_stages(client_gater->stages_for(target_peer),
                      {recorded_gater_stage::peer_dial, recorded_gater_stage::address_dial,
                       recorded_gater_stage::peer_dial});

   forge::asio::blocking::run(runtime, client.async_stop());
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   auto first_stopped = false;
   try {
      static_cast<void>(first.get());
   } catch (...) {
      first_stopped = true;
   }
   BOOST_TEST(first_stopped);

   const auto released_by = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while ((client.diagnostics().resources.active_dials != 0U ||
           client.diagnostics().resources.transient.outbound_connections != 0U ||
           client.diagnostics().resources.system.file_descriptors != 0U) &&
          std::chrono::steady_clock::now() < released_by) {
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "staged direct attempt resource release");
   }
   const auto resources = client.diagnostics().resources;
   BOOST_TEST(resources.active_dials == 0U);
   BOOST_TEST(resources.transient.outbound_connections == 0U);
   BOOST_TEST(resources.system.outbound_connections == 0U);
   BOOST_TEST(resources.system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_tcp_session_fd_reservation_survives_tls_handoff_until_transport_close) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(199))};
   auto client = node{runtime, options_for(peer(200))};

   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));
   const auto endpoint = require_endpoint_for(server.local_endpoints(), endpoint::protocol_kind::tcp);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(endpoint, node::connect_options{.expected_peer = server.local_peer()})));

   BOOST_TEST(client.diagnostics().resources.system.file_descriptors == 1U);
   forge::asio::blocking::run(runtime, client.async_stop());
   BOOST_TEST(client.diagnostics().resources.system.file_descriptors == 0U);

   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_gater_inbound_denials_leave_no_server_state_for_tcp_and_quic) {
   for (const auto transport : {endpoint::protocol_kind::tcp, endpoint::protocol_kind::quic_v1}) {
      for (const auto rejected :
           {recorded_gater_stage::accept, recorded_gater_stage::secured, recorded_gater_stage::upgraded}) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
         auto server_gater = std::make_shared<recording_connection_gater>(rejected);
         auto server_options = options_for(peer(197));
         auto client_options = options_for(peer(198));
         server_options.connection_gater = server_gater;
         auto server = node{runtime, std::move(server_options)};
         auto client = node{runtime, std::move(client_options)};

         forge::asio::blocking::run(runtime, server.async_listen(transport == endpoint::protocol_kind::tcp
                                                                     ? make_tcp_endpoint(0)
                                                                     : make_quic_endpoint(0)));
         const auto server_endpoint = require_endpoint_for(server.local_endpoints(), transport);
         const auto resources_before = server.diagnostics().resources;
         try {
            static_cast<void>(forge::asio::blocking::run(
                runtime,
                client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
         } catch (const forge::exceptions::base&) {
            // TCP reports a remote close during connect. QUIC may complete the
            // authenticated dial before the server-side Accept hook runs.
         }

         const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
         while (gater_rejections(server.metrics(), rejected) == 0U && std::chrono::steady_clock::now() < deadline) {
            wait_on_runtime(runtime, std::chrono::milliseconds{1}, "P2P inbound connection gater rejection");
         }
         BOOST_TEST(gater_rejections(server.metrics(), rejected) == 1U);
         const auto stages = server_gater->stages();
         const auto expected_count = inbound_gater_stage_count(rejected);
         BOOST_REQUIRE_EQUAL(stages.size(), expected_count);
         BOOST_TEST(static_cast<int>(stages.back()) == static_cast<int>(rejected));
         BOOST_TEST(server.metrics().active_sessions == 0U);
         BOOST_TEST(server.diagnostics().sessions.empty());
         BOOST_TEST(!server.peers().find(client.local_peer()).has_value());
         if (transport == endpoint::protocol_kind::tcp && rejected == recorded_gater_stage::accept) {
            const auto resources = server.diagnostics().resources;
            BOOST_TEST(resources.transient.inbound_connections == resources_before.transient.inbound_connections);
            BOOST_TEST(resources.system.inbound_connections == resources_before.system.inbound_connections);
            BOOST_TEST(resources.transient.file_descriptors == resources_before.transient.file_descriptors);
            BOOST_TEST(resources.system.file_descriptors == resources_before.system.file_descriptors);
         }

         forge::asio::blocking::run(runtime, client.async_stop());
         forge::asio::blocking::run(runtime, server.async_stop());
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_connection_gater_accept_denial_does_not_retire_direct_listener) {
   for (const auto transport : {endpoint::protocol_kind::tcp, endpoint::protocol_kind::quic_v1}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto server_gater = std::make_shared<recording_connection_gater>(recorded_gater_stage::accept);
      auto server_options = options_for(peer(201));
      auto client_options = options_for(peer(202));
      server_options.connection_gater = server_gater;
      auto server = node{runtime, std::move(server_options)};
      auto client = node{runtime, std::move(client_options)};

      forge::asio::blocking::run(runtime, server.async_listen(transport == endpoint::protocol_kind::tcp
                                                                  ? make_tcp_endpoint(0)
                                                                  : make_quic_endpoint(0)));
      const auto server_endpoint = require_endpoint_for(server.local_endpoints(), transport);
      try {
         static_cast<void>(forge::asio::blocking::run(
             runtime,
             client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
      } catch (const forge::exceptions::base&) {
         // A TCP dial observes the remote close synchronously. A QUIC dial may
         // complete before the server-side Accept hook rejects its connection.
      }

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (gater_rejections(server.metrics(), recorded_gater_stage::accept) == 0U &&
             std::chrono::steady_clock::now() < deadline) {
         wait_on_runtime(runtime, std::chrono::milliseconds{1}, "P2P inbound accept gater rejection");
      }
      BOOST_TEST(gater_rejections(server.metrics(), recorded_gater_stage::accept) == 1U);
      server_gater->allow();

      static_cast<void>(forge::asio::blocking::run(
          runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
      const auto established_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (server.metrics().active_sessions != 1U && std::chrono::steady_clock::now() < established_deadline) {
         wait_on_runtime(runtime, std::chrono::milliseconds{1}, "P2P direct listener recovery after gater denial");
      }
      BOOST_TEST(server.metrics().active_sessions == 1U);

      forge::asio::blocking::run(runtime, client.async_stop());
      forge::asio::blocking::run(runtime, server.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_tcp_handshake_is_admitted_before_security_upgrade) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = options_for(peer(199));
   server_options.limits.resources.transient.max_inbound_connections = 1;
   server_options.limits.resources.transient.max_connections = 1;
   auto server = node{runtime, std::move(server_options)};

   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));
   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));
   auto listeners = server.local_endpoints();
   std::erase_if(listeners, [](const endpoint& value) { return !value.is_direct_tcp(); });
   BOOST_REQUIRE_EQUAL(listeners.size(), 2U);

   auto first = boost::asio::ip::tcp::socket{runtime.context()};
   auto second = boost::asio::ip::tcp::socket{runtime.context()};
   const auto connect_raw = [](boost::asio::ip::tcp::socket& socket,
                               const endpoint& target) -> boost::asio::awaitable<void> {
      co_await socket.async_connect(
          boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address(target.transport.host), target.transport.port},
          boost::asio::use_awaitable);
   };

   forge::asio::blocking::run(runtime, connect_raw(first, listeners[0]));
   const auto admitted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (server.diagnostics().resources.transient.inbound_connections != 1U) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < admitted_deadline);
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "first raw TCP admission");
   }

   forge::asio::blocking::run(runtime, connect_raw(second, listeners[1]));
   const auto rejected_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (server.diagnostics().resources.denied_connections == 0U) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < rejected_deadline);
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "second raw TCP admission rejection");
   }
   const auto bounded = server.diagnostics().resources;
   BOOST_TEST(bounded.transient.inbound_connections == 1U);
   BOOST_TEST(bounded.denied_connections >= 1U);

   auto ignored = boost::system::error_code{};
   first.close(ignored);
   second.close(ignored);
   forge::asio::blocking::run(runtime, server.async_stop());
   BOOST_TEST(server.diagnostics().resources.transient.inbound_connections == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_duplicate_direct_listen_rejects_typed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto value = node{runtime, options_for(peer(212))};

   forge::asio::blocking::run(runtime, value.async_listen(make_tcp_endpoint(0)));
   const auto local = require_endpoint_for(value.local_endpoints(), endpoint::protocol_kind::tcp);

   try {
      forge::asio::blocking::run(runtime, value.async_listen(local));
      BOOST_FAIL("expected duplicate listen rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::invalid_options));
   }

   forge::asio::blocking::run(runtime, value.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_preserves_multiple_direct_endpoints_without_duplicates) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("peer-exchange-multi-server");
   const auto client_identity = make_test_certificate_identity("peer-exchange-multi-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));
   const auto advertised = server.local_endpoints();
   BOOST_REQUIRE_EQUAL(advertised.size(), 2U);

   const auto quic = require_endpoint_for(advertised, endpoint::protocol_kind::quic_v1);
   client.peers().learn_endpoint(server.local_peer(), quic,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::peer_exchange});
   forge::asio::blocking::run(runtime, client.async_request_peer_exchange(server.local_peer()));
   forge::asio::blocking::run(runtime, client.async_request_peer_exchange(server.local_peer()));

   const auto learned = client.peers().find(server.local_peer());
   BOOST_REQUIRE(learned);
   auto learned_endpoints = std::vector<endpoint>{};
   learned_endpoints.reserve(learned->endpoints.size());
   for (const auto& item : learned->endpoints) {
      learned_endpoints.push_back(parse_endpoint(item.address.to_string()));
   }
   BOOST_TEST(contains_protocol(learned_endpoints, endpoint::protocol_kind::quic_v1));
   BOOST_TEST(contains_protocol(learned_endpoints, endpoint::protocol_kind::tcp));
   auto seen = std::set<std::string>{};
   for (const auto& item : learned->endpoints) {
      BOOST_TEST(peer_for(item.address).to_bytes() == server.local_peer().to_bytes(), boost::test_tools::per_element());
      BOOST_TEST(seen.insert(item.address.to_string()).second);
   }

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_negotiates_the_smaller_receive_limit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("peer-exchange-large-receive-limit");
   const auto client_identity = make_test_certificate_identity("peer-exchange-small-receive-limit");
   auto server_options = options_for(server_identity);
   server_options.limits.max_peer_exchange_message_size = 1024;
   auto client_options = options_for(client_identity);
   constexpr auto client_receive_limit = std::size_t{256};
   client_options.limits.max_peer_exchange_message_size = client_receive_limit;
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));
   const auto server_endpoint = require_endpoint_for(server.local_endpoints(), endpoint::protocol_kind::quic_v1);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::peer_exchange});

   forge::asio::blocking::run(runtime, client.async_request_peer_exchange(server.local_peer()));

   BOOST_TEST(client.metrics().peer_exchange_messages >= 1U);
   BOOST_TEST(server.metrics().peer_exchange_messages >= 1U);
   const auto negotiated = peer_exchange_codec::negotiate_response_max_frame_size(
       client_receive_limit, peer_exchange_codec::options{.max_message_size = 1024});
   BOOST_TEST(negotiated == client_receive_limit);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_timeout_cancels_only_stalled_yamux_stream) {
   auto server_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{server_runtime, options_for(make_test_identity())};
   auto client_options = options_for(make_test_identity());
   client_options.limits.topology.query_timeout = std::chrono::milliseconds{100};
   auto client = node{client_runtime, std::move(client_options)};
   register_echo(server);

   const auto server_endpoint = listen_tcp(server, server_runtime);
   static_cast<void>(forge::asio::blocking::run(
       client_runtime,
       client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   const auto sessions_opened = client.metrics().sessions_opened;
   auto release_server = block_runtime(server_runtime, "peer exchange Yamux multistream negotiation barrier");
   auto request = boost::asio::co_spawn(
       client_runtime.context(), client.async_request_peer_exchange(server.local_peer()), boost::asio::use_future);
   const auto canceled = request.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
   release_server->set_value();
   BOOST_REQUIRE(canceled);
   try {
      request.get();
      BOOST_FAIL("expected stalled peer exchange Yamux negotiation timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }

   auto sibling = forge::asio::blocking::run(
       client_runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                         node::open_options{
                                                             .allow_relay = false,
                                                             .timeout = std::chrono::seconds{2},
                                                             .direct_attempt_timeout = std::chrono::seconds{2},
                                                             .max_direct_endpoints = 1,
                                                         }));
   const auto payload = std::vector<std::uint8_t>{'p', 'e', 'x'};
   forge::asio::blocking::run(client_runtime, sibling.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(client_runtime, sibling.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   forge::asio::blocking::run(client_runtime, sibling.async_close());
   BOOST_TEST(client.metrics().sessions_opened == sessions_opened);

   forge::asio::blocking::run(client_runtime, client.async_stop());
   forge::asio::blocking::run(server_runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_receive_limits_are_typed_and_v1_bounded) {
   const auto local = peer_exchange_codec::options{.max_message_size = 1024};
   BOOST_TEST(peer_exchange_message{}.max_frame_size == peer_exchange_codec::options{}.max_message_size);
   BOOST_TEST(peer_exchange_codec::negotiate_response_max_frame_size(512, local) == 512U);
   BOOST_TEST(peer_exchange_codec::negotiate_response_max_frame_size(2048, local) == local.max_message_size);
   BOOST_CHECK_THROW(static_cast<void>(peer_exchange_codec::negotiate_response_max_frame_size(0, local)),
                     exceptions::codec_error);
   BOOST_CHECK_THROW(static_cast<void>(peer_exchange_codec::negotiate_response_max_frame_size(
                         peer_exchange_codec::minimum_message_size - 1, local)),
                     exceptions::codec_error);
   BOOST_CHECK_THROW(static_cast<void>(peer_exchange_codec::negotiate_response_max_frame_size(
                         static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1, local)),
                     exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_codec_preserves_dnsaddr) {
   const auto remote = peer(220);
   const auto address = forge::multiformats::multiaddr::parse("/dnsaddr/pex.example/p2p/" + remote.to_string());
   const auto message = peer_exchange_message{
       .kind = peer_exchange_message::type::peer_exchange_response,
       .peer = peer(219),
       .endpoints = {peer_exchange_message::endpoint_record{.peer = remote, .address = address}},
   };

   const auto decoded = peer_exchange_codec::decode(peer_exchange_codec::encode(message));
   BOOST_REQUIRE_EQUAL(decoded.endpoints.size(), 1U);
   BOOST_TEST(decoded.endpoints.front().address.to_string() == address.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_codec_rejects_malformed_multiaddr_endpoint_record) {
   const auto remote = peer(220);
   const auto address = forge::multiformats::multiaddr::parse("/dnsaddr/pex.example/p2p/" + remote.to_string());
   const auto message = peer_exchange_message{
       .kind = peer_exchange_message::type::peer_exchange_response,
       .peer = peer(219),
       .endpoints = {peer_exchange_message::endpoint_record{.peer = remote, .address = address}},
   };
   auto encoded = peer_exchange_codec::encode(message);
   const auto address_text = address.to_string();
   const auto encoded_address = std::search(encoded.begin(), encoded.end(), address_text.begin(), address_text.end());
   BOOST_REQUIRE(encoded_address != encoded.end());
   *encoded_address = static_cast<std::uint8_t>('x');

   BOOST_CHECK_THROW((void)peer_exchange_codec::decode(encoded), exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_rejects_spoofed_response_identity_without_mutating_victim) {
   auto store = peer_store{};
   const auto authenticated_peer = peer(218);
   const auto victim = peer(219);
   auto original_endpoint = make_tcp_endpoint(4001, "8.8.8.8");
   original_endpoint.peer = victim;
   store.upsert(peer_store::record{
       .peer = victim,
       .capabilities = capability_set{.bits = capabilities::peer_exchange},
   });
   store.learn_endpoint(victim, original_endpoint, capability_set{.bits = capabilities::peer_exchange});
   const auto before = store.find(victim);
   BOOST_REQUIRE(before);

   auto spoofed_endpoint = make_tcp_endpoint(4002, "1.1.1.1");
   spoofed_endpoint.peer = victim;
   const auto response = peer_exchange_message{
       .kind = peer_exchange_message::type::peer_exchange_response,
       .peer = victim,
       .capabilities = capability_set{.bits = capabilities::pubsub},
       .endpoints = {{
           .peer = victim,
           .address = address_for(spoofed_endpoint),
           .capabilities = capability_set{.bits = capabilities::pubsub},
       }},
   };
   try {
      detail::learn_authenticated_peer_exchange_response(store, response, authenticated_peer);
      BOOST_FAIL("expected spoofed peer exchange identity rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::peer_verification_failed));
   }

   const auto after = store.find(victim);
   BOOST_REQUIRE(after);
   BOOST_TEST(after->capabilities.bits == before->capabilities.bits);
   BOOST_REQUIRE_EQUAL(after->endpoints.size(), before->endpoints.size());
   BOOST_TEST(after->endpoints.front().address.to_string() == before->endpoints.front().address.to_string());
   BOOST_TEST(!store.find(authenticated_peer).has_value());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_keeps_third_party_capabilities_untrusted) {
   auto store = peer_store{};
   const auto authenticated_peer = peer(219);
   const auto third_party = peer(220);
   auto third_party_endpoint = make_tcp_endpoint(4001, "8.8.4.4");
   third_party_endpoint.peer = third_party;
   store.upsert(peer_store::record{
       .peer = authenticated_peer,
       .capabilities = capability_set{.bits = capabilities::direct_quic},
   });

   detail::learn_authenticated_peer_exchange_response(
       store,
       peer_exchange_message{
           .kind = peer_exchange_message::type::peer_exchange_response,
           .peer = authenticated_peer,
           .capabilities = capability_set{.bits = capabilities::peer_exchange},
           .endpoints = {peer_exchange_message::endpoint_record{
               .peer = third_party,
               .address = address_for(third_party_endpoint),
               .capabilities = capability_set{.bits = capabilities::peer_exchange | capabilities::pubsub},
           }},
       },
       authenticated_peer);

   const auto responder = store.find(authenticated_peer);
   BOOST_REQUIRE(responder);
   BOOST_TEST(responder->capabilities.has(capabilities::direct_quic));
   BOOST_TEST(!responder->capabilities.has(capabilities::peer_exchange));

   const auto learned = store.find(third_party);
   BOOST_REQUIRE(learned);
   BOOST_TEST(!learned->capabilities.has(capabilities::peer_exchange));
   BOOST_TEST(!learned->capabilities.has(capabilities::pubsub));
   BOOST_TEST(static_cast<int>(learned->discovered_by) == static_cast<int>(discovery::source::peer_exchange));
   BOOST_REQUIRE_EQUAL(learned->endpoints.size(), 1U);
   BOOST_TEST(learned->endpoints.front().address.to_string() == third_party_endpoint.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scheduler_requires_exact_identify_protocol_and_bounds_singleflight) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
   const auto exact = detail::peer_exchange_scheduler::session{
       .peer = peer(221),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .capabilities = capability_set{.bits = capabilities::peer_exchange},
       .protocols = {builtins::peer_exchange},
   };
   const auto capability_only = detail::peer_exchange_scheduler::session{
       .peer = peer(222),
       .session_id = 2,
       .identify_state = identify::state::identified,
       .capabilities = capability_set{.bits = capabilities::peer_exchange},
       .protocols = {builtins::identify},
   };
   const auto lookalike = detail::peer_exchange_scheduler::session{
       .peer = peer(223),
       .session_id = 3,
       .identify_state = identify::state::identified,
       .capabilities = capability_set{.bits = capabilities::peer_exchange},
       .protocols = {protocol_id{.value = "/forge/p2p/peer-exchange/1/extra"}},
   };
   auto scheduler = detail::peer_exchange_scheduler{};

   BOOST_TEST(detail::peer_exchange_scheduler::eligible(exact));
   BOOST_TEST(!detail::peer_exchange_scheduler::eligible(capability_only));
   BOOST_TEST(!detail::peer_exchange_scheduler::eligible(lookalike));

   const auto candidates = std::vector<detail::peer_exchange_scheduler::session>{exact, capability_only, lookalike};
   auto first = scheduler.claim_peer(exact.peer, candidates, now, 1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(first.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::started));
   auto joined = scheduler.claim_peer(exact.peer, candidates, now, 1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(joined.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::joined));
   auto batch = scheduler.claim_batch(candidates, now, 1, 1, runtime.context().get_executor());
   BOOST_REQUIRE_EQUAL(batch.size(), 1U);
   BOOST_TEST(static_cast<int>(batch.front().status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::joined));
   BOOST_TEST(scheduler.size() == 1U);

   scheduler.fail(first, exceptions::code::timeout, "test peer exchange failure", now, std::chrono::seconds{10});
   scheduler.leave(joined);
   scheduler.leave(batch.front());
   scheduler.leave(first);
   auto blocked =
       scheduler.claim_peer(exact.peer, candidates, now + std::chrono::seconds{1}, 1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(blocked.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::backoff));
   const auto blocked_batch =
       scheduler.claim_batch(candidates, now + std::chrono::seconds{1}, 1, 1, runtime.context().get_executor());
   BOOST_TEST(blocked_batch.empty());

   auto retried = scheduler.claim_peer(exact.peer, candidates, now + std::chrono::seconds{10}, 1,
                                       runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(retried.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::started));
   scheduler.succeed(retried, now + std::chrono::seconds{10}, std::chrono::seconds{10});
   scheduler.leave(retried);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scheduler_saturates_maximum_success_and_failure_backoffs) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
   const auto maximum = (std::chrono::milliseconds::max)();
   const auto success_candidate = detail::peer_exchange_scheduler::session{
       .peer = peer(235),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };
   const auto failure_candidate = detail::peer_exchange_scheduler::session{
       .peer = peer(236),
       .session_id = 2,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };

   auto success_scheduler = detail::peer_exchange_scheduler{};
   auto successful = success_scheduler.claim_peer(success_candidate.peer, {success_candidate}, now, 1,
                                                  runtime.context().get_executor());
   BOOST_TEST(successful.started());
   success_scheduler.succeed(successful, now, maximum);
   success_scheduler.leave(successful);
   const auto success_blocked = success_scheduler.claim_peer(success_candidate.peer, {success_candidate}, now, 1,
                                                             runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(success_blocked.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::backoff));
   const auto success_before_max = success_scheduler.claim_peer(success_candidate.peer, {success_candidate},
                                                                std::chrono::steady_clock::time_point::max() -
                                                                    std::chrono::steady_clock::duration{1},
                                                                1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(success_before_max.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::backoff));
   auto success_retry =
       success_scheduler.claim_peer(success_candidate.peer, {success_candidate},
                                    std::chrono::steady_clock::time_point::max(), 1, runtime.context().get_executor());
   BOOST_TEST(success_retry.started());
   success_scheduler.succeed(success_retry, std::chrono::steady_clock::time_point::max(),
                             std::chrono::milliseconds::zero());
   success_scheduler.leave(success_retry);

   auto failure_scheduler = detail::peer_exchange_scheduler{};
   auto failed = failure_scheduler.claim_peer(failure_candidate.peer, {failure_candidate}, now, 1,
                                              runtime.context().get_executor());
   BOOST_TEST(failed.started());
   failure_scheduler.fail(failed, exceptions::code::timeout, {}, now, maximum);
   failure_scheduler.leave(failed);
   const auto failure_blocked = failure_scheduler.claim_peer(failure_candidate.peer, {failure_candidate}, now, 1,
                                                             runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(failure_blocked.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::backoff));
   const auto failure_before_max = failure_scheduler.claim_peer(failure_candidate.peer, {failure_candidate},
                                                                std::chrono::steady_clock::time_point::max() -
                                                                    std::chrono::steady_clock::duration{1},
                                                                1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(failure_before_max.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::backoff));
   auto failure_retry =
       failure_scheduler.claim_peer(failure_candidate.peer, {failure_candidate},
                                    std::chrono::steady_clock::time_point::max(), 1, runtime.context().get_executor());
   BOOST_TEST(failure_retry.started());
   failure_scheduler.succeed(failure_retry, std::chrono::steady_clock::time_point::max(),
                             std::chrono::milliseconds::zero());
   failure_scheduler.leave(failure_retry);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scheduler_close_is_terminal_and_bounds_waiters) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
   const auto candidate = detail::peer_exchange_scheduler::session{
       .peer = peer(230),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };
   auto scheduler = detail::peer_exchange_scheduler{1};
   auto first = scheduler.claim_peer(candidate.peer, {candidate}, now, 1, runtime.context().get_executor());
   auto joined = scheduler.claim_peer(candidate.peer, {candidate}, now, 1, runtime.context().get_executor());
   const auto rejected = scheduler.claim_peer(candidate.peer, {candidate}, now, 1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(first.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::started));
   BOOST_TEST(static_cast<int>(joined.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::joined));
   BOOST_TEST(static_cast<int>(rejected.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::backpressure));

   scheduler.leave(joined);
   auto recycled = scheduler.claim_peer(candidate.peer, {candidate}, now, 1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(recycled.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::joined));

   scheduler.close();
   const auto result = forge::asio::blocking::run(runtime, recycled.participant.wait());
   BOOST_TEST(!result.succeeded);
   BOOST_REQUIRE(result.error);
   BOOST_TEST(static_cast<int>(*result.error) == static_cast<int>(exceptions::code::closed));
   const auto after_close = scheduler.claim_peer(candidate.peer, {candidate}, now, 1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(after_close.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::closed));
   BOOST_TEST(scheduler.claim_batch({candidate}, now, 1, 1, runtime.context().get_executor()).empty());
   scheduler.leave(recycled);
   scheduler.leave(first);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scheduler_entry_publish_failure_rolls_back_singleflight) {
   struct failure_state {
      bool fail_publish = true;
   };
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto state = failure_state{};
   auto scheduler = detail::peer_exchange_scheduler{
       1,
       detail::peer_exchange_scheduler::test_hooks{
           .context = &state,
           .reach =
               [](void* context, detail::peer_exchange_scheduler::test_stage stage) {
                  auto& value = *static_cast<failure_state*>(context);
                  if (stage == detail::peer_exchange_scheduler::test_stage::before_entry_publish &&
                      value.fail_publish) {
                     throw std::bad_alloc{};
                  }
               },
       },
   };
   const auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
   const auto candidate = detail::peer_exchange_scheduler::session{
       .peer = peer(232),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };

   BOOST_CHECK_THROW(static_cast<void>(scheduler.claim_next({candidate}, now, 1, runtime.context().get_executor())),
                     std::bad_alloc);
   BOOST_TEST(scheduler.size() == 0U);

   state.fail_publish = false;
   auto retry = scheduler.claim_next({candidate}, now, 1, runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(retry.status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::started));
   scheduler.succeed(retry, now, std::chrono::seconds{1});
   scheduler.leave(retry);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scheduler_batch_failure_rolls_back_every_started_claim) {
   struct failure_state {
      std::size_t entry_publishes = 0;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto failure = failure_state{};
   auto scheduler = detail::peer_exchange_scheduler{
       1,
       detail::peer_exchange_scheduler::test_hooks{
           .context = &failure,
           .reach =
               [](void* context, detail::peer_exchange_scheduler::test_stage stage) {
                  auto& value = *static_cast<failure_state*>(context);
                  if (stage == detail::peer_exchange_scheduler::test_stage::before_entry_publish &&
                      ++value.entry_publishes == 2U) {
                     throw std::bad_alloc{};
                  }
               },
       },
   };
   const auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds{1};
   const auto first = detail::peer_exchange_scheduler::session{
       .peer = peer(233),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };
   const auto second = detail::peer_exchange_scheduler::session{
       .peer = peer(234),
       .session_id = 2,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };

   BOOST_CHECK_THROW(
       static_cast<void>(scheduler.claim_batch({first, second}, now, 2, 2, runtime.context().get_executor())),
       std::bad_alloc);

   auto retry = scheduler.claim_batch({first, second}, now, 2, 2, runtime.context().get_executor());
   BOOST_REQUIRE_EQUAL(retry.size(), 2U);
   BOOST_TEST(retry[0].started());
   BOOST_TEST(retry[1].started());
   scheduler.succeed(retry[0], now, std::chrono::seconds{1});
   scheduler.succeed(retry[1], now, std::chrono::seconds{1});
   scheduler.leave(retry[0]);
   scheduler.leave(retry[1]);
}

BOOST_AUTO_TEST_CASE(p2p_topology_peer_exchange_claims_settle_post_entry_pre_await_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = detail::peer_exchange_scheduler{};
   auto scheduler_mutex = std::mutex{};
   const auto now = std::chrono::steady_clock::now();
   const auto query_timeout = std::chrono::milliseconds{1};
   const auto candidate = detail::peer_exchange_scheduler::session{
       .peer = peer(233),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };

   const auto fail_after_claim = [&] {
      auto pending = scheduler.claim_batch({candidate}, now, 1, 1, runtime.context().get_executor());
      BOOST_REQUIRE_EQUAL(pending.size(), 1U);
      auto claim_guard =
          detail::topology_peer_exchange_claims{scheduler_mutex, scheduler, std::move(pending), query_timeout};
      auto claimed = std::make_shared<detail::topology_peer_exchange_claims>(std::move(claim_guard));
      claimed->stage();
      const auto worker_claim = claimed->staged_claims().front();
      auto worker_claims = claimed;
      claimed.reset();

      try {
         throw std::bad_alloc{};
      } catch (...) {
         // This models a parent join allocation failure after publication.
         // The worker owns the settlement state and can finish idempotently.
         worker_claims->settle_worker(*worker_claim);
         worker_claims->settle_worker(*worker_claim);
         throw;
      }
   };

   BOOST_CHECK_THROW(fail_after_claim(), std::bad_alloc);
   const auto retry_at = std::chrono::steady_clock::now() + query_timeout + std::chrono::milliseconds{1};
   auto retry = scheduler.claim_batch({candidate}, retry_at, 1, 1, runtime.context().get_executor());
   BOOST_REQUIRE_EQUAL(retry.size(), 1U);
   BOOST_TEST(static_cast<int>(retry.front().status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::started));
   scheduler.succeed(retry.front(), retry_at, std::chrono::seconds{1});
   scheduler.leave(retry.front());
}

BOOST_AUTO_TEST_CASE(p2p_topology_peer_exchange_claims_roll_back_commit_before_shared_owner_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = detail::peer_exchange_scheduler{};
   auto scheduler_mutex = std::mutex{};
   const auto now = std::chrono::steady_clock::now();
   const auto query_timeout = std::chrono::milliseconds{1};
   const auto candidate = detail::peer_exchange_scheduler::session{
       .peer = peer(234),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };

   const auto fail_before_shared_owner = [&] {
      auto pending = scheduler.claim_batch({candidate}, now, 1, 1, runtime.context().get_executor());
      BOOST_REQUIRE_EQUAL(pending.size(), 1U);
      auto claim_guard =
          detail::topology_peer_exchange_claims{scheduler_mutex, scheduler, std::move(pending), query_timeout};
      // Exact failpoint: claim_batch committed, but make_shared has not yet
      // established the worker-visible settlement owner.
      static_cast<void>(claim_guard);
      throw std::bad_alloc{};
   };

   BOOST_CHECK_THROW(fail_before_shared_owner(), std::bad_alloc);
   const auto retry_at = std::chrono::steady_clock::now() + query_timeout + std::chrono::milliseconds{1};
   auto retry = scheduler.claim_batch({candidate}, retry_at, 1, 1, runtime.context().get_executor());
   BOOST_REQUIRE_EQUAL(retry.size(), 1U);
   BOOST_TEST(static_cast<int>(retry.front().status) ==
              static_cast<int>(detail::peer_exchange_scheduler::claim_status::started));
   scheduler.fail(retry.front(), exceptions::code::closed, {}, retry_at, query_timeout);
   scheduler.leave(retry.front());
}

BOOST_AUTO_TEST_CASE(p2p_direct_peer_exchange_claim_rolls_back_commit_before_awaitable_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = detail::peer_exchange_scheduler{};
   auto scheduler_mutex = std::mutex{};
   const auto now = std::chrono::steady_clock::now();
   const auto query_timeout = std::chrono::milliseconds{1};
   const auto candidate = detail::peer_exchange_scheduler::session{
       .peer = peer(237),
       .session_id = 1,
       .identify_state = identify::state::identified,
       .protocols = {builtins::peer_exchange},
   };

   const auto fail_before_awaitable = [&] {
      auto claim_guard = detail::topology_peer_exchange_claims{
          scheduler_mutex, scheduler,
          scheduler.claim_peer(candidate.peer, {candidate}, now, 1, runtime.context().get_executor()), query_timeout};
      BOOST_TEST(claim_guard.single_claim().started());
      // Models coroutine-frame allocation for await_peer_exchange_claim after
      // claim_peer committed, with the stack rollback owner already installed.
      throw std::bad_alloc{};
   };

   BOOST_CHECK_THROW(fail_before_awaitable(), std::bad_alloc);
   auto retry = scheduler.claim_peer(candidate.peer, {candidate}, std::chrono::steady_clock::time_point::max(), 1,
                                     runtime.context().get_executor());
   BOOST_TEST(retry.started());
   scheduler.succeed(retry, std::chrono::steady_clock::time_point::max(), std::chrono::milliseconds::zero());
   scheduler.leave(retry);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_cancellation_honors_stop_before_callback_publication) {
   auto cancellation = detail::peer_exchange_cancellation{};
   auto calls = 0U;
   auto observed_stop = false;

   cancellation.reserve(1);
   cancellation.request_stop();
   cancellation.publish([&] noexcept {
      observed_stop = cancellation.stop_requested();
      ++calls;
   });
   cancellation.cancel();
   cancellation.request_stop();

   BOOST_TEST(observed_stop);
   BOOST_TEST(calls == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_cancellation_honors_cancel_before_callback_publication) {
   auto cancellation = detail::peer_exchange_cancellation{};
   auto calls = 0U;

   cancellation.reserve(1);
   cancellation.cancel();
   cancellation.publish([&] noexcept { ++calls; });
   cancellation.cancel();

   BOOST_TEST(!cancellation.stop_requested());
   BOOST_TEST(calls == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_cancellation_emits_stop_after_worker_slot_binds) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto cancellation = detail::peer_exchange_cancellation{};
   cancellation.reserve(1);
   auto signal = std::make_shared<boost::asio::cancellation_signal>();
   auto timer = std::make_shared<boost::asio::steady_timer>(runtime.context());
   timer->expires_at((boost::asio::steady_timer::time_point::max)());
   auto bound = std::promise<void>{};
   auto bound_future = bound.get_future();

   auto worker = boost::asio::co_spawn(
       runtime.context(),
       [timer, &bound]() -> boost::asio::awaitable<bool> {
          bound.set_value();
          auto error = boost::system::error_code{};
          co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
          co_return error == boost::asio::error::operation_aborted;
       },
       boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_future));
   bound_future.wait();

   cancellation.request_stop();
   cancellation.publish([signal] noexcept { signal->emit(boost::asio::cancellation_type::terminal); });

   BOOST_TEST(worker.get());
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_honors_stop_before_worker_start) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto started = false;

   stop->request_stop();
   auto worker = boost::asio::co_spawn(
       runtime.context(),
       detail::async_run_with_stop_bridge(
           stop,
           [&started](std::shared_ptr<detail::worker_terminal_owner>) -> boost::asio::awaitable<void> {
              started = true;
              co_return;
           }),
       boost::asio::use_future);

   BOOST_CHECK_NO_THROW(worker.get());
   BOOST_TEST(!started);
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_inherits_caller_cancellation) {
   namespace asio = boost::asio;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto entered = std::make_shared<forge::asio::notification>();
   auto cancellation_count = std::atomic_size_t{};
   const auto entered_epoch = entered->epoch();
   auto signal = asio::cancellation_signal{};

   auto worker = asio::co_spawn(
       runtime.context(),
       detail::async_run_with_stop_bridge(
           stop,
           [entered,
            &cancellation_count](std::shared_ptr<detail::worker_terminal_owner> terminal) -> asio::awaitable<void> {
              const auto executor = co_await asio::this_coro::executor;
              auto wait = std::make_shared<asio::steady_timer>(executor, asio::steady_timer::time_point::max());
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait, &cancellation_count]() noexcept {
                     cancellation_count.fetch_add(1, std::memory_order_acq_rel);
                     cancel_timer_noexcept(wait);
                  },
              }));
              entered->notify();
              auto error = boost::system::error_code{};
              co_await wait->async_wait(asio::redirect_error(asio::use_awaitable, error));
              BOOST_TEST(error == asio::error::operation_aborted);
           }),
       asio::bind_cancellation_slot(signal.slot(), asio::use_future));

   static_cast<void>(asio::co_spawn(runtime.context(), entered->async_wait(entered_epoch), asio::use_future).get());
   signal.emit(asio::cancellation_type::terminal);

   BOOST_REQUIRE(worker.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(worker.get());
   BOOST_TEST(stop->stop_requested());
   BOOST_TEST(cancellation_count.load(std::memory_order_acquire) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_serializes_stop_on_the_worker_strand) {
   namespace asio = boost::asio;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto worker_strand = asio::make_strand(runtime.context());
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto entered = std::make_shared<forge::asio::notification>();
   auto canceled = std::make_shared<forge::asio::notification>();
   auto started_on_strand = std::atomic_bool{false};
   auto canceled_on_strand = std::atomic_bool{false};
   auto cancellation_count = std::atomic_size_t{};

   const auto entered_epoch = entered->epoch();
   auto worker = asio::co_spawn(
       worker_strand,
       detail::async_run_with_stop_bridge(
           stop,
           [worker_strand, entered, canceled, &started_on_strand, &canceled_on_strand,
            &cancellation_count](std::shared_ptr<detail::worker_terminal_owner> terminal) -> asio::awaitable<void> {
              const auto executor = co_await asio::this_coro::executor;
              started_on_strand.store(worker_strand.running_in_this_thread(), std::memory_order_release);
              auto wait = std::make_shared<asio::steady_timer>(executor, asio::steady_timer::time_point::max());
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait, worker_strand, canceled, &canceled_on_strand, &cancellation_count]() noexcept {
                     canceled_on_strand.store(worker_strand.running_in_this_thread(), std::memory_order_release);
                     cancellation_count.fetch_add(1, std::memory_order_acq_rel);
                     cancel_timer_noexcept(wait);
                     canceled->notify();
                  },
              }));
              entered->notify();
              auto error = boost::system::error_code{};
              co_await wait->async_wait(asio::redirect_error(asio::use_awaitable, error));
              BOOST_TEST(error == asio::error::operation_aborted);
           }),
       asio::use_future);

   static_cast<void>(asio::co_spawn(runtime.context(), entered->async_wait(entered_epoch), asio::use_future).get());
   const auto canceled_epoch = canceled->epoch();
   stop->request_stop();
   static_cast<void>(asio::co_spawn(runtime.context(), canceled->async_wait(canceled_epoch), asio::use_future).get());
   BOOST_CHECK_NO_THROW(worker.get());
   BOOST_TEST(started_on_strand.load(std::memory_order_acquire));
   BOOST_TEST(canceled_on_strand.load(std::memory_order_acquire));
   BOOST_TEST(cancellation_count.load(std::memory_order_acquire) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_lifecycle_stop_after_both_branches_start_cancels_and_joins) {
   namespace asio = boost::asio;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto operation = lifecycle.track();
   BOOST_REQUIRE(operation.active());

   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto stop_wait_started = std::make_shared<forge::asio::notification>();
   auto work_started = std::make_shared<forge::asio::notification>();
   auto cancellation_count = std::atomic_size_t{};
   auto work_completed = std::atomic_bool{false};
   const auto stop_wait_epoch = stop_wait_started->epoch();
   const auto work_epoch = work_started->epoch();

   auto worker = asio::co_spawn(
       operation.executor(),
       detail::async_run_with_stop_bridge(
           stop,
           [work_started, &cancellation_count,
            &work_completed](std::shared_ptr<detail::worker_terminal_owner> terminal) -> asio::awaitable<void> {
              const auto executor = co_await asio::this_coro::executor;
              auto wait = std::make_shared<asio::steady_timer>(executor, asio::steady_timer::time_point::max());
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait, &cancellation_count]() noexcept {
                     cancellation_count.fetch_add(1, std::memory_order_acq_rel);
                     cancel_timer_noexcept(wait);
                  },
              }));
              work_started->notify();
              auto error = boost::system::error_code{};
              co_await wait->async_wait(asio::redirect_error(asio::use_awaitable, error));
              BOOST_TEST(error == asio::error::operation_aborted);
              work_completed.store(true, std::memory_order_release);
           },
           detail::worker_stop_bridge_options{
               .lifecycle_stop = operation.stop_source(),
               .before_stop_wait = [stop_wait_started] { stop_wait_started->notify(); },
           }),
       asio::use_future);

   static_cast<void>(
       asio::co_spawn(runtime.context(), stop_wait_started->async_wait(stop_wait_epoch), asio::use_future).get());
   static_cast<void>(asio::co_spawn(runtime.context(), work_started->async_wait(work_epoch), asio::use_future).get());
   BOOST_TEST(!stop->stop_requested());

   lifecycle.request_stop();

   BOOST_REQUIRE(worker.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(worker.get());
   BOOST_TEST(stop->stop_requested());
   BOOST_TEST(work_completed.load(std::memory_order_acquire));
   BOOST_TEST(cancellation_count.load(std::memory_order_acquire) == 1U);

   operation.release();
   auto drained = asio::co_spawn(runtime.context(), lifecycle.wait(), asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drained.get());
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_stop_before_terminal_publication_is_sticky) {
   namespace asio = boost::asio;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto worker_strand = asio::make_strand(runtime.context());
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto entered = std::make_shared<forge::asio::notification>();
   auto release_publication = std::make_shared<forge::asio::notification>();
   auto cancellation_count = std::atomic_size_t{};
   auto canceled_on_strand = std::atomic_bool{false};

   const auto entered_epoch = entered->epoch();
   const auto release_epoch = release_publication->epoch();
   auto worker = asio::co_spawn(
       worker_strand,
       detail::async_run_with_stop_bridge(
           stop,
           [worker_strand, entered, release_publication, release_epoch, &cancellation_count,
            &canceled_on_strand](std::shared_ptr<detail::worker_terminal_owner> terminal) -> asio::awaitable<void> {
              entered->notify();
              static_cast<void>(co_await release_publication->async_wait(release_epoch));
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [worker_strand, &cancellation_count, &canceled_on_strand]() noexcept {
                     canceled_on_strand.store(worker_strand.running_in_this_thread(), std::memory_order_release);
                     cancellation_count.fetch_add(1, std::memory_order_acq_rel);
                  },
              }));
              BOOST_TEST(terminal->stop_requested());
           }),
       asio::use_future);

   static_cast<void>(asio::co_spawn(runtime.context(), entered->async_wait(entered_epoch), asio::use_future).get());
   stop->request_stop();
   release_publication->notify();
   BOOST_CHECK_NO_THROW(worker.get());
   BOOST_TEST(cancellation_count.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(canceled_on_strand.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_natural_completion_seals_terminal_callback_once) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto cancellation_count = std::atomic_size_t{};
   auto completion_count = std::atomic_size_t{};

   auto worker = boost::asio::co_spawn(
       runtime.context(),
       detail::async_run_with_stop_bridge(
           stop,
           [&cancellation_count, &completion_count](
               std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [&cancellation_count]() noexcept { cancellation_count.fetch_add(1, std::memory_order_acq_rel); },
              }));
              completion_count.fetch_add(1, std::memory_order_acq_rel);
              co_return;
           }),
       boost::asio::use_future);

   BOOST_CHECK_NO_THROW(worker.get());
   stop->request_stop();
   BOOST_TEST(completion_count.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(cancellation_count.load(std::memory_order_acquire) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_does_not_own_arbitrary_throwing_asio_handler) {
   auto signal = boost::asio::cancellation_signal{};
   signal.slot().assign([](boost::asio::cancellation_type) { throw std::runtime_error{"unrelated handler"}; });
   BOOST_CHECK_THROW(signal.emit(boost::asio::cancellation_type::terminal), std::runtime_error);

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto entered = std::make_shared<forge::asio::notification>();
   auto cancellation_count = std::atomic_size_t{};
   const auto entered_epoch = entered->epoch();
   auto worker = boost::asio::co_spawn(
       runtime.context(),
       detail::async_run_with_stop_bridge(
           stop,
           [entered, &cancellation_count](
               std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              const auto executor = co_await boost::asio::this_coro::executor;
              auto wait =
                  std::make_shared<boost::asio::steady_timer>(executor, boost::asio::steady_timer::time_point::max());
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait, &cancellation_count]() noexcept {
                     cancellation_count.fetch_add(1, std::memory_order_acq_rel);
                     cancel_timer_noexcept(wait);
                  },
              }));
              entered->notify();
              auto error = boost::system::error_code{};
              co_await wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
              BOOST_TEST(error == boost::asio::error::operation_aborted);
           }),
       boost::asio::use_future);

   static_cast<void>(
       boost::asio::co_spawn(runtime.context(), entered->async_wait(entered_epoch), boost::asio::use_future).get());
   stop->request_stop();
   BOOST_CHECK_NO_THROW(worker.get());
   BOOST_TEST(cancellation_count.load(std::memory_order_acquire) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_worker_stop_bridge_joins_the_published_stop_waiter_after_work_setup_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto work_started = false;

   auto worker = boost::asio::co_spawn(
       runtime.context(),
       detail::async_run_with_stop_bridge(
           stop,
           [&work_started](std::shared_ptr<detail::worker_terminal_owner>) -> boost::asio::awaitable<void> {
              work_started = true;
              co_return;
           },
           detail::worker_stop_bridge_options{
               .before_work_spawn = [] { throw std::bad_alloc{}; },
           }),
       boost::asio::use_future);

   BOOST_CHECK_THROW(worker.get(), std::bad_alloc);
   BOOST_TEST(!work_started);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scored_candidates_are_bounded_and_ordered) {
   auto store = peer_store{};
   store.upsert(peer_store::record{.peer = peer(224)});
   store.upsert(peer_store::record{.peer = peer(225)});
   store.upsert(peer_store::record{.peer = peer(226)});
   store.mark_success(peer(225), path::kind::direct, std::chrono::milliseconds{10});
   store.mark_success(peer(226), path::kind::direct, std::chrono::milliseconds{20});

   const auto candidates = store.scored_candidates(2);
   BOOST_REQUIRE_EQUAL(candidates.size(), 2U);
   BOOST_TEST(candidates[0].peer.to_bytes() == peer(225).to_bytes(), boost::test_tools::per_element());
   BOOST_TEST(candidates[1].peer.to_bytes() == peer(226).to_bytes(), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scored_candidates_filter_the_source_index) {
   auto store = peer_store{};
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   store.upsert(peer_store::record{
       .peer = peer(227),
       .discovered_by = discovery::source::dht,
       .discovery_expires_at = expires_at,
   });
   store.upsert(peer_store::record{
       .peer = peer(228),
       .discovered_by = discovery::source::peer_exchange,
       .discovery_expires_at = expires_at,
   });
   store.upsert(peer_store::record{
       .peer = peer(229),
       .discovered_by = discovery::source::peer_exchange,
       .discovery_expires_at = expires_at,
   });
   store.mark_success(peer(229), path::kind::direct, std::chrono::milliseconds{10});

   const auto candidates = store.scored_candidates(discovery::source::peer_exchange, 2);
   BOOST_REQUIRE_EQUAL(candidates.size(), 2U);
   BOOST_TEST(candidates[0].peer.to_bytes() == peer(229).to_bytes(), boost::test_tools::per_element());
   BOOST_TEST(candidates[1].peer.to_bytes() == peer(228).to_bytes(), boost::test_tools::per_element());
   BOOST_TEST(static_cast<int>(candidates[0].discovered_by) == static_cast<int>(discovery::source::peer_exchange));
   BOOST_TEST(static_cast<int>(candidates[1].discovered_by) == static_cast<int>(discovery::source::peer_exchange));
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_scored_candidates_skip_expired_rows_before_limiting) {
   auto store = peer_store{};
   const auto expired_at = std::chrono::system_clock::now() - std::chrono::seconds{1};
   const auto live_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   store.upsert(peer_store::record{
       .peer = peer(231),
       .discovered_by = discovery::source::peer_exchange,
       .discovery_expires_at = expired_at,
   });
   store.upsert(peer_store::record{
       .peer = peer(232),
       .discovered_by = discovery::source::peer_exchange,
       .discovery_expires_at = live_at,
   });
   store.upsert(peer_store::record{
       .peer = peer(233),
       .discovered_by = discovery::source::peer_exchange,
       .discovery_expires_at = live_at,
   });
   store.mark_success(peer(231), path::kind::direct, std::chrono::milliseconds{1});
   store.mark_success(peer(232), path::kind::direct, std::chrono::milliseconds{10});
   store.mark_success(peer(233), path::kind::direct, std::chrono::milliseconds{20});

   const auto candidates = store.scored_candidates(discovery::source::peer_exchange, 2);
   BOOST_REQUIRE_EQUAL(candidates.size(), 2U);
   BOOST_TEST(std::ranges::none_of(candidates, [](const auto& value) { return value.peer == peer(231); }));
   BOOST_TEST(std::ranges::any_of(candidates, [](const auto& value) { return value.peer == peer(232); }));
   BOOST_TEST(std::ranges::any_of(candidates, [](const auto& value) { return value.peer == peer(233); }));
}

BOOST_AUTO_TEST_CASE(p2p_local_endpoints_collapse_canonical_equivalent_advertised_endpoints) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto local = peer(219);
   auto configured = make_tcp_endpoint(4001, "127.0.0.1");
   configured.peer = local;
   auto equivalent = parse_endpoint(configured.to_string());
   equivalent.peer = std::nullopt;
   auto options = options_for(local);
   options.advertised_endpoints = {configured, equivalent};
   auto value = node{runtime, std::move(options)};

   const auto endpoints = value.local_endpoints();
   BOOST_REQUIRE_EQUAL(endpoints.size(), 1U);
   BOOST_REQUIRE(endpoints.front().peer.has_value());
   BOOST_TEST(endpoints.front().peer->to_bytes() == local.to_bytes(), boost::test_tools::per_element());
   BOOST_TEST(endpoints.front().to_string() == configured.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_filters_non_routable_third_party_endpoints) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("peer-exchange-filter-server");
   const auto client_identity = make_test_certificate_identity("peer-exchange-filter-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};
   const auto third = peer(220);

   auto loopback = make_tcp_endpoint(4001, "127.0.0.1");
   auto private_endpoint = make_tcp_endpoint(4002, "192.168.1.10");
   auto link_local = make_tcp_endpoint(4003, "169.254.1.10");
   auto public_endpoint = make_tcp_endpoint(4004, "8.8.4.4");
   auto dns_endpoint = make_dns_tcp_endpoint(4005, "example.com");
   auto localhost_dns = make_dns_tcp_endpoint(4006, "localhost");
   for (auto* endpoint_value :
        {&loopback, &private_endpoint, &link_local, &public_endpoint, &dns_endpoint, &localhost_dns}) {
      endpoint_value->peer = third;
      server.peers().learn_endpoint(third, *endpoint_value, capability_set{});
   }

   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   const auto quic = require_endpoint_for(server.local_endpoints(), endpoint::protocol_kind::quic_v1);
   client.peers().learn_endpoint(server.local_peer(), quic,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::peer_exchange});
   forge::asio::blocking::run(runtime, client.async_request_peer_exchange(server.local_peer()));

   const auto learned = client.peers().find(third);
   BOOST_REQUIRE(learned);
   auto seen = std::set<std::string>{};
   for (const auto& item : learned->endpoints) {
      seen.insert(item.address.to_string());
   }
   BOOST_TEST(seen.contains(public_endpoint.to_string()));
   BOOST_TEST(seen.contains(dns_endpoint.to_string()));
   BOOST_TEST(!seen.contains(loopback.to_string()));
   BOOST_TEST(!seen.contains(private_endpoint.to_string()));
   BOOST_TEST(!seen.contains(link_local.to_string()));
   BOOST_TEST(!seen.contains(localhost_dns.to_string()));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_host_addresses_rejects_third_party_relay_with_non_routable_transport) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("peer-exchange-relay-filter-server");
   const auto client_identity = make_test_certificate_identity("peer-exchange-relay-filter-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};
   const auto third = peer(221);

   const auto loopback_relay = parse_endpoint("/ip4/127.0.0.1/tcp/9090/p2p-circuit/p2p/" + third.to_string());
   const auto private_relay = parse_endpoint("/ip4/10.0.0.7/tcp/9090/p2p-circuit/p2p/" + third.to_string());
   const auto link_local_relay = parse_endpoint("/ip4/169.254.10.7/tcp/9090/p2p-circuit/p2p/" + third.to_string());
   const auto public_relay = parse_endpoint("/ip4/8.8.8.8/tcp/9090/p2p-circuit/p2p/" + third.to_string());
   const auto dns_relay = parse_endpoint("/dns4/relay.example.com/tcp/9090/p2p-circuit/p2p/" + third.to_string());
   for (const auto& endpoint_value : {loopback_relay, private_relay, link_local_relay, public_relay, dns_relay}) {
      server.peers().learn_endpoint(third, endpoint_value, capability_set{});
   }

   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   const auto quic = require_endpoint_for(server.local_endpoints(), endpoint::protocol_kind::quic_v1);
   client.peers().learn_endpoint(server.local_peer(), quic,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::peer_exchange});
   forge::asio::blocking::run(runtime, client.async_request_peer_exchange(server.local_peer()));

   const auto learned = client.peers().find(third);
   BOOST_REQUIRE(learned);
   auto seen = std::set<std::string>{};
   for (const auto& item : learned->endpoints) {
      seen.insert(item.address.to_string());
   }
   BOOST_TEST(seen.contains(public_relay.to_string()));
   BOOST_TEST(seen.contains(dns_relay.to_string()));
   BOOST_TEST(!seen.contains(loopback_relay.to_string()));
   BOOST_TEST(!seen.contains(private_relay.to_string()));
   BOOST_TEST(!seen.contains(link_local_relay.to_string()));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_provider_response_uses_response_stream_provenance) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_profile = amino_v1(dht::mode::server);
   const auto provider_persistence = dht::record_store::make_memory_persistence();
   const auto key =
       amino_provider_key(std::vector<std::uint8_t>{'f', 'c', 'l', '-', 'd', 'h', 't', '-', 't', 'r', 'u', 's', 't'});
   const auto now = std::chrono::system_clock::now();
   const auto provider_expiry = now + std::chrono::hours{1};
   const auto address_expiry = now + std::chrono::minutes{30};
   const auto public_provider = peer(223);
   auto public_endpoint = make_dns_tcp_endpoint(4223, "provider.example.com");
   public_endpoint.peer = public_provider;
   const auto loopback_provider = peer(224);
   auto loopback_endpoint = make_tcp_endpoint(4224, "127.0.0.1");
   loopback_endpoint.peer = loopback_provider;
   const auto private_provider = peer(225);
   auto private_endpoint = make_tcp_endpoint(4225, "192.168.50.20");
   private_endpoint.peer = private_provider;
   const auto loopback_relay_provider = peer(228);
   const auto loopback_relay_endpoint =
       parse_endpoint("/ip4/127.0.0.1/tcp/4226/p2p-circuit/p2p/" + loopback_relay_provider.to_string());
   seed_dht_provider_records(runtime, server_profile, provider_persistence,
                             {
                                 {.key = key,
                                  .provider = public_provider,
                                  .endpoints = addresses_for({public_endpoint}),
                                  .provider_expires_at = provider_expiry,
                                  .addresses_expires_at = address_expiry},
                                 {.key = key,
                                  .provider = loopback_provider,
                                  .endpoints = addresses_for({loopback_endpoint}),
                                  .provider_expires_at = provider_expiry,
                                  .addresses_expires_at = address_expiry},
                                 {.key = key,
                                  .provider = private_provider,
                                  .endpoints = addresses_for({private_endpoint}),
                                  .provider_expires_at = provider_expiry,
                                  .addresses_expires_at = address_expiry},
                                 {.key = key,
                                  .provider = loopback_relay_provider,
                                  .endpoints = addresses_for({loopback_relay_endpoint}),
                                  .provider_expires_at = provider_expiry,
                                  .addresses_expires_at = address_expiry},
                             });
   const auto server_identity = make_test_certificate_identity("dht-discovery-server");
   const auto client_identity = make_test_certificate_identity("dht-discovery-client");
   auto server_options = dht_options_for(server_identity, server_profile);
   server_options.dht_record_persistence.emplace(server_profile.protocol, provider_persistence);
   auto client_options = dht_options_for(client_identity, amino_v1());
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   forge::asio::blocking::run(runtime, server.async_hydrate_peer_state());
   const auto server_endpoint = listen(server, runtime);
   verify_dht_server(runtime, client, server, server_endpoint, builtins::kad_dht);

   const auto providers = forge::asio::blocking::run(runtime, client.async_find_providers(builtins::kad_dht, key));
   BOOST_REQUIRE_EQUAL(providers.size(), 4U);
   const auto public_result = std::ranges::find(providers, public_provider, &dht::peer::id);
   BOOST_REQUIRE(public_result != providers.end());
   BOOST_REQUIRE_EQUAL(public_result->endpoints.size(), 1U);
   BOOST_TEST(public_result->endpoints.front().to_string() == public_endpoint.to_string());
   const auto loopback_result = std::ranges::find(providers, loopback_provider, &dht::peer::id);
   BOOST_REQUIRE(loopback_result != providers.end());
   BOOST_REQUIRE_EQUAL(loopback_result->endpoints.size(), 1U);
   BOOST_TEST(loopback_result->endpoints.front().to_string() == loopback_endpoint.to_string());
   const auto private_result = std::ranges::find(providers, private_provider, &dht::peer::id);
   BOOST_REQUIRE(private_result != providers.end());
   BOOST_TEST(private_result->endpoints.empty());
   const auto loopback_relay_result = std::ranges::find(providers, loopback_relay_provider, &dht::peer::id);
   BOOST_REQUIRE(loopback_relay_result != providers.end());
   BOOST_REQUIRE_EQUAL(loopback_relay_result->endpoints.size(), 1U);
   BOOST_TEST(loopback_relay_result->endpoints.front().to_string() == loopback_relay_endpoint.to_string());

   const auto queries_before_cached_lookup = server.metrics().dht_queries;
   const auto query_options = dht::query_options{.requested_count = 1};
   const auto cached =
       forge::asio::blocking::run(runtime, client.async_find_providers(builtins::kad_dht, key, query_options));
   BOOST_REQUIRE_EQUAL(cached.size(), 1U);
   BOOST_TEST(cached.front().id.to_string() == public_provider.to_string());
   BOOST_TEST(server.metrics().dht_queries > queries_before_cached_lookup);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_add_provider_preserves_identity_and_continues_rpc_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto profile_limits = dht::options{};
   profile_limits.provider_record_ttl = std::chrono::hours{3};
   profile_limits.provider_address_ttl = std::chrono::hours{2};
   profile_limits.provider_republish_interval = std::chrono::hours{1};
   const auto server_profile = custom_test_dht_profile(dht::mode::server, profile_limits);
   auto server_options = dht_options_for(peer(226), server_profile);
   auto server_store = std::make_shared<tracking_dht_record_store_persistence>();
   server_options.dht_record_persistence.emplace(server_profile.protocol, server_store);
   auto client_options = dht_options_for(peer(227), custom_test_dht_profile(dht::mode::client, profile_limits));
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   const auto key = make_dht_key(
       std::vector<std::uint8_t>{'f', 'c', 'l', '-', 'a', 'd', 'd', '-', 'p', 'r', 'o', 'v', 'i', 'd', 'e', 'r'});
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), server_profile.protocol));
   auto authenticated_peer = std::optional<peer_id>{};
   for (auto attempt = 0U; attempt < 20U && !authenticated_peer; ++attempt) {
      const auto sessions = server.diagnostics().sessions;
      if (!sessions.empty()) {
         authenticated_peer = sessions.front().remote_peer;
         break;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "DHT session diagnostics");
   }
   BOOST_REQUIRE(authenticated_peer.has_value());
   auto provider_endpoint = make_dns_tcp_endpoint(4227, "provider-announce.example.com");
   provider_endpoint.peer = *authenticated_peer;
   auto payload = dht::codec::encode(
       dht::message{
           .type = dht::message_type::add_provider,
           .key_value = key,
           .provider_peers = std::vector<dht::peer>{dht::peer{
               .id = *authenticated_peer,
               .endpoints = addresses_for({provider_endpoint}),
               .connection = dht::connection_type::connected,
           }},
       },
       server_profile);
   const auto ping = dht::codec::encode(dht::message{.type = dht::message_type::ping}, server_profile);
   payload.insert(payload.end(), ping.begin(), ping.end());
   const auto responses_before = server.metrics().dht_responses;
   const auto rejections_before = server.metrics().protocol_rejections;
   const auto stored_after = std::chrono::system_clock::now();
   forge::asio::blocking::run(runtime, stream.async_write(payload));
   const auto response = dht::codec::decode(
       wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream))), server_profile);
   BOOST_TEST(static_cast<int>(response.type) == static_cast<int>(dht::message_type::ping));
   forge::asio::blocking::run(runtime, stream.async_close());
   for (auto attempt = 0U; attempt < 40U && (server_store->provider_upsert_attempts.load() == 0U ||
                                             active_stream_count(server.diagnostics().resources) != 0U);
        ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "DHT ADD_PROVIDER handling");
   }

   BOOST_TEST(server.metrics().dht_queries >= 2U);
   BOOST_TEST(server.metrics().dht_responses == responses_before + 1U);
   BOOST_TEST(server.metrics().protocol_rejections == rejections_before);
   BOOST_TEST(active_stream_count(server.diagnostics().resources) == 0U);
   BOOST_TEST(server_store->provider_upsert_attempts.load(std::memory_order_relaxed) == 1U);
   const auto page =
       forge::asio::blocking::run(runtime, server_store->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers,
                                               .limit = 1,
                                           }));
   BOOST_REQUIRE_EQUAL(page.providers.size(), 1U);
   BOOST_TEST(page.providers.front().provider.to_string() == authenticated_peer->to_string());
   BOOST_REQUIRE_EQUAL(page.providers.front().endpoints.size(), 1U);
   BOOST_TEST(page.providers.front().endpoints.front().to_string() == provider_endpoint.to_string());
   const auto observed_at = std::chrono::system_clock::now();
   BOOST_TEST(page.providers.front().provider_expires_at >= stored_after + profile_limits.provider_record_ttl);
   BOOST_TEST(page.providers.front().provider_expires_at <= observed_at + profile_limits.provider_record_ttl);
   BOOST_TEST(page.providers.front().addresses_expires_at >= stored_after + profile_limits.provider_address_ttl);
   BOOST_TEST(page.providers.front().addresses_expires_at <= observed_at + profile_limits.provider_address_ttl);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_add_provider_retains_identity_when_address_is_filtered) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_profile = amino_v1(dht::mode::server);
   auto server_options = dht_options_for(peer(236), server_profile);
   auto server_store = std::make_shared<tracking_dht_record_store_persistence>();
   server_options.dht_record_persistence.emplace(server_profile.protocol, server_store);
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, dht_options_for(peer(237), amino_v1())};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::kad_dht));
   auto authenticated_peer = std::optional<peer_id>{};
   for (auto attempt = 0U; attempt < 20U && !authenticated_peer; ++attempt) {
      const auto sessions = server.diagnostics().sessions;
      if (!sessions.empty()) {
         authenticated_peer = sessions.front().remote_peer;
         break;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "DHT session diagnostics");
   }
   BOOST_REQUIRE(authenticated_peer.has_value());
   auto provider_endpoint = make_quic_endpoint(4237, "169.254.10.20");
   provider_endpoint.peer = *authenticated_peer;
   const auto key =
       amino_provider_key(std::vector<std::uint8_t>{'a', 'd', 'd', 'r', 'e', 's', 's', 'l', 'e', 's', 's'});
   auto payload = dht::codec::encode(
       dht::message{
           .type = dht::message_type::add_provider,
           .key_value = key,
           .provider_peers = {dht::peer{.id = *authenticated_peer,
                                        .endpoints = addresses_for({provider_endpoint})}},
       },
       server_profile);
   const auto ping = dht::codec::encode(dht::message{.type = dht::message_type::ping}, server_profile);
   payload.insert(payload.end(), ping.begin(), ping.end());
   forge::asio::blocking::run(runtime, stream.async_write(payload));
   const auto response = dht::codec::decode(
       wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream))), server_profile);
   BOOST_TEST(static_cast<int>(response.type) == static_cast<int>(dht::message_type::ping));
   forge::asio::blocking::run(runtime, stream.async_close());
   for (auto attempt = 0U; attempt < 40U && active_stream_count(server.diagnostics().resources) != 0U; ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{25}, "DHT addressless provider stream close");
   }
   BOOST_TEST(active_stream_count(server.diagnostics().resources) == 0U);
   BOOST_TEST(server_store->provider_upsert_attempts.load(std::memory_order_relaxed) == 1U);
   const auto page =
       forge::asio::blocking::run(runtime, server_store->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers,
                                               .limit = 1,
                                           }));
   BOOST_REQUIRE_EQUAL(page.providers.size(), 1U);
   BOOST_TEST(page.providers.front().provider.to_string() == authenticated_peer->to_string());
   BOOST_TEST(page.providers.front().endpoints.empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_persistent_stream_resets_idle_deadline_after_each_rpc) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.query_timeout = std::chrono::milliseconds{250};
   auto server = node{runtime, dht_options_for(peer(238), custom_test_dht_profile(dht::mode::server, limits))};
   auto client = node{runtime, dht_options_for(peer(239), custom_test_dht_profile(dht::mode::client, limits))};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), content_swarm_test_dht));
   const auto ping = dht::codec::encode(dht::message{.type = dht::message_type::ping},
                                        custom_test_dht_profile(dht::mode::client, limits));

   wait_on_runtime(runtime, std::chrono::milliseconds{150}, "first DHT stream idle interval");
   forge::asio::blocking::run(runtime, stream.async_write(ping));
   auto response = dht::codec::decode(
       wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream))), limits);
   BOOST_TEST(static_cast<int>(response.type) == static_cast<int>(dht::message_type::ping));

   wait_on_runtime(runtime, std::chrono::milliseconds{150}, "second DHT stream idle interval");
   forge::asio::blocking::run(runtime, stream.async_write(ping));
   response = dht::codec::decode(
       wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream))), limits);
   BOOST_TEST(static_cast<int>(response.type) == static_cast<int>(dht::message_type::ping));

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_rejected_value_request_does_not_admit_server_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = dht_options_for(peer(229), amino_v1(dht::mode::server));
   auto client_options = options_for(peer(230), capability_set{.bits = capabilities::direct_quic});
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);

   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::kad_dht));
   forge::asio::blocking::run(runtime, stream.async_write(dht::codec::encode(dht::message{
                                           .type = dht::message_type::get_value,
                                           .key_value = make_dht_key(std::vector<std::uint8_t>{'n', 'o', 'p'}),
                                       })));
   for (auto attempt = 0U; attempt < 20U && server.metrics().dht_queries == 0U; ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{10}, "rejected DHT value request");
   }
   stream.cancel();
   BOOST_TEST(server.metrics().dht_queries == 1U);
   BOOST_TEST(server.routing_status(builtins::kad_dht).active == 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_inbound_query_does_not_admit_client_as_server) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = dht_options_for(peer(231), amino_v1(dht::mode::server));
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, options_for(peer(232), capability_set{.bits = capabilities::direct_quic})};
   const auto server_endpoint = listen(server, runtime);

   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::kad_dht));
   const auto request = dht::message{
       .type = dht::message_type::find_node,
       .key_value = make_dht_key(client.local_peer()),
   };
   forge::asio::blocking::run(runtime, stream.async_write(dht::codec::encode(request)));
   const auto response =
       dht::codec::decode(wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream))));

   BOOST_TEST(static_cast<int>(response.type) == static_cast<int>(request.type));
   BOOST_TEST(response.key_value.bytes.empty());
   BOOST_TEST(server.routing_status(builtins::kad_dht).active == 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_large_non_put_key_uses_bounded_response_and_preserves_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.max_outbound_message_size = 64 * 1024;
   limits.max_inbound_message_size = 64 * 1024;
   const auto server_profile = custom_test_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_dht_profile(dht::mode::client, limits);
   auto server = node{runtime, dht_options_for(peer(233), server_profile)};
   auto client = node{runtime, dht_options_for(peer(234), client_profile)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), content_swarm_test_dht));
   const auto request = dht::message{
       .type = dht::message_type::find_node,
       .key_value = make_dht_key(std::vector<std::uint8_t>(32 * 1024, std::uint8_t{'k'})),
   };
   forge::asio::blocking::run(runtime, stream.async_write(dht::codec::encode(request, client_profile)));
   const auto payload = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024));
   const auto response = dht::codec::decode(wrap_length_delimited(payload), client_profile);
   BOOST_TEST(payload.size() <= 16U * 1024U);
   BOOST_TEST(static_cast<int>(response.type) == static_cast<int>(request.type));
   BOOST_TEST(response.key_value.bytes.empty());

   forge::asio::blocking::run(
       runtime, stream.async_write(dht::codec::encode(dht::message{.type = dht::message_type::ping}, client_profile)));
   const auto ping_payload = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024));
   const auto ping = dht::codec::decode(wrap_length_delimited(ping_payload), client_profile);
   BOOST_TEST(ping_payload.size() <= 16U * 1024U);
   BOOST_TEST(static_cast<int>(ping.type) == static_cast<int>(dht::message_type::ping));

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_large_stored_value_is_omitted_without_resetting_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.max_outbound_message_size = 64 * 1024;
   limits.max_inbound_message_size = 64 * 1024;
   limits.max_record_size = 64 * 1024;
   const auto server_profile = custom_test_value_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_value_dht_profile(dht::mode::client, limits);
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   const auto key = make_dht_key(std::vector<std::uint8_t>{'/', 'l', 'a', 'r', 'g', 'e'});
   auto batch = dht::record_store::mutation_batch{};
   batch.value_upserts.push_back(dht::record_store::value_record{
       .record = dht::record{.key_value = key, .value = std::vector<std::uint8_t>(24 * 1024, std::uint8_t{'v'})},
       .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
   });
   static_cast<void>(forge::asio::blocking::run(runtime, persistence->async_apply(std::move(batch))));

   auto server_options = dht_options_for(peer(235), server_profile);
   server_options.dht_record_persistence.emplace(server_profile.protocol, persistence);
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, dht_options_for(peer(236), client_profile)};
   forge::asio::blocking::run(runtime, server.async_hydrate_peer_state());
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), content_swarm_value_test_dht));
   forge::asio::blocking::run(
       runtime, stream.async_write(dht::codec::encode(
                    dht::message{.type = dht::message_type::get_value, .key_value = key}, client_profile)));
   const auto payload = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024));
   const auto response = dht::codec::decode(wrap_length_delimited(payload), client_profile);
   BOOST_TEST(payload.size() <= 16U * 1024U);
   BOOST_TEST(!response.record_value.has_value());

   forge::asio::blocking::run(
       runtime, stream.async_write(dht::codec::encode(dht::message{.type = dht::message_type::ping}, client_profile)));
   const auto ping_payload = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024));
   const auto ping = dht::codec::decode(wrap_length_delimited(ping_payload), client_profile);
   BOOST_TEST(static_cast<int>(ping.type) == static_cast<int>(dht::message_type::ping));

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_oversized_put_echo_is_rejected_before_durable_mutation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.max_outbound_message_size = 64 * 1024;
   limits.max_inbound_message_size = 64 * 1024;
   limits.max_record_size = 64 * 1024;
   const auto server_profile = custom_test_value_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_value_dht_profile(dht::mode::client, limits);
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto server_options = dht_options_for(peer(239), server_profile);
   server_options.dht_record_persistence.emplace(server_profile.protocol, persistence);
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, dht_options_for(peer(240), client_profile)};
   forge::asio::blocking::run(runtime, server.async_hydrate_peer_state());
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   const auto key = make_dht_key(std::vector<std::uint8_t>{'/', 'o', 'v', 'e', 'r', 's', 'i', 'z', 'e', 'd'});
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), content_swarm_value_test_dht));
   forge::asio::blocking::run(runtime, stream.async_write(dht::codec::encode(
                                           dht::message{
                                               .type = dht::message_type::put_value,
                                               .key_value = key,
                                               .record_value =
                                                   dht::record{
                                                       .key_value = key,
                                                       .value = std::vector<std::uint8_t>(24 * 1024, std::uint8_t{'v'}),
                                                   },
                                           },
                                           client_profile)));
   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024))),
                     std::exception);

   const auto persisted =
       forge::asio::blocking::run(runtime, persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::values,
                                               .limit = 1,
                                           }));
   BOOST_TEST(persisted.values.empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_provider_response_incrementally_truncates_to_wire_budget) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.max_outbound_message_size = 512 * 1024;
   limits.max_inbound_message_size = 512 * 1024;
   const auto server_profile = custom_test_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_dht_profile(dht::mode::client, limits);
   auto persistence = dht::record_store::make_memory_persistence();
   const auto key = make_dht_key(std::vector<std::uint8_t>{'b', 'o', 'u', 'n', 'd', 'e', 'd'});
   const auto now = std::chrono::system_clock::now();
   auto providers = std::vector<dht::record_store::provider_record>{};
   for (auto provider_index = 0U; provider_index < 20U; ++provider_index) {
      const auto provider = peer(static_cast<std::uint8_t>(100U + provider_index));
      auto endpoints = std::vector<endpoint>{};
      for (auto endpoint_index = 0U; endpoint_index < 64U; ++endpoint_index) {
         const auto host = std::string(50, 'a') + "." + std::string(50, 'b') + "." + std::string(50, 'c') + ".p" +
                           std::to_string(provider_index) + "e" + std::to_string(endpoint_index) + ".example.com";
         auto endpoint = make_dns_tcp_endpoint(4'000 + endpoint_index, host);
         endpoint.peer = provider;
         endpoints.push_back(std::move(endpoint));
      }
      providers.push_back(dht::record_store::provider_record{
          .key = key,
          .provider = provider,
          .endpoints = addresses_for(std::move(endpoints)),
          .provider_expires_at = now + std::chrono::hours{1},
          .addresses_expires_at = now + std::chrono::hours{1},
      });
   }
   seed_dht_provider_records(runtime, server_profile, persistence, std::move(providers));

   auto server_options = dht_options_for(peer(237), server_profile);
   server_options.dht_record_persistence.emplace(server_profile.protocol, persistence);
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, dht_options_for(peer(238), client_profile)};
   forge::asio::blocking::run(runtime, server.async_hydrate_peer_state());
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), content_swarm_test_dht));
   forge::asio::blocking::run(
       runtime, stream.async_write(dht::codec::encode(
                    dht::message{.type = dht::message_type::get_providers, .key_value = key}, client_profile)));
   const auto payload = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024));
   const auto response = dht::codec::decode(wrap_length_delimited(payload), client_profile);
   auto returned_endpoints = std::size_t{};
   for (const auto& provider : response.provider_peers) {
      returned_endpoints += provider.endpoints.size();
   }
   BOOST_TEST(payload.size() <= 16U * 1024U);
   BOOST_TEST(!response.provider_peers.empty());
   BOOST_TEST(returned_endpoints < 20U * 64U);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_rejected_request_cannot_inject_routing_candidates) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = dht_options_for(peer(233), amino_v1(dht::mode::server));
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, options_for(peer(234), capability_set{.bits = capabilities::direct_quic})};
   const auto server_endpoint = listen(server, runtime);
   const auto injected = peer(235);
   auto injected_endpoint = make_dns_tcp_endpoint(4235, "injected.example.com");
   injected_endpoint.peer = injected;

   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::kad_dht));
   forge::asio::blocking::run(runtime,
                              stream.async_write(dht::codec::encode(dht::message{
                                  .type = dht::message_type::get_value,
                                  .key_value = make_dht_key(std::vector<std::uint8_t>{'r', 'e', 'j', 'e', 'c', 't'}),
                                  .closer_peers = std::vector<dht::peer>{dht::peer{
                                      .id = injected,
                                      .endpoints = addresses_for({injected_endpoint}),
                                      .connection = dht::connection_type::can_connect,
                                  }},
                              })));
   for (auto attempt = 0U; attempt < 20U && server.metrics().dht_queries == 0U; ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{10}, "rejected DHT candidate injection");
   }
   stream.cancel();

   BOOST_TEST(server.metrics().dht_queries == 1U);
   BOOST_TEST(!server.peers().find(injected).has_value());
   BOOST_TEST(server.routing_status(builtins::kad_dht).active == 0U);
   BOOST_TEST(server.routing_status(builtins::kad_dht).replacements == 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_discovery_rejects_third_party_non_routable_endpoints) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options =
       options_for(peer(226), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   auto client_options =
       options_for(peer(227), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});

   const auto public_identity = make_test_certificate_identity("rendezvous-third-party-public");
   const auto relay_identity = make_test_certificate_identity("rendezvous-third-party-relay");
   const auto loopback_identity = make_test_certificate_identity("rendezvous-third-party-loopback");
   const auto private_identity = make_test_certificate_identity("rendezvous-third-party-private");
   const auto loopback_relay_identity = make_test_certificate_identity("rendezvous-third-party-loopback-relay");
   const auto private_relay_identity = make_test_certificate_identity("rendezvous-third-party-private-relay");
   auto public_endpoint = make_dns_tcp_endpoint(4228, "api.example.com");
   public_endpoint.peer = public_identity.peer;
   const auto relay_endpoint =
       parse_endpoint("/ip4/8.8.8.8/tcp/4001/p2p-circuit/p2p/" + relay_identity.peer.to_string());
   auto loopback_endpoint = make_tcp_endpoint(4230, "127.0.0.1");
   loopback_endpoint.peer = loopback_identity.peer;
   auto private_endpoint = make_tcp_endpoint(4231, "10.20.30.40");
   private_endpoint.peer = private_identity.peer;
   const auto loopback_relay_endpoint =
       parse_endpoint("/ip4/127.0.0.1/tcp/4001/p2p-circuit/p2p/" + loopback_relay_identity.peer.to_string());
   const auto private_relay_endpoint =
       parse_endpoint("/ip4/10.20.30.40/tcp/4001/p2p-circuit/p2p/" + private_relay_identity.peer.to_string());
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};

   forge::asio::blocking::run(runtime, server.peers().async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.discovery",
                                           .peer = public_identity.peer,
                                           .endpoints = addresses_for({public_endpoint}),
                                           .signed_peer_record = make_signed_rendezvous_peer_record(
                                               public_identity, std::vector<endpoint>{public_endpoint}, 1),
                                           .ttl = std::chrono::seconds{7'200},
                                           .expires_at = expires_at,
                                           .sequence = 1,
                                       }));
   forge::asio::blocking::run(runtime, server.peers().async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.discovery",
                                           .peer = relay_identity.peer,
                                           .endpoints = addresses_for({relay_endpoint}),
                                           .signed_peer_record = make_signed_rendezvous_peer_record(
                                               relay_identity, std::vector<endpoint>{relay_endpoint}, 2),
                                           .ttl = std::chrono::seconds{7'200},
                                           .expires_at = expires_at,
                                           .sequence = 2,
                                       }));
   forge::asio::blocking::run(runtime, server.peers().async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.discovery",
                                           .peer = loopback_identity.peer,
                                           .endpoints = addresses_for({loopback_endpoint}),
                                           .signed_peer_record = make_signed_rendezvous_peer_record(
                                               loopback_identity, std::vector<endpoint>{loopback_endpoint}, 3),
                                           .ttl = std::chrono::seconds{7'200},
                                           .expires_at = expires_at,
                                           .sequence = 3,
                                       }));
   forge::asio::blocking::run(runtime, server.peers().async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.discovery",
                                           .peer = private_identity.peer,
                                           .endpoints = addresses_for({private_endpoint}),
                                           .signed_peer_record = make_signed_rendezvous_peer_record(
                                               private_identity, std::vector<endpoint>{private_endpoint}, 4),
                                           .ttl = std::chrono::seconds{7'200},
                                           .expires_at = expires_at,
                                           .sequence = 4,
                                       }));
   forge::asio::blocking::run(runtime,
                              server.peers().async_upsert_rendezvous(rendezvous::registration{
                                  .namespace_name = "forge.discovery",
                                  .peer = loopback_relay_identity.peer,
                                  .endpoints = addresses_for({loopback_relay_endpoint}),
                                  .signed_peer_record = make_signed_rendezvous_peer_record(
                                      loopback_relay_identity, std::vector<endpoint>{loopback_relay_endpoint}, 5),
                                  .ttl = std::chrono::seconds{7'200},
                                  .expires_at = expires_at,
                                  .sequence = 5,
                              }));
   forge::asio::blocking::run(runtime,
                              server.peers().async_upsert_rendezvous(rendezvous::registration{
                                  .namespace_name = "forge.discovery",
                                  .peer = private_relay_identity.peer,
                                  .endpoints = addresses_for({private_relay_endpoint}),
                                  .signed_peer_record = make_signed_rendezvous_peer_record(
                                      private_relay_identity, std::vector<endpoint>{private_relay_endpoint}, 6),
                                  .ttl = std::chrono::seconds{7'200},
                                  .expires_at = expires_at,
                                  .sequence = 6,
                              }));

   const auto discovered = forge::asio::blocking::run(
       runtime, client.async_rendezvous_discover(server.local_peer(), rendezvous::discover_request{
                                                                          .namespace_name = "forge.discovery",
                                                                          .limit = 10,
                                                                      }));
   auto peers = std::set<std::string>{};
   for (const auto& registration : discovered.registrations) {
      peers.insert(registration.peer.to_string());
   }
   BOOST_TEST(peers.contains(public_identity.peer.to_string()));
   BOOST_TEST(peers.contains(relay_identity.peer.to_string()));
   BOOST_TEST(!peers.contains(loopback_identity.peer.to_string()));
   BOOST_TEST(!peers.contains(private_identity.peer.to_string()));
   BOOST_TEST(!peers.contains(loopback_relay_identity.peer.to_string()));
   BOOST_TEST(!peers.contains(private_relay_identity.peer.to_string()));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_stop_closes_all_direct_listeners) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(216))};
   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   forge::asio::blocking::run(runtime, server.async_listen(make_tcp_endpoint(0)));
   const auto local = server.local_endpoints();
   const auto quic = require_endpoint_for(local, endpoint::protocol_kind::quic_v1);
   const auto tcp = require_endpoint_for(local, endpoint::protocol_kind::tcp);

   forge::asio::blocking::run(runtime, server.async_stop());

   auto rebound = node{runtime, options_for(peer(217))};
   forge::asio::blocking::run(runtime, rebound.async_listen(quic));
   auto tcp_rebound = forge::net::tcp::listener{runtime.context().get_executor(), tcp.transport};
   BOOST_TEST(tcp_rebound.valid());

   forge::asio::blocking::run(runtime, tcp_rebound.async_close());
   forge::asio::blocking::run(runtime, rebound.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_nodes_prefer_tls_yamux_and_echo_frames) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_identity();
   const auto client_identity = make_test_identity();
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};
   register_echo(server);

   const auto server_endpoint = listen_tcp(server, runtime);
   BOOST_TEST(server_endpoint.is_direct_tcp());

   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(session.remote_peer.value == server.local_peer().value);
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo));
   BOOST_TEST(static_cast<std::uint8_t>(stream.authentication()) ==
              static_cast<std::uint8_t>(peer_authentication::libp2p_tls));
   auto payload = std::vector<std::uint8_t>(192 * 1024);
   for (auto index = std::size_t{}; index < payload.size(); ++index) {
      payload[index] = static_cast<std::uint8_t>(index % 251U);
   }
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(client.metrics().path_direct_opens >= 1U);
   BOOST_TEST(server.metrics().protocol_streams_accepted >= 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_raw_dns4_connect_retains_source_root_for_public_and_private_tcp) {
   for (const auto private_profile : {false, true}) {
      BOOST_TEST_CONTEXT("private profile=" << private_profile) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
         const auto server_identity = make_test_identity();
         const auto client_identity = make_test_identity();
         auto server_options = private_profile ? private_network_options_for(server_identity) : options_for(server_identity);
         auto client_options = private_profile ? private_network_options_for(client_identity) : options_for(client_identity);
         auto gater = std::make_shared<recording_connection_gater>();
         client_options.connection_gater = gater;
         client_options.limits.resources.max_dial_attempts = 1;
         client_options.limits.resources.max_dial_attempts_per_peer = 1;
         auto server = node{runtime, std::move(server_options)};
         auto client = node{runtime, std::move(client_options)};
         register_echo(server);
         const auto listening = listen_tcp(server, runtime);
         const auto root = forge::multiformats::multiaddr::parse(
             "/dns4/localhost/tcp/" + std::to_string(listening.transport.port) + "/p2p/" + server.local_peer().to_string());
         // Identify also advertises the source root, so no numeric address is
         // independently learned from the remote node during this assertion.
         server.set_advertised_endpoints({parse_endpoint(root.to_string())});

         const auto session = forge::asio::blocking::run(
             runtime, client.async_connect(root, node::connect_options{
                 .allow_relay = false, .timeout = std::chrono::seconds{3}, .max_direct_endpoints = 1}));
         BOOST_TEST(session.remote_peer.to_string() == server.local_peer().to_string());
         const auto stages = gater->stages_for(server.local_peer());
         BOOST_TEST(std::count(stages.begin(), stages.end(), recorded_gater_stage::peer_dial) == 1);
         BOOST_TEST(std::count(stages.begin(), stages.end(), recorded_gater_stage::address_dial) == 1);
         BOOST_TEST(client.diagnostics().resources.active_dials == 0U);

         auto stream = forge::asio::blocking::run(
             runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo));
         const auto payload = std::vector<std::uint8_t>{'d', 'n', 's'};
         forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
         const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
         BOOST_TEST(reply == payload, boost::test_tools::per_element());

         const auto record = client.peers().find(server.local_peer());
         BOOST_REQUIRE(record.has_value());
         const auto source = std::ranges::find_if(record->endpoints, [&](const auto& candidate) {
            return candidate.address.to_string() == root.to_string();
         });
         BOOST_REQUIRE(source != record->endpoints.end());
         BOOST_TEST(source->successes >= 1U);
         BOOST_TEST(source->failures == 0U);
         BOOST_TEST(source->sources.learned);
         for (const auto& candidate : record->endpoints) {
            if (candidate.address.to_string() != root.to_string()) {
               BOOST_TEST(candidate.successes == 0U);
               BOOST_TEST(!candidate.sources.learned);
            }
         }
         forge::asio::blocking::run(runtime, client.async_stop());
         BOOST_TEST(client.diagnostics().resources.active_dials == 0U);
         BOOST_TEST(client.diagnostics().resources.system.file_descriptors == 0U);
         forge::asio::blocking::run(runtime, server.async_stop());
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_raw_dns4_connect_holds_one_logical_permit_and_stop_releases_it) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = options_for(make_test_identity());
   options.limits.resources.max_dial_attempts = 1;
   options.limits.resources.max_dial_attempts_per_peer = 1;
   auto client = node{runtime, std::move(options)};
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   const auto stalled = start_stalling_tcp_peer(runtime, std::chrono::seconds{5}, accepted);
   const auto root = forge::multiformats::multiaddr::parse(
       "/dns4/localhost/tcp/" + std::to_string(stalled.transport.port) + "/p2p/" + peer(248).to_string());
   const auto connect_options = node::connect_options{
       .allow_relay = false, .timeout = std::chrono::seconds{10},
       .direct_attempt_timeout = std::chrono::seconds{5}, .max_direct_endpoints = 1};
   auto pending = boost::asio::co_spawn(runtime.context(), client.async_connect(root, connect_options),
                                      boost::asio::use_future);
   BOOST_REQUIRE(accepted_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_TEST(client.diagnostics().resources.active_dials == 1U);
   BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 1U);
   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(runtime, client.async_connect(root, connect_options)),
       forge::exceptions::base, [](const auto& error) {
          return exceptions::code_of(error) == exceptions::code::backpressure_rejected;
       });
   BOOST_TEST(client.diagnostics().resources.active_dials == 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   const auto resources = client.diagnostics().resources;
   BOOST_TEST(resources.active_dials == 0U);
   BOOST_TEST(resources.transient.outbound_connections == 0U);
   BOOST_TEST(resources.system.outbound_connections == 0U);
   BOOST_TEST(resources.system.file_descriptors == 0U);
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(pending.get(), forge::exceptions::base, [](const auto& error) {
      const auto kind = exceptions::code_of(error);
      return kind == exceptions::code::canceled || kind == exceptions::code::closed;
   });
   BOOST_TEST(client.metrics().active_sessions == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_private_network_tcp_tls_yamux_protects_direct_streams) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_identity();
   const auto client_identity = make_test_identity();
   auto server = node{runtime, private_network_options_for(server_identity)};
   auto client = node{runtime, private_network_options_for(client_identity)};
   register_echo(server);

   const auto server_endpoint = listen_tcp(server, runtime);
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(server_endpoint)));
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo));
   BOOST_TEST(static_cast<std::uint8_t>(stream.authentication()) ==
              static_cast<std::uint8_t>(peer_authentication::libp2p_tls));
   const auto payload = std::vector<std::uint8_t>{'p', 'n', 'e', 't'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   const auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   BOOST_CHECK(std::find(record->protocols.begin(), record->protocols.end(), builtins::relay_stop) ==
               record->protocols.end());
   BOOST_CHECK(std::find(record->protocols.begin(), record->protocols.end(), builtins::dcutr) ==
               record->protocols.end());
   BOOST_CHECK(std::find(record->protocols.begin(), record->protocols.end(), builtins::autonat_v1) ==
               record->protocols.end());
   BOOST_CHECK(std::find(record->protocols.begin(), record->protocols.end(), builtins::autonat_v2_dial_request) ==
               record->protocols.end());
   BOOST_CHECK(std::find(record->protocols.begin(), record->protocols.end(), builtins::autonat_v2_dial_back) ==
               record->protocols.end());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_private_network_rejects_mismatched_key_and_listener_survives) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, private_network_options_for(make_test_identity(), 17)};
   auto bad_client = node{runtime, private_network_options_for(make_test_identity(), 29)};
   auto good_client = node{runtime, private_network_options_for(make_test_identity(), 17)};
   const auto server_endpoint = listen_tcp(server, runtime);
   const auto server_before = server.metrics();
   const auto client_before = bad_client.metrics();

   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, bad_client.async_connect(server_endpoint))),
                     forge::exceptions::base);
   const auto server_after_rejection = server.metrics();
   const auto client_after_rejection = bad_client.metrics();
   BOOST_CHECK_EQUAL(server_after_rejection.sessions_opened, server_before.sessions_opened);
   BOOST_CHECK_EQUAL(server_after_rejection.active_sessions, server_before.active_sessions);
   BOOST_CHECK_EQUAL(client_after_rejection.path_direct_attempts, client_before.path_direct_attempts + 1U);
   BOOST_CHECK(!server.peers().find(bad_client.local_peer()).has_value());
   static_cast<void>(forge::asio::blocking::run(runtime, good_client.async_connect(server_endpoint)));

   forge::asio::blocking::run(runtime, bad_client.async_stop());
   forge::asio::blocking::run(runtime, good_client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_private_network_maps_absent_and_truncated_peer_nonce_to_closed) {
   for (const auto returned_nonce_bytes : {std::size_t{0}, std::size_t{7}}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto client = node{runtime, private_network_options_for(make_test_identity())};
      const auto endpoint = start_pnet_nonce_rejecting_tcp_peer(runtime, returned_nonce_bytes);

      try {
         static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(endpoint)));
         BOOST_FAIL("private P2P connect accepted a missing or truncated peer nonce");
      } catch (const forge::exceptions::base& error) {
         BOOST_TEST(exceptions::is(error, exceptions::code::closed));
      }
      BOOST_CHECK_EQUAL(client.metrics().sessions_opened, 0U);
      BOOST_CHECK_EQUAL(client.metrics().active_sessions, 0U);

      forge::asio::blocking::run(runtime, client.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_private_network_maps_post_selection_pnet_close_to_p2p_closed) {
   for (const auto& protocol : {protocol_id{.value = "/noise"}, protocol_id{.value = "/tls/1.0.0"}}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto client = node{runtime, private_network_options_for(make_test_identity())};
      auto [peer_endpoint, peer_finished] = start_pnet_security_closing_tcp_peer(runtime, protocol);

      try {
         static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(peer_endpoint)));
         BOOST_FAIL("private P2P connect accepted a peer that closed before the security handshake");
      } catch (const forge::exceptions::base& error) {
         BOOST_REQUIRE(exceptions::code_of(error).has_value());
         BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::closed));
      }

      wait_for_server(peer_finished, std::chrono::seconds{2}, "post-selection PNET closing peer");
      forge::asio::blocking::run(runtime, client.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_private_network_rejects_non_tcp_and_relay_operations_before_io) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto value = node{runtime, private_network_options_for(make_test_identity())};
   const auto relay_peer = peer(246);

   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, value.async_listen(make_quic_endpoint(0)))),
                     exceptions::invalid_options);
   BOOST_CHECK_THROW(
       static_cast<void>(forge::asio::blocking::run(runtime, value.async_connect(make_quic_endpoint(1)))),
       exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, value.async_reserve_relay(relay_peer))),
                     exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, value.async_refresh_relay_candidates())),
                     exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, value.async_cancel_relay(relay_peer))),
                     exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, value.async_attempt_hole_punch(relay_peer))),
                     exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, value.async_probe_reachability(relay_peer))),
                     exceptions::invalid_options);
   BOOST_CHECK_THROW(
       static_cast<void>(forge::asio::blocking::run(
           runtime, value.async_open_protocol_stream(relay_peer, builtins::echo,
                                                     node::open_options{.relay_peer = relay_peer}))),
       exceptions::invalid_options);

   forge::asio::blocking::run(runtime, value.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_private_network_options_and_reserved_protocols_fail_fast) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto quic = private_network_options_for(make_test_identity());
   quic.capabilities.add(capabilities::direct_quic);
   BOOST_CHECK_THROW(static_cast<void>(node{runtime, std::move(quic)}), exceptions::invalid_options);

   auto relay = private_network_options_for(make_test_identity());
   relay.relay_policy.client_enabled = true;
   BOOST_CHECK_THROW(static_cast<void>(node{runtime, std::move(relay)}), exceptions::invalid_options);

   auto non_tcp = private_network_options_for(make_test_identity());
   non_tcp.advertised_endpoints = {make_quic_endpoint(1)};
   BOOST_CHECK_THROW(static_cast<void>(node{runtime, std::move(non_tcp)}), exceptions::invalid_options);

   auto value = node{runtime, private_network_options_for(make_test_identity())};
   const auto handler = [](node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; };
   const auto reserved = std::array{
       builtins::autonat_v1,
       builtins::autonat_v2_dial_request,
       builtins::autonat_v2_dial_back,
       builtins::relay_hop,
       builtins::relay_stop,
       builtins::dcutr,
   };
   const auto direct_attempts = value.metrics().path_direct_attempts;
   for (const auto& protocol : reserved) {
      BOOST_CHECK_THROW(value.register_protocol_handler(protocol, handler), exceptions::invalid_options);
      BOOST_CHECK_THROW(
          static_cast<void>(forge::asio::blocking::run(
              runtime, value.async_open_protocol_stream(peer(247), protocol))),
          exceptions::invalid_options);
      BOOST_CHECK_EQUAL(value.metrics().path_direct_attempts, direct_attempts);
   }

   const auto allowed = std::array{
       builtins::kad_dht,
       builtins::rendezvous,
       builtins::meshsub_v11,
       protocol_id{.value = "/forge/private-network/custom/1"},
   };
   for (const auto& protocol : allowed) {
      BOOST_CHECK_NO_THROW(value.register_protocol_handler(protocol, handler));
      BOOST_CHECK(value.unregister_protocol_handler(protocol));
   }
   forge::asio::blocking::run(runtime, value.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_protocol_timeout_cancels_only_stalled_yamux_stream) {
   auto server_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{server_runtime, options_for(make_test_identity())};
   auto client = node{client_runtime, options_for(make_test_identity())};
   register_echo(server);

   const auto server_endpoint = listen_tcp(server, server_runtime);
   static_cast<void>(forge::asio::blocking::run(
       client_runtime,
       client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   const auto sessions_opened = client.metrics().sessions_opened;
   auto sibling = forge::asio::blocking::run(
       client_runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                         node::open_options{
                                                             .allow_relay = false,
                                                             .timeout = std::chrono::seconds{2},
                                                             .direct_attempt_timeout = std::chrono::seconds{2},
                                                             .max_direct_endpoints = 1,
                                                         }));

   auto release_server = block_runtime(server_runtime, "direct Yamux multistream negotiation barrier");
   auto stalled = boost::asio::co_spawn(
       client_runtime.context(),
       client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                         node::open_options{
                                             .allow_relay = false,
                                             .timeout = std::chrono::milliseconds{100},
                                             .direct_attempt_timeout = std::chrono::milliseconds{100},
                                             .max_direct_endpoints = 1,
                                         }),
       boost::asio::use_future);
   const auto canceled = stalled.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
   release_server->set_value();
   BOOST_REQUIRE(canceled);
   try {
      static_cast<void>(stalled.get());
      BOOST_FAIL("expected stalled Yamux protocol negotiation timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }

   const auto payload = std::vector<std::uint8_t>{'s', 'i', 'b', 'l', 'i', 'n', 'g'};
   forge::asio::blocking::run(client_runtime, sibling.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(client_runtime, sibling.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   forge::asio::blocking::run(client_runtime, sibling.async_close());
   BOOST_TEST(client.metrics().sessions_opened == sessions_opened);

   forge::asio::blocking::run(client_runtime, client.async_stop());
   forge::asio::blocking::run(server_runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_timeout_cancels_stalled_yamux_negotiation_and_preserves_session) {
   auto server_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.query_timeout = std::chrono::milliseconds{100};
   auto server =
       node{server_runtime, dht_options_for(make_test_identity(), custom_test_dht_profile(dht::mode::server, limits))};
   auto client =
       node{client_runtime, dht_options_for(make_test_identity(), custom_test_dht_profile(dht::mode::client, limits))};
   register_echo(server);

   const auto server_endpoint = listen_tcp(server, server_runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   static_cast<void>(forge::asio::blocking::run(
       client_runtime,
       client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   BOOST_REQUIRE(client.routing_status(content_swarm_test_dht).active > 0U);
   const auto sessions_opened = client.metrics().sessions_opened;

   auto release_server = block_runtime(server_runtime, "DHT Yamux multistream negotiation barrier");
   auto lookup = boost::asio::co_spawn(
       client_runtime.context(),
       client.async_find_peer(
           content_swarm_test_dht, peer(199),
           dht::query_options{.requested_count = 1, .quorum = 1, .timeout = std::chrono::milliseconds{100}}),
       boost::asio::use_future);
   const auto canceled = lookup.wait_for(std::chrono::seconds{1}) == std::future_status::ready;
   release_server->set_value();
   BOOST_REQUIRE(canceled);
   try {
      const auto result = lookup.get();
      BOOST_TEST(!result.complete);
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }

   auto sibling = forge::asio::blocking::run(
       client_runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                         node::open_options{
                                                             .allow_relay = false,
                                                             .timeout = std::chrono::seconds{2},
                                                             .direct_attempt_timeout = std::chrono::seconds{2},
                                                             .max_direct_endpoints = 1,
                                                         }));
   const auto payload = std::vector<std::uint8_t>{'d', 'h', 't'};
   forge::asio::blocking::run(client_runtime, sibling.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(client_runtime, sibling.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   forge::asio::blocking::run(client_runtime, sibling.async_close());
   BOOST_TEST(client.metrics().sessions_opened == sessions_opened);

   forge::asio::blocking::run(client_runtime, client.async_stop());
   forge::asio::blocking::run(server_runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_accept_survives_aborted_inbound_upgrade) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_identity();
   const auto client_identity = make_test_identity();
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};
   register_echo(server);
   const auto server_endpoint = listen_tcp(server, runtime);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto socket = boost::asio::ip::tcp::socket{co_await boost::asio::this_coro::executor};
      co_await socket.async_connect(
          boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address(server_endpoint.transport.host),
                                         server_endpoint.transport.port},
          boost::asio::use_awaitable);
      auto ignored = boost::system::error_code{};
      socket.close(ignored);
   }());
   const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (server.metrics().handshakes_failed == 0U) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < failure_deadline);
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "aborted inbound upgrade");
   }

   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(session.remote_peer.to_string() == server.local_peer().to_string());
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'a', 'f', 't', 'e', 'r'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_concurrent_handshakes_reuse_configured_node_identity) {
   constexpr auto connection_count = std::size_t{4};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = connection_count * 2}};
   const auto client_identity = make_test_identity();
   auto client = node{runtime, options_for(client_identity)};
   auto servers = std::vector<std::unique_ptr<node>>{};
   auto endpoints = std::vector<endpoint>{};
   servers.reserve(connection_count);
   endpoints.reserve(connection_count);

   for (auto index = std::size_t{}; index < connection_count; ++index) {
      const auto server_identity = make_test_identity();
      auto server = std::make_unique<node>(runtime, options_for(server_identity));
      register_echo(*server);
      endpoints.push_back(listen_tcp(*server, runtime));
      servers.push_back(std::move(server));
   }

   auto ready = std::atomic_size_t{};
   auto connections = std::vector<std::future<node::session_info>>{};
   connections.reserve(connection_count);
   for (auto index = std::size_t{}; index < connection_count; ++index) {
      connections.push_back(boost::asio::co_spawn(
          runtime.context(),
          [&client, &servers, &endpoints, &ready, index]() -> boost::asio::awaitable<node::session_info> {
             ready.fetch_add(1, std::memory_order_acq_rel);
             while (ready.load(std::memory_order_acquire) != connection_count) {
                co_await boost::asio::post(boost::asio::use_awaitable);
             }
             co_return co_await client.async_connect(endpoints[index],
                                                     node::connect_options{
                                                         .expected_peer = servers[index]->local_peer(),
                                                         .allow_relay = false,
                                                     });
          },
          boost::asio::use_future));
   }

   for (auto index = std::size_t{}; index < connection_count; ++index) {
      BOOST_REQUIRE(connections[index].wait_for(std::chrono::seconds{5}) == std::future_status::ready);
      const auto session = connections[index].get();
      BOOST_TEST(session.remote_peer.value == servers[index]->local_peer().value);

      auto stream = forge::asio::blocking::run(
          runtime, client.async_open_protocol_stream(servers[index]->local_peer(), builtins::echo,
                                                     node::open_options{.allow_relay = false}));
      const auto payload = std::vector<std::uint8_t>{'t', 'l', 's', static_cast<std::uint8_t>(index)};
      forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
      const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
      forge::asio::blocking::run(runtime, stream.async_close());
      BOOST_TEST(reply == payload, boost::test_tools::per_element());
   }

   BOOST_TEST(client.metrics().active_sessions == connection_count);
   forge::asio::blocking::run(runtime, client.async_stop());
   for (auto& server : servers) {
      forge::asio::blocking::run(runtime, server->async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_insecure_quic_node_does_not_require_signing_identity) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = options_for(peer(218));
   options.certificate_pem.clear();
   options.private_key_pem.clear();
   options.public_key.clear();

   auto value = node{runtime, std::move(options)};
   BOOST_TEST(value.local_peer().value == peer(218).value);
   forge::asio::blocking::run(runtime, value.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_rejects_tls_peer_mismatch) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("tcp-peer-mismatch-server");
   const auto client_identity = make_test_certificate_identity("tcp-peer-mismatch-client");
   auto server_options = options_for(server_identity);
   auto client_options = options_for(client_identity);
   server_options.allow_insecure_test_mode = false;
   client_options.allow_insecure_test_mode = false;
   server_options.peer_state.persistence = peer_store::make_memory_persistence();
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   auto server_endpoint = listen_tcp(server, runtime);
   server_endpoint.peer.reset();
   auto rejected = boost::asio::co_spawn(
       runtime.context(), client.async_connect(server_endpoint, node::connect_options{.expected_peer = peer(150)}),
       boost::asio::use_future);
   BOOST_REQUIRE(rejected.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   try {
      static_cast<void>(rejected.get());
      BOOST_FAIL("expected TCP TLS peer mismatch");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::peer_verification_failed));
   }

   auto stopped = boost::asio::co_spawn(runtime.context(), client.async_stop(), boost::asio::use_future);
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   stopped.get();
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_upgrade_honors_attempt_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = node{runtime, options_for(peer(201))};
   const auto stalled_endpoint = start_stalling_tcp_peer(runtime);

   auto saw_timeout = false;
   const auto completed = forge::asio::blocking::run_for(
       runtime,
       [&]() -> boost::asio::awaitable<void> {
          try {
             (void)co_await client.async_connect(stalled_endpoint,
                                                 node::connect_options{.expected_peer = peer(202),
                                                                       .allow_relay = false,
                                                                       .timeout = std::chrono::milliseconds{100}});
             BOOST_FAIL("expected stalled TCP direct connect timeout");
          } catch (const forge::exceptions::base& error) {
             BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
             BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                        static_cast<int>(exceptions::code::timeout));
             saw_timeout = true;
          }
       }(),
       std::chrono::milliseconds{1'000});

   BOOST_TEST(completed);
   BOOST_TEST(saw_timeout);
   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_noise_stall_honors_attempt_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = node{runtime, options_for(make_test_identity())};
   auto security_started = std::make_shared<std::promise<void>>();
   auto security_started_future = security_started->get_future();
   const auto stalled_endpoint =
       start_stalling_security_tcp_peer(runtime, protocol_id{.value = "/noise"}, security_started);

   auto pending = boost::asio::co_spawn(runtime.context(),
                                        client.async_connect(stalled_endpoint,
                                                             node::connect_options{
                                                                 .expected_peer = peer(202),
                                                                 .allow_relay = false,
                                                                 .timeout = std::chrono::milliseconds{100},
                                                             }),
                                        boost::asio::use_future);
   wait_for_server(security_started_future, std::chrono::seconds{2}, "Noise handshake stall");

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   try {
      (void)pending.get();
      BOOST_FAIL("expected stalled Noise handshake timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::timeout));
   }
   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_noise_stall_is_canceled_by_node_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = node{runtime, options_for(make_test_identity())};
   auto security_started = std::make_shared<std::promise<void>>();
   auto security_started_future = security_started->get_future();
   const auto stalled_endpoint =
       start_stalling_security_tcp_peer(runtime, protocol_id{.value = "/noise"}, security_started);
   auto pending = boost::asio::co_spawn(runtime.context(),
                                        client.async_connect(stalled_endpoint,
                                                             node::connect_options{
                                                                 .expected_peer = peer(203),
                                                                 .allow_relay = false,
                                                                 .timeout = std::chrono::seconds{5},
                                                             }),
                                        boost::asio::use_future);
   wait_for_server(security_started_future, std::chrono::seconds{2}, "Noise handshake stop stall");

   const auto started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(runtime, client.async_stop());
   const auto stop_elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
   BOOST_TEST(stop_elapsed < 750);
   BOOST_REQUIRE(pending.wait_for(std::chrono::milliseconds{750}) == std::future_status::ready);
   try {
      (void)pending.get();
      BOOST_FAIL("expected stalled Noise handshake cancellation");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      const auto kind = *forge::net::p2p::exceptions::code_of(error);
      const auto stopped = kind == exceptions::code::canceled || kind == exceptions::code::closed;
      BOOST_TEST_CONTEXT("stop error=" << static_cast<int>(kind)) {
         BOOST_TEST(stopped);
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_tls_stall_honors_attempt_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = node{runtime, options_for(make_test_identity())};
   auto security_started = std::make_shared<std::promise<void>>();
   auto security_started_future = security_started->get_future();
   const auto stalled_endpoint =
       start_stalling_security_tcp_peer(runtime, protocol_id{.value = "/tls/1.0.0"}, security_started);

   auto pending = boost::asio::co_spawn(runtime.context(),
                                        client.async_connect(stalled_endpoint,
                                                             node::connect_options{
                                                                 .expected_peer = peer(204),
                                                                 .allow_relay = false,
                                                                 .timeout = std::chrono::milliseconds{100},
                                                             }),
                                        boost::asio::use_future);
   wait_for_server(security_started_future, std::chrono::seconds{2}, "TLS handshake stall");

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   try {
      (void)pending.get();
      BOOST_FAIL("expected stalled TLS handshake timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::timeout));
   }
   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_stop_cancels_shared_tls_owner_during_yamux_negotiation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 3}};
   const auto server_identity = make_test_certificate_identity("tls-yamux-stall-server");
   const auto client_identity = make_test_certificate_identity("tls-yamux-stall-client");
   auto server_identity_options = options_for(server_identity);
   auto client_options = options_for(client_identity);
   client_options.allow_insecure_test_mode = false;
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto client = node{runtime, std::move(client_options)};
   auto yamux_started = std::make_shared<std::promise<void>>();
   auto yamux_started_future = yamux_started->get_future();
   auto peer_finished = std::make_shared<std::promise<void>>();
   auto peer_finished_future = peer_finished->get_future();
   auto stalled_endpoint =
       start_stalling_tls_yamux_tcp_peer(runtime, server_identity_options, yamux_started, peer_finished);
   stalled_endpoint.peer = server_identity.peer;

   auto pending = boost::asio::co_spawn(runtime.context(),
                                        client.async_connect(stalled_endpoint,
                                                             node::connect_options{
                                                                 .expected_peer = server_identity.peer,
                                                                 .allow_relay = false,
                                                                 .timeout = std::chrono::seconds{5},
                                                             }),
                                        boost::asio::use_future);
   wait_for_server(yamux_started_future, std::chrono::seconds{2}, "TLS Yamux negotiation stall");

   auto stopped = boost::asio::co_spawn(runtime.context(), client.async_stop(), boost::asio::use_future);
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   stopped.get();
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   try {
      static_cast<void>(pending.get());
      BOOST_FAIL("node stop should cancel stalled Yamux negotiation through the shared TLS owner");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      const auto kind = *forge::net::p2p::exceptions::code_of(error);
      const auto canceled = kind == exceptions::code::canceled || kind == exceptions::code::closed;
      BOOST_TEST(canceled);
   }
   BOOST_REQUIRE(peer_finished_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   peer_finished_future.get();
}

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_stop_closes_listener_port) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto server = node{runtime, options_for(peer(203))};
   const auto server_endpoint = listen_tcp(server, runtime);

   forge::asio::blocking::run(runtime, server.async_stop());

   auto rebound = forge::net::tcp::listener{runtime.context().get_executor(), server_endpoint.transport};
   BOOST_TEST(rebound.valid());
   forge::asio::blocking::run(runtime, rebound.async_close());
}

BOOST_AUTO_TEST_CASE(quic_libp2p_profile_sets_required_alpn) {
   auto client = forge::net::quic::libp2p::client_profile();
   auto server = forge::net::quic::libp2p::server_profile();

   BOOST_TEST(client.alpn == "libp2p");
   BOOST_TEST(server.alpn == "libp2p");
   BOOST_TEST(forge::net::quic::libp2p::is_profile_alpn(client.alpn));
}

BOOST_AUTO_TEST_CASE(transport_frame_and_stream_round_trip_payload) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto backend = std::make_shared<queued_transport_stream>(42);
   auto value = forge::net::transport::detail::stream_access::make(backend);
   const auto payload = std::vector<std::uint8_t>{'t', 'r', 'a', 'n', 's', 'p', 'o', 'r', 't'};
   const auto encoded = forge::net::transport::encode_frame(payload);
   const auto decoded = forge::net::transport::decode_frame(encoded);
   BOOST_TEST(static_cast<int>(decoded.status) ==
              static_cast<int>(forge::net::transport::frame_decode_status::complete));
   BOOST_TEST(decoded.payload == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, value.async_write_frame(payload));
   BOOST_REQUIRE_EQUAL(backend->writes.size(), 1U);
   BOOST_TEST(forge::net::transport::decode_frame(backend->writes.front()).payload == payload,
              boost::test_tools::per_element());

   backend->reads.push_back({encoded.begin(), encoded.begin() + 3});
   backend->reads.push_back({encoded.begin() + 3, encoded.end()});
   const auto read = forge::asio::blocking::run(runtime, value.async_read_frame());
   BOOST_TEST(read == payload, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(p2p_stream_wraps_transport_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto backend = std::make_shared<queued_transport_stream>(77);
   auto value = forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)};
   const auto payload = std::vector<std::uint8_t>{'p', '2', 'p'};
   const auto reply = std::vector<std::uint8_t>{'o', 'k'};

   BOOST_TEST(value.valid());
   BOOST_TEST(value.id() == 77);

   forge::asio::blocking::run(runtime, value.async_write(payload));
   BOOST_REQUIRE_EQUAL(backend->writes.size(), 1U);
   BOOST_TEST(backend->writes.front() == payload, boost::test_tools::per_element());

   backend->reads.push_back(reply);
   const auto read = forge::asio::blocking::run(runtime, value.async_read());
   BOOST_TEST(read == reply, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, value.async_close());
   BOOST_TEST(backend->closed);
}

BOOST_AUTO_TEST_CASE(p2p_stream_delegates_chunk_read_write_and_preserves_framed_trailing_bytes) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto backend = std::make_shared<queued_transport_stream>(78);
   auto value = forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)};
   const auto payload = std::vector<std::uint8_t>{'c', 'h', 'u', 'n', 'k'};
   const auto first = std::vector<std::uint8_t>{'o', 'n', 'e'};
   const auto second = std::vector<std::uint8_t>{'t', 'w', 'o'};

   forge::asio::blocking::run(runtime, value.async_write(forge::net::transport::chunk{payload}));
   BOOST_REQUIRE_EQUAL(backend->chunk_writes, 1U);
   BOOST_REQUIRE_EQUAL(backend->writes.size(), 1U);
   BOOST_TEST(backend->writes.front() == payload, boost::test_tools::per_element());

   backend->reads.push_back(payload);
   auto read = forge::asio::blocking::run(runtime, value.async_read_chunk());
   BOOST_REQUIRE_EQUAL(backend->chunk_reads, 1U);
   BOOST_TEST(read.to_vector() == payload, boost::test_tools::per_element());

   auto combined = forge::net::transport::encode_frame(first);
   auto encoded_second = forge::net::transport::encode_frame(second);
   combined.insert(combined.end(), encoded_second.begin(), encoded_second.end());
   backend->reads.push_back(std::move(combined));

   auto first_read = forge::asio::blocking::run(runtime, value.async_read_frame_chunk());
   BOOST_TEST(first_read.to_vector() == first, boost::test_tools::per_element());
   const auto second_read = forge::asio::blocking::run(runtime, value.async_read_frame());
   BOOST_TEST(second_read == second, boost::test_tools::per_element());
   BOOST_REQUIRE_EQUAL(backend->chunk_reads, 2U);
}

BOOST_AUTO_TEST_CASE(p2p_stream_with_buffer_uses_transport_bounds_and_preserves_coalesced_frames) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto backend = std::make_shared<queued_transport_stream>(79);
   auto first = std::vector<std::uint8_t>{'p', 'r', 'e'};
   auto second = std::vector<std::uint8_t>{'b', 'u', 'f'};
   const auto options = forge::net::transport::frame_options{.max_size = 3, .max_buffered_size = 14};
   auto buffered = forge::net::transport::encode_frame(first, options);
   auto encoded_second = forge::net::transport::encode_frame(second, options);
   buffered.insert(buffered.end(), encoded_second.begin(), encoded_second.end());
   BOOST_REQUIRE_EQUAL(buffered.size(), options.max_buffered_size);

   auto value = forge::net::p2p::detail::stream_access::with_buffer(
       forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)}, std::move(buffered));

   auto first_read = forge::asio::blocking::run(runtime, value.async_read_frame_chunk(options));
   BOOST_TEST(first_read.to_vector() == first, boost::test_tools::per_element());
   const auto second_read = forge::asio::blocking::run(runtime, value.async_read_frame(options));
   BOOST_TEST(second_read == second, boost::test_tools::per_element());
   BOOST_TEST(backend->chunk_reads == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_private_v1_lazy_security_negotiation_preserves_coalesced_handshake_tail) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto security_protocols = std::array{
       protocol_id{.value = "/noise"},
       protocol_id{.value = "/tls/1.0.0"},
   };

   for (const auto& security_protocol : security_protocols) {
      const auto handshake_prefix = security_protocol.value == "/noise"
                                        ? std::vector<std::uint8_t>{0x00, 0x20, 0x01, 0x02, 0x03}
                                        : std::vector<std::uint8_t>{0x16, 0x03, 0x03, 0x00, 0x05};
      auto outbound_backend = std::make_shared<queued_transport_stream>(801);
      auto outbound_input = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(
          protocol_negotiation::message{.kind = protocol_negotiation::message_kind::header,
                                        .protocol = protocol_negotiation::multistream_v1}));
      if (security_protocol.value == "/noise") {
         const auto unavailable = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(
             protocol_negotiation::message{.kind = protocol_negotiation::message_kind::not_available,
                                           .protocol = protocol_negotiation::not_available}));
         outbound_input.insert(outbound_input.end(), unavailable.begin(), unavailable.end());
      }
      const auto outbound_selection = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(
          protocol_negotiation::message{.kind = protocol_negotiation::message_kind::protocol,
                                        .protocol = security_protocol}));
      outbound_input.insert(outbound_input.end(), outbound_selection.begin(), outbound_selection.end());
      outbound_input.insert(outbound_input.end(), handshake_prefix.begin(), handshake_prefix.end());
      outbound_backend->reads.push_back(std::move(outbound_input));
      auto outbound_stream = forge::net::p2p::stream{
          forge::net::transport::detail::stream_access::make(outbound_backend)};

      const auto outbound_selected = forge::asio::blocking::run(
          runtime, detail::select_private_stream_security_protocol(outbound_stream));
      BOOST_TEST(outbound_selected.value == security_protocol.value);
      BOOST_TEST(outbound_backend->chunk_reads == 1U);
      const auto outbound_security_input = forge::asio::blocking::run(runtime, outbound_stream.async_read());
      BOOST_TEST(outbound_security_input == handshake_prefix, boost::test_tools::per_element());
      BOOST_TEST(outbound_backend->chunk_reads == 1U);

      auto inbound_backend = std::make_shared<queued_transport_stream>(802);
      auto inbound_input = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(
          protocol_negotiation::message{.kind = protocol_negotiation::message_kind::header,
                                        .protocol = protocol_negotiation::multistream_v1}));
      const auto inbound_proposal = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(
          protocol_negotiation::message{.kind = protocol_negotiation::message_kind::protocol,
                                        .protocol = security_protocol}));
      inbound_input.insert(inbound_input.end(), inbound_proposal.begin(), inbound_proposal.end());
      inbound_input.insert(inbound_input.end(), handshake_prefix.begin(), handshake_prefix.end());
      inbound_backend->reads.push_back(std::move(inbound_input));
      auto inbound_stream = forge::net::p2p::stream{
          forge::net::transport::detail::stream_access::make(inbound_backend)};

      const auto inbound_selected = forge::asio::blocking::run(
          runtime, detail::accept_private_stream_security_protocol(inbound_stream));
      BOOST_TEST(inbound_selected.value == security_protocol.value);
      BOOST_TEST(inbound_backend->chunk_reads == 1U);
      const auto inbound_security_input = forge::asio::blocking::run(runtime, inbound_stream.async_read());
      BOOST_TEST(inbound_security_input == handshake_prefix, boost::test_tools::per_element());
      BOOST_TEST(inbound_backend->chunk_reads == 1U);
   }
}

BOOST_AUTO_TEST_CASE(p2p_stream_rejects_oversized_prefetched_frame_with_transport_limit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto backend = std::make_shared<queued_transport_stream>(80);
   const auto options = forge::net::transport::frame_options{.max_size = 3, .max_buffered_size = 7};
   auto oversized = forge::net::transport::encode_frame(std::vector<std::uint8_t>{'l', 'a', 'r', 'g'});
   BOOST_REQUIRE(oversized.size() > options.max_buffered_size);
   auto value = forge::net::p2p::detail::stream_access::with_buffer(
       forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)}, std::move(oversized));

   BOOST_CHECK_THROW((void)forge::asio::blocking::run(runtime, value.async_read_frame(options)),
                     forge::net::transport::exceptions::frame_too_large);
   BOOST_TEST(backend->chunk_reads == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_rejects_oversized_frame_before_reading_body) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto backend = std::make_shared<queued_transport_stream>(80);
   auto value = forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)};
   backend->reads.push_back({0x00, 0x00, 0x04, 0x01});

   try {
      static_cast<void>(forge::asio::blocking::run(
          runtime, peer_exchange_codec::async_read(value, peer_exchange_codec::options{.max_message_size = 1024})));
      BOOST_FAIL("expected peer exchange oversized frame rejection");
   } catch (const forge::exceptions::base&) {
   }
   BOOST_TEST(backend->chunk_reads == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_timeout_cancels_stalled_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto entered = std::make_shared<forge::asio::notification>();
   auto backend = std::make_shared<stalling_transport_stream>(81, entered);
   auto value = forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)};

   const auto observed = entered->epoch();
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       peer_exchange_codec::async_read(value, peer_exchange_codec::options{.max_message_size = 1024}),
       boost::asio::use_future);
   static_cast<void>(forge::asio::blocking::run(runtime, entered->async_wait(observed)));

   auto deadline = operation_deadline{runtime.context(), std::chrono::milliseconds{20}};
   deadline.arm([&value] noexcept { value.cancel(); });
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   try {
      static_cast<void>(pending.get());
      BOOST_FAIL("expected stalled peer exchange read cancellation");
   } catch (const boost::system::system_error& error) {
      BOOST_TEST(error.code() == boost::asio::error::operation_aborted);
   }
   BOOST_TEST(deadline.timed_out());
   BOOST_TEST(backend->closed);
}

BOOST_AUTO_TEST_CASE(p2p_session_lifecycle_ignores_stale_replaced_session) {
   struct tracked_session {
      std::uint64_t id = 0;
      node::session_info info;
   };

   const auto remote = peer(2);
   auto sessions = std::map<std::uint64_t, std::shared_ptr<tracked_session>>{};
   auto stale = std::make_shared<tracked_session>(tracked_session{.id = 1, .info = {.remote_peer = remote}});
   auto current = std::make_shared<tracked_session>(tracked_session{.id = 2, .info = {.remote_peer = remote}});

   sessions[current->id] = current;

   BOOST_TEST(!detail::erase_current_session(sessions, stale));
   BOOST_REQUIRE(sessions.contains(current->id));
   BOOST_TEST(sessions.at(current->id).get() == current.get());

   BOOST_TEST(detail::erase_current_session(sessions, current));
   BOOST_TEST(!sessions.contains(current->id));
}

BOOST_AUTO_TEST_CASE(p2p_session_lifecycle_cancels_rejected_new_session) {
   struct tracked_connection {
      void request_cancel() noexcept {
         canceled = true;
         ++cancel_count;
      }

      bool canceled = false;
      std::size_t cancel_count = 0;
   };
   struct tracked_session {
      bool closed = false;
      tracked_connection connection;
   };

   auto rejected = std::make_shared<tracked_session>();
   detail::cancel_rejected_session(rejected);

   BOOST_TEST(rejected->closed);
   BOOST_TEST(rejected->connection.canceled);
   BOOST_TEST(rejected->connection.cancel_count == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_inbound_stop_between_accept_and_admission_is_not_a_handshake_failure) {
   auto handshakes_failed = std::uint64_t{};
   auto wire_frames = std::vector<std::vector<std::uint8_t>>{};

   for (const auto kind : {exceptions::code::closed, exceptions::code::canceled}) {
      if (!detail::suppress_inbound_handshake_failure(kind, true)) {
         ++handshakes_failed;
         wire_frames.push_back({'f', 'a', 'i', 'l'});
      }
   }

   BOOST_TEST(handshakes_failed == 0U);
   BOOST_TEST(wire_frames.empty());
   BOOST_TEST(!detail::suppress_inbound_handshake_failure(exceptions::code::closed, false));
   BOOST_TEST(!detail::suppress_inbound_handshake_failure(exceptions::code::peer_verification_failed, true));
}

BOOST_AUTO_TEST_CASE(quic_transport_adapter_preserves_endpoint_kind_and_authority) {
   const auto ip4 =
       forge::net::quic::to_transport_endpoint(forge::net::quic::endpoint{.host = "127.0.0.1", .port = 4001});
   BOOST_TEST(static_cast<int>(ip4.host_type) == static_cast<int>(forge::net::transport::endpoint::host_kind::ip4));
   BOOST_TEST(static_cast<int>(ip4.protocol) ==
              static_cast<int>(forge::net::transport::endpoint::protocol_kind::quic_v1));
   const auto authority = std::string{"127.0.0.1"} + ":" + std::to_string(4001);
   BOOST_TEST(ip4.authority() == authority);
   const auto roundtrip = forge::net::quic::from_transport_endpoint(ip4);
   BOOST_TEST(roundtrip.authority() == authority);

   const auto dns =
       forge::net::quic::to_transport_endpoint(forge::net::quic::endpoint{.host = "localhost", .port = 4002});
   BOOST_TEST(static_cast<int>(dns.host_type) == static_cast<int>(forge::net::transport::endpoint::host_kind::dns));
}

BOOST_AUTO_TEST_CASE(p2p_multistream_select_encodes_libp2p_messages) {
   using namespace protocol_negotiation;

   const auto header = encode_frame(encode_message(protocol_negotiation::message{.kind = message_kind::header}));
   BOOST_TEST(header == std::vector<std::uint8_t>({19,  '/', 'm', 'u', 'l', 't', 'i', 's', 't', 'r',
                                                   'e', 'a', 'm', '/', '1', '.', '0', '.', '0', '\n'}),
              boost::test_tools::per_element());

   const auto ping = protocol_id{.value = "/ipfs/ping/1.0.0"};
   auto decoded = decode_message(decode_frame(encode_frame(encode_message(protocol_negotiation::message{
                                                  .kind = message_kind::protocol, .protocol = ping})))
                                     .payload);
   BOOST_TEST(static_cast<int>(decoded.kind) == static_cast<int>(message_kind::protocol));
   BOOST_TEST(decoded.protocol.value == ping.value);

   auto list = decode_message(encode_message(
       protocol_negotiation::message{.kind = message_kind::protocols, .protocols = std::vector<protocol_id>{ping}}));
   BOOST_TEST(static_cast<int>(list.kind) == static_cast<int>(message_kind::protocols));
   BOOST_REQUIRE_EQUAL(list.protocols.size(), 1U);
   BOOST_TEST(list.protocols.front().value == ping.value);

   BOOST_CHECK_THROW((void)decode_frame(std::vector<std::uint8_t>{0x81, 0x81, 0x01}), forge::exceptions::base);
   BOOST_CHECK_THROW((void)decode_message(std::vector<std::uint8_t>{'b', 'a', 'd', '\n'}), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_libp2p_reachability_relay_protocol_ids_are_exact) {
   BOOST_TEST(builtins::autonat_v1.value == "/libp2p/autonat/1.0.0");
   BOOST_TEST(builtins::autonat_v2_dial_request.value == "/libp2p/autonat/2/dial-request");
   BOOST_TEST(builtins::autonat_v2_dial_back.value == "/libp2p/autonat/2/dial-back");
   BOOST_TEST(builtins::relay_hop.value == "/libp2p/circuit/relay/0.2.0/hop");
   BOOST_TEST(builtins::relay_stop.value == "/libp2p/circuit/relay/0.2.0/stop");
   BOOST_TEST(builtins::dcutr.value == "/libp2p/dcutr");
   BOOST_TEST(builtins::kad_dht.value == "/ipfs/kad/1.0.0");
   BOOST_TEST(builtins::rendezvous.value == "/rendezvous/1.0.0");
   BOOST_TEST(builtins::meshsub_v11.value == "/meshsub/1.1.0");
   BOOST_TEST(builtins::meshsub_v10.value == "/meshsub/1.0.0");
}

BOOST_AUTO_TEST_CASE(p2p_reachability_relay_public_types_are_owner_scoped) {
   static_assert(std::is_same_v<decltype(reachability::result{}.value), reachability::state>);
   static_assert(std::is_same_v<decltype(hole_punch::result{}.value), hole_punch::status>);
   static_assert(std::is_same_v<decltype(relay::reservation::info{}.relay_peer), peer_id>);
   static_assert(std::is_same_v<decltype(path::policy{}.allow_relay), bool>);
   static_assert(std::is_same_v<decltype(path::result{}.kind), path::kind>);
   static_assert(std::is_same_v<decltype(resource_manager::snapshot{}.streams.inbound_streams), std::size_t>);
   static_assert(std::is_same_v<decltype(dht::options{}.replication), std::size_t>);
   static_assert(std::is_same_v<decltype(dht::query_result{}.target), dht::key>);
   static_assert(std::is_same_v<decltype(rendezvous::registration{}.peer), peer_id>);
   static_assert(std::is_same_v<decltype(topology::policy{}.dht_enabled), bool>);
   static_assert(std::is_same_v<decltype(topology::policy{}.peers), topology::watermarks>);
   static_assert(std::is_same_v<decltype(discovery::policy{}.max_results), std::size_t>);
   static_assert(std::is_same_v<decltype(node::limits{}.discovery), discovery::policy>);
   static_assert(std::is_same_v<decltype(discovery::result{}.discovered_by), discovery::source>);
}

BOOST_AUTO_TEST_CASE(p2p_legacy_discovery_policy_maps_to_topology_and_rejects_conflicts) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto static_options = options_for(peer(235));
   static_options.limits.discovery.enabled = false;
   auto static_node = node{runtime, std::move(static_options)};
   BOOST_TEST(static_node.diagnostics().topology.mode == "static-only");

   auto conflicting_options = options_for(peer(236));
   conflicting_options.limits.discovery.max_results = 7;
   conflicting_options.limits.topology.max_candidates = 8;
   BOOST_CHECK_THROW((static_cast<void>(node{runtime, std::move(conflicting_options)})), exceptions::invalid_options);

   forge::asio::blocking::run(runtime, static_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_legacy_peer_exchange_mutator_preserves_capability_union) {
   auto store = peer_store{};
   const auto remote = peer(237);
   store.upsert(peer_store::record{.peer = remote, .capabilities = capability_set{.bits = capabilities::direct_quic}});
   store.apply_peer_exchange(remote, capability_set{.bits = capabilities::peer_exchange | capabilities::pubsub});

   const auto learned = store.find(remote);
   BOOST_REQUIRE(learned);
   BOOST_TEST(learned->capabilities.has(capabilities::direct_quic));
   BOOST_TEST(learned->capabilities.has(capabilities::peer_exchange));
   BOOST_TEST(learned->capabilities.has(capabilities::pubsub));
}

BOOST_AUTO_TEST_CASE(p2p_host_address_sanitizer_respects_routed_scope) {
   const auto discovered = peer(238);
   auto private_candidate = make_tcp_endpoint(4001, "192.168.1.20");
   private_candidate.peer = discovered;
   auto public_response_session = make_tcp_endpoint(4002, "8.8.8.8");
   auto private_other_session = make_tcp_endpoint(4003, "192.168.1.21");

   const auto from_public_response =
       host_addresses::sanitize_discovered_endpoints({private_candidate}, discovered,
                                                     host_addresses::learning_context{
                                                         .source = host_addresses::source_kind::routed,
                                                         .remote_endpoint = public_response_session,
                                                     });
   BOOST_TEST(from_public_response.empty());

   const auto from_private_response =
       host_addresses::sanitize_discovered_endpoints({private_candidate}, discovered,
                                                     host_addresses::learning_context{
                                                         .source = host_addresses::source_kind::routed,
                                                         .remote_endpoint = private_other_session,
                                                     });
   BOOST_REQUIRE_EQUAL(from_private_response.size(), 1U);
   BOOST_TEST(from_private_response.front().to_string() == private_candidate.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_dht_codec_roundtrips_libp2p_message_shape_and_rejects_malformed) {
   const auto profile = amino_v1();
   const auto provider = peer(90);
   const auto target = peer(91);
   const auto provider_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4900/quic-v1/p2p/" + provider.to_string());
   const auto target_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4901/quic-v1/p2p/" + target.to_string());
   const auto key = amino_provider_key(std::vector<std::uint8_t>{'f', 'c', 'l', '-', 'd', 'h', 't'});

   const auto encoded = dht::codec::encode(
       dht::message{
           .type = dht::message_type::get_providers,
           .key_value = key,
           .closer_peers = std::vector<dht::peer>{dht::peer{
               .id = target,
               .endpoints = addresses_for({target_endpoint}),
               .connection = dht::connection_type::can_connect,
           }},
           .provider_peers = std::vector<dht::peer>{dht::peer{
               .id = provider,
               .endpoints = addresses_for({provider_endpoint}),
               .connection = dht::connection_type::connected,
           }},
       },
       profile);
   const auto decoded = dht::codec::decode(encoded, profile);

   BOOST_TEST(static_cast<int>(decoded.type) == static_cast<int>(dht::message_type::get_providers));
   BOOST_TEST(decoded.key_value.bytes == key.bytes, boost::test_tools::per_element());
   BOOST_REQUIRE_EQUAL(decoded.closer_peers.size(), 1U);
   BOOST_REQUIRE_EQUAL(decoded.provider_peers.size(), 1U);
   BOOST_TEST(decoded.closer_peers.front().id.to_string() == target.to_string());
   BOOST_TEST(decoded.closer_peers.front().endpoints.front().to_string() == target_endpoint.to_string());
   BOOST_TEST(static_cast<int>(decoded.closer_peers.front().connection) ==
              static_cast<int>(dht::connection_type::can_connect));
   BOOST_TEST(decoded.provider_peers.front().id.to_string() == provider.to_string());
   BOOST_TEST(decoded.provider_peers.front().endpoints.front().to_string() == provider_endpoint.to_string());
   BOOST_TEST(static_cast<int>(decoded.provider_peers.front().connection) ==
              static_cast<int>(dht::connection_type::connected));

   BOOST_CHECK_THROW((void)dht::codec::decode(std::vector<std::uint8_t>{0x02, 0x08, 0x63}, profile),
                     forge::exceptions::base);
   BOOST_CHECK_THROW((void)dht::codec::decode(std::vector<std::uint8_t>{0x01, 0x10}, profile), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_dht_exchange_rejects_mismatched_response_before_admission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto profile = amino_v1();
   const auto request = dht::message{
       .type = dht::message_type::find_node,
       .key_value = make_dht_key(std::vector<std::uint8_t>{'r', 'e', 'q', 'u', 'e', 's', 't'}),
   };
   auto backend = std::make_shared<queued_transport_stream>(51);
   backend->reads.push_back(dht::codec::encode(dht::message{
       .type = dht::message_type::find_node,
       .key_value = make_dht_key(std::vector<std::uint8_t>{'w', 'r', 'o', 'n', 'g'}),
   }));
   auto stream = forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)};

   try {
      (void)forge::asio::blocking::run(runtime,
                                       detail::async_exchange_dht(std::move(stream), request, profile,
                                                                  runtime.context(), std::chrono::milliseconds{250}));
      BOOST_FAIL("expected mismatched DHT response rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::protocol_error));
   }
   BOOST_TEST(backend->closed);
}

BOOST_AUTO_TEST_CASE(p2p_dht_exchange_accepts_omitted_or_echoed_response_key) {
   const auto profile = amino_v1();
   const auto request = dht::message{
       .type = dht::message_type::find_node,
       .key_value = make_dht_key(std::vector<std::uint8_t>{'r', 'e', 'q', 'u', 'e', 's', 't'}),
   };

   BOOST_CHECK_NO_THROW(
       detail::validate_dht_response(request, dht::message{.type = dht::message_type::find_node}, profile));
   BOOST_CHECK_NO_THROW(detail::validate_dht_response(
       request, dht::message{.type = dht::message_type::find_node, .key_value = request.key_value}, profile));
}

BOOST_AUTO_TEST_CASE(p2p_dht_exchange_times_out_while_waiting_for_response) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto profile = amino_v1();
   auto backend = std::make_shared<stalling_transport_stream>(52);
   auto stream = forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(backend)};
   const auto started = std::chrono::steady_clock::now();

   try {
      (void)forge::asio::blocking::run(
          runtime, detail::async_exchange_dht(
                       std::move(stream),
                       dht::message{
                           .type = dht::message_type::find_node,
                           .key_value = make_dht_key(std::vector<std::uint8_t>{'t', 'i', 'm', 'e', 'o', 'u', 't'}),
                       },
                       profile, runtime.context(), std::chrono::milliseconds{25}));
      BOOST_FAIL("expected DHT exchange timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }
   BOOST_TEST(backend->closed);
   BOOST_TEST(
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() <
       1'000);
}

BOOST_AUTO_TEST_CASE(p2p_dht_k_bucket_bounds_active_and_replacement_capacity) {
   const auto local = peer(92);
   const auto peers = peers_in_same_dht_bucket(local, 5);
   auto table = dht::routing_table{
       local, custom_test_dht_profile(
                  dht::mode::client,
                  dht::options{.replication = 2, .alpha = 1, .replacement_cache_size = 2, .failure_threshold = 2})
                  .limits};
   for (const auto& candidate : peers) {
      table.upsert(dht::peer{.id = candidate}, dht::routing_admission::verified_server);
   }
   table.upsert(dht::peer{.id = local});

   const auto status = table.status();
   BOOST_TEST(status.active == 2U);
   BOOST_TEST(status.replacements == 2U);
   BOOST_TEST(status.candidates == 0U);
   BOOST_TEST(status.nonempty_buckets == 1U);
   BOOST_REQUIRE_EQUAL(table.snapshot().size(), 2U);
   BOOST_REQUIRE_EQUAL(table.closest(peers.front().to_bytes(), 20).size(), 2U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_k_bucket_keeps_candidates_out_of_active_routing) {
   const auto local = peer(93);
   const auto peers = peers_in_same_dht_bucket(local, 2);
   auto table = dht::routing_table{
       local, custom_test_dht_profile(dht::mode::client,
                                      dht::options{.replication = 2, .alpha = 1, .replacement_cache_size = 2})
                  .limits};
   table.upsert(dht::peer{.id = peers[0]}, dht::routing_admission::candidate);
   table.upsert(dht::peer{.id = peers[1]}, dht::routing_admission::verified_server);

   const auto status = table.status();
   BOOST_TEST(status.active == 1U);
   BOOST_TEST(status.replacements == 1U);
   BOOST_TEST(status.candidates == 1U);
   const auto active = table.snapshot();
   BOOST_REQUIRE_EQUAL(active.size(), 1U);
   BOOST_TEST(active.front().id.to_string() == peers[1].to_string());

   table.upsert(dht::peer{.id = peers[0]}, dht::routing_admission::verified_server);
   BOOST_TEST(table.status().active == 2U);
   BOOST_TEST(table.status().replacements == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_k_bucket_candidate_updates_cannot_downgrade_verified_entries) {
   const auto local = peer(236);
   const auto peers = peers_in_same_dht_bucket(local, 2);
   auto table = dht::routing_table{
       local, custom_test_dht_profile(
                  dht::mode::client,
                  dht::options{.replication = 1, .alpha = 1, .replacement_cache_size = 1, .failure_threshold = 1})
                  .limits};
   auto active_endpoint = make_dns_tcp_endpoint(4236, "active.example.com");
   active_endpoint.peer = peers[0];
   auto replacement_endpoint = make_dns_tcp_endpoint(4237, "replacement.example.com");
   replacement_endpoint.peer = peers[1];
   table.upsert(dht::peer{.id = peers[0], .endpoints = addresses_for({active_endpoint})},
                dht::routing_admission::verified_server);
   table.upsert(dht::peer{.id = peers[1], .endpoints = addresses_for({replacement_endpoint})},
                dht::routing_admission::verified_server);

   auto untrusted_endpoint = make_dns_tcp_endpoint(4238, "untrusted.example.com");
   untrusted_endpoint.peer = peers[0];
   table.upsert(dht::peer{.id = peers[0], .endpoints = addresses_for({untrusted_endpoint})},
                dht::routing_admission::candidate);
   untrusted_endpoint.peer = peers[1];
   table.upsert(dht::peer{.id = peers[1], .endpoints = addresses_for({untrusted_endpoint})},
                dht::routing_admission::candidate);

   auto active = table.snapshot();
   BOOST_REQUIRE_EQUAL(active.size(), 1U);
   BOOST_REQUIRE_EQUAL(active.front().endpoints.size(), 1U);
   BOOST_TEST(active.front().endpoints.front().to_string() == active_endpoint.to_string());

   table.mark_failure(peers[0]);
   active = table.snapshot();
   BOOST_REQUIRE_EQUAL(active.size(), 1U);
   BOOST_REQUIRE_EQUAL(active.front().endpoints.size(), 1U);
   BOOST_TEST(active.front().id.to_string() == peers[1].to_string());
   BOOST_TEST(active.front().endpoints.front().to_string() == replacement_endpoint.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_seeds_include_bounded_candidates_without_admitting_them) {
   const auto local = peer(237);
   const auto candidates = peers_in_same_dht_bucket(local, 3);
   const auto target = peer(238);
   auto table = dht::routing_table{
       local, custom_test_dht_profile(dht::mode::client,
                                      dht::options{.replication = 2, .alpha = 1, .replacement_cache_size = 3})
                  .limits};
   for (const auto& candidate : candidates) {
      table.upsert(dht::peer{.id = candidate}, dht::routing_admission::candidate);
   }

   BOOST_TEST(table.closest(target.to_bytes(), 10).empty());
   const auto first = table.query_seeds(target.to_bytes(), 2);
   const auto second = table.query_seeds(target.to_bytes(), 2);
   BOOST_REQUIRE_EQUAL(first.size(), 2U);
   BOOST_REQUIRE_EQUAL(second.size(), first.size());
   for (auto index = std::size_t{}; index < first.size(); ++index) {
      BOOST_TEST(first[index].id.to_string() == second[index].id.to_string());
   }
   BOOST_TEST(table.status().active == 0U);
   BOOST_TEST(table.status().candidates == 3U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_seeds_prioritize_active_and_evict_failed_replacements) {
   const auto local = peer(18);
   const auto peers = peers_in_same_dht_bucket(local, 4);
   auto table = dht::routing_table{
       local, custom_test_dht_profile(
                  dht::mode::client,
                  dht::options{.replication = 2, .alpha = 1, .replacement_cache_size = 2, .failure_threshold = 2})
                  .limits};
   table.upsert(dht::peer{.id = peers[0]}, dht::routing_admission::verified_server);
   table.upsert(dht::peer{.id = peers[1]}, dht::routing_admission::verified_server);
   table.upsert(dht::peer{.id = peers[2]}, dht::routing_admission::verified_server);
   table.upsert(dht::peer{.id = peers[3]}, dht::routing_admission::candidate);

   auto expected = std::vector<peer_id>{peers[0], peers[1]};
   std::ranges::sort(expected, [&](const auto& left, const auto& right) {
      const auto left_distance = distance_between(left.to_bytes(), peers[3].to_bytes());
      const auto right_distance = distance_between(right.to_bytes(), peers[3].to_bytes());
      return left_distance != right_distance ? left_distance < right_distance : left < right;
   });
   const auto seeds = table.query_seeds(peers[3].to_bytes(), 2);
   BOOST_REQUIRE_EQUAL(seeds.size(), 2U);
   BOOST_TEST(seeds[0].id.to_string() == expected[0].to_string());
   BOOST_TEST(seeds[1].id.to_string() == expected[1].to_string());

   for (const auto& replacement : {peers[2], peers[3]}) {
      table.mark_failure(replacement);
      table.mark_failure(replacement);
   }
   BOOST_TEST(table.status().replacements == 0U);
   BOOST_TEST(table.status().candidates == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_k_bucket_promotes_verified_replacement_before_candidate) {
   const auto local = peer(94);
   const auto peers = peers_in_same_dht_bucket(local, 3);
   auto table = dht::routing_table{
       local, custom_test_dht_profile(
                  dht::mode::client,
                  dht::options{.replication = 1, .alpha = 1, .replacement_cache_size = 2, .failure_threshold = 1})
                  .limits};
   table.upsert(dht::peer{.id = peers[0]}, dht::routing_admission::verified_server);
   table.upsert(dht::peer{.id = peers[1]}, dht::routing_admission::candidate);
   table.upsert(dht::peer{.id = peers[2]}, dht::routing_admission::verified_server);
   table.mark_failure(peers[0]);

   const auto active = table.snapshot();
   BOOST_REQUIRE_EQUAL(active.size(), 1U);
   BOOST_TEST(active.front().id.to_string() == peers[2].to_string());
   BOOST_TEST(table.status().replacements == 1U);
   BOOST_TEST(table.status().candidates == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_k_bucket_closest_is_sha256_xor_ordered_and_deterministic) {
   const auto local = peer(95);
   const auto candidates = std::vector<peer_id>{peer(96), peer(97), peer(98), peer(99)};
   const auto target = peer(100);
   auto table = dht::routing_table{
       local,
       custom_test_dht_profile(dht::mode::client, dht::options{.replication = candidates.size(), .alpha = 1}).limits};
   for (const auto& candidate : candidates) {
      table.upsert(dht::peer{.id = candidate});
   }

   const auto zero = distance_between(target.to_bytes(), target.to_bytes());
   BOOST_TEST(std::ranges::all_of(zero.bytes, [](auto value) { return value == 0; }));

   auto expected = candidates;
   std::ranges::sort(expected, [&](const auto& left, const auto& right) {
      const auto left_distance = distance_between(left.to_bytes(), target.to_bytes());
      const auto right_distance = distance_between(right.to_bytes(), target.to_bytes());
      return left_distance != right_distance ? left_distance < right_distance : left < right;
   });
   const auto first = table.closest(target.to_bytes(), 3);
   const auto second = table.closest(target.to_bytes(), 3);
   BOOST_REQUIRE_EQUAL(first.size(), 3U);
   BOOST_REQUIRE_EQUAL(second.size(), first.size());
   for (auto index = std::size_t{}; index < first.size(); ++index) {
      BOOST_TEST(first[index].id.to_string() == expected[index].to_string());
      BOOST_TEST(second[index].id.to_string() == first[index].id.to_string());
   }
}

BOOST_AUTO_TEST_CASE(p2p_dht_k_bucket_evicts_after_failure_threshold) {
   const auto local = peer(101);
   const auto peers = peers_in_same_dht_bucket(local, 2);
   auto table = dht::routing_table{
       local, custom_test_dht_profile(
                  dht::mode::client,
                  dht::options{.replication = 1, .alpha = 1, .replacement_cache_size = 1, .failure_threshold = 2})
                  .limits};
   table.upsert(dht::peer{.id = peers[0]});
   table.upsert(dht::peer{.id = peers[1]});

   table.mark_failure(peers[0]);
   BOOST_TEST(table.snapshot().front().id.to_string() == peers[0].to_string());
   table.mark_failure(peers[0]);

   const auto active = table.snapshot();
   BOOST_REQUIRE_EQUAL(active.size(), 1U);
   BOOST_TEST(active.front().id.to_string() == peers[1].to_string());
   BOOST_TEST(table.status().replacements == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_failed_target_seed_is_not_complete_or_closest) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto target = peer(19);
   auto target_endpoint = make_dns_tcp_endpoint(4019, "failed-target.example.com");
   target_endpoint.peer = target;

   const auto result = forge::asio::blocking::run(
       runtime,
       dht_query::run(
           dht_query::request{
               .target = make_dht_key(target),
               .target_peer = target,
               .options = custom_test_dht_profile(dht::mode::client, dht::options{.replication = 2, .alpha = 1}).limits,
               .seeds = std::vector<dht::peer>{dht::peer{
                   .id = target,
                   .endpoints = addresses_for({target_endpoint}),
                   .connection = dht::connection_type::can_connect,
               }},
           },
           [](const dht::peer&) -> boost::asio::awaitable<dht::message> {
              FORGE_THROW_CODE(forge::net::p2p::exceptions::code::timeout, "failed DHT target seed");
              co_return dht::message{};
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }));

   BOOST_TEST(!result.query.complete);
   BOOST_TEST(result.query.closest_peers.empty());
   BOOST_REQUIRE_EQUAL(result.failed.size(), 1U);
   BOOST_TEST(result.failed.front().to_string() == target.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_stops_after_closest_nonfailed_k_peers_are_queried) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto target = make_dht_key(peer(29));
   auto candidates = std::vector<dht::peer>{};
   for (const auto value : {30U, 31U, 32U}) {
      const auto id = peer(static_cast<std::uint8_t>(value));
      auto candidate_endpoint =
          make_dns_tcp_endpoint(static_cast<std::uint16_t>(4'000U + value), "query-convergence.example.com");
      candidate_endpoint.peer = id;
      candidates.push_back(dht::peer{
          .id = id,
          .endpoints = addresses_for({candidate_endpoint}),
          .connection = dht::connection_type::can_connect,
      });
   }
   std::ranges::sort(candidates, [&](const auto& left, const auto& right) {
      const auto left_distance = distance_between(left.id.to_bytes(), target.bytes);
      const auto right_distance = distance_between(right.id.to_bytes(), target.bytes);
      return left_distance != right_distance ? left_distance < right_distance : left.id < right.id;
   });
   auto queried = std::vector<peer_id>{};

   const auto result = forge::asio::blocking::run(
       runtime,
       dht_query::run(
           dht_query::request{
               .target = target,
               .options = custom_test_dht_profile(dht::mode::client, dht::options{.replication = 2, .alpha = 1}).limits,
               .seeds = candidates,
           },
           [&queried](const dht::peer& candidate) -> boost::asio::awaitable<dht::message> {
              queried.push_back(candidate.id);
              co_return dht::message{.type = dht::message_type::find_node};
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }));

   BOOST_REQUIRE_EQUAL(queried.size(), 2U);
   BOOST_TEST(queried[0].to_string() == candidates[0].id.to_string());
   BOOST_TEST(queried[1].to_string() == candidates[1].id.to_string());
   BOOST_REQUIRE_EQUAL(result.query.closest_peers.size(), 2U);
   BOOST_TEST(result.query.closest_peers[0].id.to_string() == candidates[0].id.to_string());
   BOOST_TEST(result.query.closest_peers[1].id.to_string() == candidates[1].id.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_dht_fanout_early_quorum_cancels_and_drains_blocked_sibling) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   using signal_channel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>;
   auto slow_started = std::make_shared<signal_channel>(runtime.context().get_executor(), 1);
   auto slow_wait = std::make_shared<boost::asio::steady_timer>(runtime.context(), std::chrono::hours{1});
   const auto slow = peer(59);
   const auto fast = peer(60);
   const auto unnecessary = peer(61);
   auto slow_drained = false;
   auto unnecessary_started = false;

   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::dht_fanout::run(runtime.context(),
                               detail::dht_fanout::request{
                                   .peers = {slow, fast, unnecessary},
                                   .concurrency = 2,
                                   .success_target = 1,
                                   .timeout = std::chrono::seconds{1},
                                   .operation = "test DHT fanout",
                               },
                               [slow_started, slow_wait, slow, fast, unnecessary, &slow_drained, &unnecessary_started](
                                   const peer_id& current, std::chrono::milliseconds,
                                   std::shared_ptr<cancellation_latch> cancellation) -> boost::asio::awaitable<bool> {
                                  if (current == slow) {
                                     cancellation->arm([slow_wait] noexcept { cancel_timer_noexcept(slow_wait); });
                                     static_cast<void>(slow_started->try_send(boost::system::error_code{}));
                                     auto error = boost::system::error_code{};
                                     co_await slow_wait->async_wait(
                                         boost::asio::redirect_error(boost::asio::use_awaitable, error));
                                     slow_drained = true;
                                     co_return false;
                                  }
                                  if (current == fast) {
                                     co_await slow_started->async_receive(boost::asio::use_awaitable);
                                     co_return true;
                                  }
                                  if (current == unnecessary) {
                                     unnecessary_started = true;
                                     co_return true;
                                  }
                                  throw std::runtime_error{"unexpected DHT fanout peer"};
                               }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = pending.get();
   BOOST_TEST(result.succeeded == 1U);
   BOOST_TEST(result.attempted == 2U);
   BOOST_TEST(slow_drained);
   BOOST_TEST(!unnecessary_started);
}

BOOST_AUTO_TEST_CASE(p2p_dht_fanout_timeout_before_coordinator_spawn_is_sticky) {
   struct barrier_state {
      std::mutex mutex;
      std::condition_variable changed;
      bool stop_requested = false;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto barrier = barrier_state{};
   auto operation_started = std::atomic_bool{false};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::dht_fanout::run(
           runtime.context(),
           detail::dht_fanout::request{
               .peers = {peer(64)},
               .concurrency = 1,
               .success_target = 1,
               .timeout = std::chrono::milliseconds{20},
               .operation = "test pre-spawn DHT fanout timeout",
               .hooks =
                   detail::dht_fanout::test_hooks{
                       .context = &barrier,
                       .reach =
                           [](void* context, detail::dht_fanout::test_stage stage) {
                              auto& value = *static_cast<barrier_state*>(context);
                              if (stage == detail::dht_fanout::test_stage::before_coordinator_spawn) {
                                 auto lock = std::unique_lock{value.mutex};
                                 value.changed.wait(lock, [&value] { return value.stop_requested; });
                              } else if (stage == detail::dht_fanout::test_stage::after_stop_request) {
                                 const auto lock = std::scoped_lock{value.mutex};
                                 value.stop_requested = true;
                                 value.changed.notify_all();
                              }
                           },
                   },
           },
           [&operation_started](const peer_id&, std::chrono::milliseconds,
                                std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
              operation_started.store(true, std::memory_order_release);
              co_return true;
           }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   try {
      static_cast<void>(pending.get());
      BOOST_FAIL("pre-spawn DHT fanout timeout unexpectedly succeeded");
   } catch (const forge::exceptions::base& error) {
      const auto code = exceptions::code_of(error);
      BOOST_REQUIRE(code.has_value());
      BOOST_TEST(static_cast<int>(*code) == static_cast<int>(exceptions::code::timeout));
   }
   BOOST_TEST(!operation_started.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_dht_fanout_worker_failure_cancels_and_joins_sibling) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   using signal_channel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>;
   auto slow_started = std::make_shared<signal_channel>(runtime.context().get_executor(), 1);
   auto slow_wait = std::make_shared<boost::asio::steady_timer>(runtime.context(), std::chrono::hours{1});
   const auto slow = peer(65);
   const auto failing = peer(66);
   auto slow_drained = false;

   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::dht_fanout::run(runtime.context(),
                               detail::dht_fanout::request{
                                   .peers = {slow, failing},
                                   .concurrency = 2,
                                   .success_target = 2,
                                   .timeout = std::chrono::seconds{1},
                                   .operation = "test failing DHT fanout",
                               },
                               [slow_started, slow_wait, slow, &slow_drained](
                                   const peer_id& current, std::chrono::milliseconds,
                                   std::shared_ptr<cancellation_latch> cancellation) -> boost::asio::awaitable<bool> {
                                  if (current == slow) {
                                     cancellation->arm([slow_wait] noexcept { cancel_timer_noexcept(slow_wait); });
                                     static_cast<void>(slow_started->try_send(boost::system::error_code{}));
                                     auto error = boost::system::error_code{};
                                     co_await slow_wait->async_wait(
                                         boost::asio::redirect_error(boost::asio::use_awaitable, error));
                                     slow_drained = error == boost::asio::error::operation_aborted;
                                     co_return false;
                                  }
                                  co_await slow_started->async_receive(boost::asio::use_awaitable);
                                  throw std::runtime_error{"DHT fanout worker failure"};
                               }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(pending.get()), std::runtime_error);
   BOOST_TEST(slow_drained);
}

BOOST_AUTO_TEST_CASE(p2p_topology_dht_fanout_second_spawn_setup_failure_cancels_and_joins_first_worker) {
   struct failure_state {
      std::size_t setups = 0;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto failure = failure_state{};
   auto workers_started = std::atomic_uint{0};
   auto workers_completed = std::atomic_uint{0};
   auto worker_saw_cancellation = std::atomic_bool{false};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::topology_dht_fanout::async_run(detail::topology_dht_fanout::request{
           .executor = runtime.context().get_executor(),
           .workers = 2,
           .cancellation = std::make_shared<cancellation_latch>(),
           .worker = [&](std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              ++workers_started;
              const auto executor = co_await boost::asio::this_coro::executor;
              auto wait = std::make_shared<boost::asio::steady_timer>(executor, std::chrono::hours{1});
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait]() noexcept { cancel_timer_noexcept(wait); },
              }));
              auto error = boost::system::error_code{};
              co_await wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
              worker_saw_cancellation.store(error == boost::asio::error::operation_aborted, std::memory_order_release);
              ++workers_completed;
              co_return;
           },
           .hooks =
               detail::topology_dht_fanout::test_hooks{
                   .context = &failure,
                   .reach =
                       [](void* context, detail::topology_dht_fanout::test_stage stage) {
                          auto& value = *static_cast<failure_state*>(context);
                          if (stage == detail::topology_dht_fanout::test_stage::before_worker_spawn &&
                              ++value.setups == 2) {
                             throw std::bad_alloc{};
                          }
                       },
               },
       }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto launch_failure = pending.get();
   BOOST_REQUIRE(static_cast<bool>(launch_failure));
   BOOST_CHECK_THROW(std::rethrow_exception(launch_failure), std::bad_alloc);
   BOOST_TEST(failure.setups == 2U);
   BOOST_TEST(workers_started.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(workers_completed.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(worker_saw_cancellation.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_topology_dht_fanout_stop_after_publish_before_worker_start_is_sticky) {
   struct barrier_state {
      std::mutex mutex;
      std::condition_variable changed;
      bool published = false;
      bool release_spawn = false;
      std::size_t completions = 0;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto cancellation = std::make_shared<cancellation_latch>();
   auto barrier = barrier_state{};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::topology_dht_fanout::async_run(detail::topology_dht_fanout::request{
           .executor = runtime.context().get_executor(),
           .workers = 1,
           .cancellation = cancellation,
           .worker = [](std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              const auto executor = co_await boost::asio::this_coro::executor;
              auto wait = std::make_shared<boost::asio::steady_timer>(executor, std::chrono::hours{1});
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait]() noexcept { cancel_timer_noexcept(wait); },
              }));
              if (terminal->stop_requested()) {
                 co_return;
              }
              auto error = boost::system::error_code{};
              co_await wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
           },
           .lifecycle = std::addressof(lifecycle),
           .hooks =
               detail::topology_dht_fanout::test_hooks{
                   .context = std::addressof(barrier),
                   .reach =
                       [](void* context, detail::topology_dht_fanout::test_stage stage) {
                          auto& value = *static_cast<barrier_state*>(context);
                          if (stage == detail::topology_dht_fanout::test_stage::before_worker_spawn) {
                             auto lock = std::unique_lock{value.mutex};
                             value.published = true;
                             value.changed.notify_all();
                             value.changed.wait(lock, [&value] { return value.release_spawn; });
                          } else if (stage == detail::topology_dht_fanout::test_stage::before_worker_completion) {
                             const auto lock = std::scoped_lock{value.mutex};
                             ++value.completions;
                          }
                       },
               },
       }),
       boost::asio::use_future);

   {
      auto lock = std::unique_lock{barrier.mutex};
      BOOST_REQUIRE(barrier.changed.wait_for(lock, std::chrono::seconds{1}, [&barrier] { return barrier.published; }));
   }
   cancellation->request_stop();
   {
      const auto lock = std::scoped_lock{barrier.mutex};
      barrier.release_spawn = true;
   }
   barrier.changed.notify_all();

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_TEST(!static_cast<bool>(pending.get()));
   {
      const auto lock = std::scoped_lock{barrier.mutex};
      BOOST_TEST(barrier.completions == 1U);
   }

   lifecycle.request_stop();
   auto drained = boost::asio::co_spawn(runtime.context(), lifecycle.wait(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drained.get());
}

BOOST_AUTO_TEST_CASE(p2p_topology_dht_fanout_stop_wait_setup_failure_cancels_and_joins_worker) {
   auto completion_callbacks = std::atomic_uint{0};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::topology_dht_fanout::async_run(detail::topology_dht_fanout::request{
           .executor = runtime.context().get_executor(),
           .workers = 1,
           .cancellation = std::make_shared<cancellation_latch>(),
           .worker = [](std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              const auto executor = co_await boost::asio::this_coro::executor;
              auto wait = std::make_shared<boost::asio::steady_timer>(executor, std::chrono::hours{1});
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait]() noexcept { cancel_timer_noexcept(wait); },
              }));
              if (terminal->stop_requested()) {
                 co_return;
              }
              auto error = boost::system::error_code{};
              co_await wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
           },
           .lifecycle = std::addressof(lifecycle),
           .hooks =
               detail::topology_dht_fanout::test_hooks{
                   .context = std::addressof(completion_callbacks),
                   .reach =
                       [](void* context, detail::topology_dht_fanout::test_stage stage) {
                          if (stage == detail::topology_dht_fanout::test_stage::before_worker_stop_wait) {
                             throw std::bad_alloc{};
                          }
                          if (stage == detail::topology_dht_fanout::test_stage::before_worker_completion) {
                             static_cast<std::atomic_uint*>(context)->fetch_add(1, std::memory_order_acq_rel);
                          }
                       },
               },
       }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto setup_failure = pending.get();
   BOOST_REQUIRE(static_cast<bool>(setup_failure));
   BOOST_CHECK_THROW(std::rethrow_exception(setup_failure), std::bad_alloc);
   BOOST_TEST(completion_callbacks.load(std::memory_order_acquire) == 1U);

   lifecycle.request_stop();
   auto drained = boost::asio::co_spawn(runtime.context(), lifecycle.wait(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drained.get());
}

BOOST_AUTO_TEST_CASE(p2p_topology_dht_fanout_stop_wait_bridge_is_sticky_and_worker_strand_bound) {
   struct barrier_state {
      std::mutex mutex;
      std::condition_variable changed;
      bool worker_started = false;
      bool release_worker = false;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto worker_strand = boost::asio::make_strand(runtime.context());
   auto cancellation = std::make_shared<cancellation_latch>();
   auto barrier = barrier_state{};
   auto workers_started = std::atomic_uint{0};
   auto workers_completed = std::atomic_uint{0};
   auto worker_saw_cancellation = std::atomic_bool{false};
   auto worker_started_on_strand = std::atomic_bool{false};
   auto worker_resumed_on_strand = std::atomic_bool{false};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::topology_dht_fanout::async_run(detail::topology_dht_fanout::request{
           .executor = worker_strand,
           .workers = 1,
           .cancellation = cancellation,
           .worker = [&](std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              workers_started.fetch_add(1, std::memory_order_acq_rel);
              worker_started_on_strand.store(worker_strand.running_in_this_thread(), std::memory_order_release);
              const auto executor = co_await boost::asio::this_coro::executor;
              auto wait = std::make_shared<boost::asio::steady_timer>(executor, std::chrono::hours{1});
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait]() noexcept { cancel_timer_noexcept(wait); },
              }));
              {
                 auto lock = std::unique_lock{barrier.mutex};
                 barrier.worker_started = true;
                 barrier.changed.notify_all();
                 barrier.changed.wait(lock, [&barrier] { return barrier.release_worker; });
              }
              auto error = boost::system::error_code{};
              co_await wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
              worker_saw_cancellation.store(error == boost::asio::error::operation_aborted, std::memory_order_release);
              worker_resumed_on_strand.store(worker_strand.running_in_this_thread(), std::memory_order_release);
              workers_completed.fetch_add(1, std::memory_order_acq_rel);
           },
       }),
       boost::asio::use_future);

   {
      auto lock = std::unique_lock{barrier.mutex};
      BOOST_REQUIRE(
          barrier.changed.wait_for(lock, std::chrono::seconds{1}, [&barrier] { return barrier.worker_started; }));
   }
   cancellation->request_stop();
   {
      const auto lock = std::scoped_lock{barrier.mutex};
      barrier.release_worker = true;
   }
   barrier.changed.notify_all();

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto run_failure = pending.get();
   BOOST_TEST(!static_cast<bool>(run_failure));
   BOOST_TEST(workers_started.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(workers_completed.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(worker_saw_cancellation.load(std::memory_order_acquire));
   BOOST_TEST(worker_started_on_strand.load(std::memory_order_acquire));
   BOOST_TEST(worker_resumed_on_strand.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_topology_dht_fanout_completion_seals_pending_cancellation_once) {
   struct barrier_state {
      std::mutex mutex;
      std::condition_variable changed;
      bool completion_entered = false;
      bool release_completion = false;
      std::size_t completion_callbacks = 0;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto cancellation = std::make_shared<cancellation_latch>();
   auto barrier = barrier_state{};
   auto workers_started = std::atomic_uint{0};
   auto workers_completed = std::atomic_uint{0};
   auto cancellations = std::atomic_uint{0};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::topology_dht_fanout::async_run(detail::topology_dht_fanout::request{
           .executor = runtime.context().get_executor(),
           .workers = 1,
           .cancellation = cancellation,
           .worker = [&](std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [&cancellations]() noexcept { cancellations.fetch_add(1, std::memory_order_acq_rel); },
              }));
              workers_started.fetch_add(1, std::memory_order_acq_rel);
              workers_completed.fetch_add(1, std::memory_order_acq_rel);
              co_return;
           },
           .lifecycle = std::addressof(lifecycle),
           .hooks =
               detail::topology_dht_fanout::test_hooks{
                   .context = std::addressof(barrier),
                   .reach =
                       [](void* context, detail::topology_dht_fanout::test_stage stage) {
                          if (stage != detail::topology_dht_fanout::test_stage::before_worker_completion) {
                             return;
                          }
                          auto& value = *static_cast<barrier_state*>(context);
                          auto lock = std::unique_lock{value.mutex};
                          ++value.completion_callbacks;
                          value.completion_entered = true;
                          value.changed.notify_all();
                          value.changed.wait(lock, [&value] { return value.release_completion; });
                       },
               },
       }),
       boost::asio::use_future);

   {
      auto lock = std::unique_lock{barrier.mutex};
      BOOST_REQUIRE(
          barrier.changed.wait_for(lock, std::chrono::seconds{1}, [&barrier] { return barrier.completion_entered; }));
   }
   cancellation->request_stop();
   {
      const auto lock = std::scoped_lock{barrier.mutex};
      barrier.release_completion = true;
   }
   barrier.changed.notify_all();

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto run_failure = pending.get();
   BOOST_TEST(!static_cast<bool>(run_failure));
   BOOST_TEST(workers_started.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(workers_completed.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(cancellations.load(std::memory_order_acquire) == 0U);
   {
      const auto lock = std::scoped_lock{barrier.mutex};
      BOOST_TEST(barrier.completion_callbacks == 1U);
   }

   lifecycle.request_stop();
   auto drained = boost::asio::co_spawn(runtime.context(), lifecycle.wait(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drained.get());
}

BOOST_AUTO_TEST_CASE(p2p_topology_dht_fanout_join_failure_cancels_lifecycle_tracked_worker) {
   struct failure_state {
      bool fail_join = true;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   auto failure = failure_state{};
   auto worker_started = std::atomic_bool{false};
   auto worker_completed = std::atomic_bool{false};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::topology_dht_fanout::async_run(detail::topology_dht_fanout::request{
           .executor = runtime.context().get_executor(),
           .workers = 1,
           .cancellation = std::make_shared<cancellation_latch>(),
           .worker = [&](std::shared_ptr<detail::worker_terminal_owner> terminal) -> boost::asio::awaitable<void> {
              worker_started.store(true, std::memory_order_release);
              const auto executor = co_await boost::asio::this_coro::executor;
              auto wait = std::make_shared<boost::asio::steady_timer>(executor, std::chrono::hours{1});
              BOOST_REQUIRE(terminal->publish(detail::worker_terminal_owner::callback{
                  [wait]() noexcept { cancel_timer_noexcept(wait); },
              }));
              auto error = boost::system::error_code{};
              co_await wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
              worker_completed.store(true, std::memory_order_release);
           },
           .lifecycle = std::addressof(lifecycle),
           .hooks =
               detail::topology_dht_fanout::test_hooks{
                   .context = &failure,
                   .reach =
                       [](void* context, detail::topology_dht_fanout::test_stage stage) {
                          const auto& value = *static_cast<failure_state*>(context);
                          if (value.fail_join && stage == detail::topology_dht_fanout::test_stage::before_join_wait) {
                             throw std::bad_alloc{};
                          }
                       },
               },
       }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto join_failure = pending.get();
   BOOST_REQUIRE(static_cast<bool>(join_failure));
   BOOST_CHECK_THROW(std::rethrow_exception(join_failure), std::bad_alloc);

   lifecycle.request_stop();
   auto drained = boost::asio::co_spawn(runtime.context(), lifecycle.wait(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drained.get());
   BOOST_TEST(worker_started.load(std::memory_order_acquire));
   BOOST_TEST(worker_completed.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_dht_fanout_full_target_attempts_every_closest_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto first = peer(62);
   const auto second = peer(63);
   auto attempted = std::vector<peer_id>{};

   const auto result = forge::asio::blocking::run(
       runtime,
       detail::dht_fanout::run(runtime.context(),
                               detail::dht_fanout::request{
                                   .peers = {first, second},
                                   .concurrency = 1,
                                   .success_target = 2,
                                   .timeout = std::chrono::seconds{1},
                                   .operation = "test full DHT fanout",
                               },
                               [&attempted](const peer_id& current, std::chrono::milliseconds,
                                            std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
                                  attempted.push_back(current);
                                  co_return true;
                               }));

   BOOST_TEST(result.succeeded == 2U);
   BOOST_TEST(result.attempted == 2U);
   BOOST_REQUIRE_EQUAL(attempted.size(), 2U);
   BOOST_TEST(attempted[0].to_string() == first.to_string());
   BOOST_TEST(attempted[1].to_string() == second.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_fast_completion_advances_while_sibling_is_blocked) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   using signal_channel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>;
   auto slow_started = std::make_shared<signal_channel>(runtime.context().get_executor(), 1);
   auto slow_wait = std::make_shared<boost::asio::steady_timer>(runtime.context(), std::chrono::hours{1});
   const auto slow = peer(45);
   const auto fast = peer(46);
   const auto replacement = peer(47);
   const auto provider = peer(48);
   const auto candidate = [](peer_id id, std::uint16_t port, std::string host) {
      auto endpoint = make_dns_tcp_endpoint(port, std::move(host));
      endpoint.peer = id;
      return dht::peer{.id = std::move(id),
                       .endpoints = addresses_for({endpoint}),
                       .connection = dht::connection_type::can_connect};
   };
   const auto slow_candidate = candidate(slow, 4'045, "query-event-slow.example.com");
   const auto fast_candidate = candidate(fast, 4'046, "query-event-fast.example.com");
   const auto replacement_candidate = candidate(replacement, 4'047, "query-event-next.example.com");
   auto slow_drained = false;
   auto advanced_before_slow_drain = false;

   auto pending = boost::asio::co_spawn(
       runtime.context(),
       dht_query::run(
           dht_query::request{
               .target = make_dht_key(peer(49)),
               .options = custom_test_dht_profile(dht::mode::client,
                                                  dht::options{.replication = 3, .alpha = 2, .max_query_peers = 4})
                              .limits,
               .seeds = std::vector<dht::peer>{slow_candidate, fast_candidate},
               .requested_provider_count = 1,
           },
           [slow_started, slow_wait, slow, fast, replacement, provider, replacement_candidate, &slow_drained,
            &advanced_before_slow_drain](const dht::peer& current) -> boost::asio::awaitable<dht::message> {
              if (current.id == slow) {
                 static_cast<void>(slow_started->try_send(boost::system::error_code{}));
                 auto error = boost::system::error_code{};
                 co_await slow_wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                 slow_drained = true;
                 co_return dht::message{.type = dht::message_type::find_node};
              }
              if (current.id == fast) {
                 co_await slow_started->async_receive(boost::asio::use_awaitable);
                 co_return dht::message{.type = dht::message_type::find_node,
                                        .closer_peers = std::vector<dht::peer>{replacement_candidate}};
              }
              if (current.id == replacement) {
                 advanced_before_slow_drain = !slow_drained;
                 co_return dht::message{
                     .type = dht::message_type::get_providers,
                     .provider_peers = std::vector<dht::peer>{dht::peer{.id = provider}},
                 };
              }
              throw std::runtime_error{"unexpected DHT query candidate"};
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = pending.get();
   BOOST_TEST(advanced_before_slow_drain);
   BOOST_TEST(slow_drained);
   BOOST_REQUIRE_EQUAL(result.query.provider_peers.size(), 1U);
   BOOST_TEST(result.query.provider_peers.front().id.to_string() == provider.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_early_target_success_cancels_and_drains_children) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   using signal_channel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>;
   auto slow_started = std::make_shared<signal_channel>(runtime.context().get_executor(), 1);
   auto slow_wait = std::make_shared<boost::asio::steady_timer>(runtime.context(), std::chrono::hours{1});
   const auto target = peer(50);
   const auto slow = peer(51);
   auto target_endpoint = make_dns_tcp_endpoint(4'050, "query-target-success.example.com");
   target_endpoint.peer = target;
   auto slow_endpoint = make_dns_tcp_endpoint(4'051, "query-target-slow.example.com");
   slow_endpoint.peer = slow;
   auto slow_drained = false;

   auto pending = boost::asio::co_spawn(
       runtime.context(),
       dht_query::run(
           dht_query::request{
               .target = make_dht_key(target),
               .target_peer = target,
               .options = custom_test_dht_profile(dht::mode::client, dht::options{.replication = 2, .alpha = 2}).limits,
               .seeds =
                   std::vector<dht::peer>{
                       dht::peer{.id = target,
                                 .endpoints = addresses_for({target_endpoint}),
                                 .connection = dht::connection_type::can_connect},
                       dht::peer{.id = slow,
                                 .endpoints = addresses_for({slow_endpoint}),
                                 .connection = dht::connection_type::can_connect},
                   },
           },
           [slow_started, slow_wait, target, slow,
            &slow_drained](const dht::peer& current) -> boost::asio::awaitable<dht::message> {
              if (current.id == slow) {
                 static_cast<void>(slow_started->try_send(boost::system::error_code{}));
                 auto error = boost::system::error_code{};
                 co_await slow_wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                 slow_drained = true;
                 co_return dht::message{.type = dht::message_type::find_node};
              }
              if (current.id == target) {
                 co_await slow_started->async_receive(boost::asio::use_awaitable);
                 co_return dht::message{.type = dht::message_type::find_node};
              }
              throw std::runtime_error{"unexpected DHT query candidate"};
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = pending.get();
   BOOST_TEST(result.query.complete);
   BOOST_TEST(slow_drained);
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_value_quorum_cancels_and_drains_children) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   using signal_channel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>;
   auto slow_started = std::make_shared<signal_channel>(runtime.context().get_executor(), 1);
   auto slow_wait = std::make_shared<boost::asio::steady_timer>(runtime.context(), std::chrono::hours{1});
   const auto slow = peer(62);
   const auto fast = peer(63);
   const auto unnecessary = peer(64);
   const auto overflow = peer(65);
   const auto candidate = [](peer_id id, std::uint16_t port) {
      auto address = make_dns_tcp_endpoint(port, "value-quorum.example.com");
      address.peer = id;
      return dht::peer{
          .id = std::move(id), .endpoints = addresses_for({address}), .connection = dht::connection_type::can_connect};
   };
   auto slow_drained = false;
   auto unnecessary_started = false;
   auto accepted = std::size_t{};
   const auto key = make_dht_key(std::vector<std::uint8_t>{'q', 'u', 'o', 'r', 'u', 'm'});

   auto pending = boost::asio::co_spawn(
       runtime.context(),
       dht_query::run(
           dht_query::request{
               .target = key,
               .options = custom_test_dht_profile(dht::mode::client,
                                                  dht::options{.replication = 3, .alpha = 2, .max_query_peers = 3})
                              .limits,
               .seeds = {candidate(slow, 4'062), candidate(fast, 4'063), candidate(unnecessary, 4'064)},
               .collect_value_responses = true,
           },
           [slow_started, slow_wait, slow, fast, unnecessary, overflow, key, candidate, &slow_drained,
            &unnecessary_started](const dht::peer& current) -> boost::asio::awaitable<dht::message> {
              if (current.id == slow) {
                 static_cast<void>(slow_started->try_send(boost::system::error_code{}));
                 auto error = boost::system::error_code{};
                 co_await slow_wait->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
                 slow_drained = true;
                 co_return dht::message{.type = dht::message_type::get_value};
              }
              if (current.id == fast) {
                 co_await slow_started->async_receive(boost::asio::use_awaitable);
                 co_return dht::message{
                     .type = dht::message_type::get_value,
                     .record_value = dht::record{.key_value = key, .value = {'v'}},
                     .closer_peers = {candidate(overflow, 4'065)},
                 };
              }
              if (current.id == unnecessary) {
                 unnecessary_started = true;
                 co_return dht::message{.type = dht::message_type::get_value};
              }
              throw std::runtime_error{"unexpected DHT value quorum candidate"};
           },
           [&accepted](const dht::peer&, dht::message& response) -> boost::asio::awaitable<bool> {
              if (response.record_value) {
                 ++accepted;
              }
              co_return accepted >= 1;
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = pending.get();
   BOOST_TEST(accepted == 1U);
   BOOST_TEST(slow_drained);
   BOOST_TEST(!unnecessary_started);
   BOOST_REQUIRE_EQUAL(result.value_responses.size(), 1U);
   BOOST_REQUIRE(result.value_responses.front().second.has_value());
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_discovered_peer_bound_keeps_closest_candidates) {
   BOOST_TEST(amino_v1().limits.max_query_peers == 256U);
   auto invalid_limits = dht::options{};
   invalid_limits.max_query_peers = 0;
   BOOST_CHECK_THROW(static_cast<void>(custom_test_dht_profile(dht::mode::client, invalid_limits)),
                     exceptions::invalid_options);

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto seed = peer(52);
   auto seed_endpoint = make_dns_tcp_endpoint(4'052, "query-bound-seed.example.com");
   seed_endpoint.peer = seed;
   const auto discovered = [](std::uint8_t value, std::uint16_t port) {
      const auto id = peer(value);
      auto endpoint = make_dns_tcp_endpoint(port, "query-bound-discovered.example.com");
      endpoint.peer = id;
      return dht::peer{.id = id,
                       .endpoints = addresses_for({endpoint}),
                       .connection = dht::connection_type::can_connect};
   };
   auto attempts = std::size_t{};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       dht_query::run(
           dht_query::request{
               .target = make_dht_key(peer(55)),
               .options = custom_test_dht_profile(dht::mode::client,
                                                  dht::options{.replication = 2, .alpha = 1, .max_query_peers = 2})
                              .limits,
               .seeds = std::vector<dht::peer>{dht::peer{
                   .id = seed,
                   .endpoints = addresses_for({seed_endpoint}),
                   .connection = dht::connection_type::can_connect,
               }},
           },
           [&attempts, discovered](const dht::peer&) -> boost::asio::awaitable<dht::message> {
              ++attempts;
              co_return dht::message{
                  .type = dht::message_type::find_node,
                  .closer_peers = std::vector<dht::peer>{discovered(53, 4'053), discovered(54, 4'054)},
              };
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = pending.get();
   BOOST_TEST(attempts >= 2U);
   BOOST_TEST(attempts <= 3U);
   BOOST_TEST(result.query.closest_peers.size() == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_failure_is_not_also_reported_as_queried) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto successful = peer(56);
   const auto failed = peer(57);
   const auto candidate = [](peer_id id, std::uint16_t port) {
      auto endpoint = make_dns_tcp_endpoint(port, "query-accounting.example.com");
      endpoint.peer = id;
      return dht::peer{.id = std::move(id),
                       .endpoints = addresses_for({endpoint}),
                       .connection = dht::connection_type::can_connect};
   };
   auto attempts = std::map<peer_id, std::size_t>{};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       dht_query::run(
           dht_query::request{
               .target = make_dht_key(peer(58)),
               .options = custom_test_dht_profile(dht::mode::client, dht::options{.replication = 2, .alpha = 2}).limits,
               .seeds = std::vector<dht::peer>{candidate(successful, 4'056), candidate(failed, 4'057)},
           },
           [&attempts, failed](const dht::peer& current) -> boost::asio::awaitable<dht::message> {
              ++attempts[current.id];
              if (current.id == failed) {
                 FORGE_THROW_CODE(exceptions::code::timeout, "injected DHT peer failure");
              }
              co_return dht::message{.type = dht::message_type::find_node};
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }),
       boost::asio::use_future);

   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = pending.get();
   BOOST_REQUIRE_EQUAL(result.queried.size(), 1U);
   BOOST_REQUIRE_EQUAL(result.failed.size(), 1U);
   BOOST_TEST(result.queried.front().to_string() == successful.to_string());
   BOOST_TEST(result.failed.front().to_string() == failed.to_string());
   BOOST_TEST(!std::ranges::contains(result.queried, failed));
   BOOST_TEST(attempts[successful] == 1U);
   BOOST_TEST(attempts[failed] == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_query_cancels_and_joins_children_with_parent) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto blocker = std::make_shared<boost::asio::steady_timer>(runtime.context(), std::chrono::hours{1});
   auto callable_lifetime = std::make_shared<int>(42);
   auto weak_lifetime = std::weak_ptr<int>{callable_lifetime};
   auto entered = std::promise<void>{};
   auto entered_future = entered.get_future();
   auto entered_count = std::size_t{};
   auto cancellation = boost::asio::cancellation_signal{};
   auto first_endpoint = make_dns_tcp_endpoint(4'042, "query-lifetime-a.example.com");
   first_endpoint.peer = peer(42);
   auto second_endpoint = make_dns_tcp_endpoint(4'043, "query-lifetime-b.example.com");
   second_endpoint.peer = peer(44);

   auto result = boost::asio::co_spawn(
       runtime.context(),
       dht_query::run(
           dht_query::request{
               .target = make_dht_key(peer(43)),
               .options = custom_test_dht_profile(dht::mode::client, dht::options{.replication = 2, .alpha = 2}).limits,
               .seeds =
                   std::vector<dht::peer>{
                       dht::peer{
                           .id = peer(42),
                           .endpoints = addresses_for({first_endpoint}),
                           .connection = dht::connection_type::can_connect,
                       },
                       dht::peer{
                           .id = peer(44),
                           .endpoints = addresses_for({second_endpoint}),
                           .connection = dht::connection_type::can_connect,
                       },
                   },
           },
           [blocker, owner = callable_lifetime, &entered,
            &entered_count](const dht::peer&) -> boost::asio::awaitable<dht::message> {
              if (++entered_count == 2) {
                 entered.set_value();
              }
              auto error = boost::system::error_code{};
              co_await blocker->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
              co_return dht::message{.type = dht::message_type::find_node};
           },
           [](const dht::peer&, const forge::exceptions::base&) { return true; }),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   callable_lifetime.reset();
   BOOST_REQUIRE(entered_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

   cancellation.emit(boost::asio::cancellation_type::terminal);
   BOOST_REQUIRE(result.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(result.get()), boost::system::system_error);
   BOOST_TEST(weak_lifetime.expired());
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_codec_roundtrips_register_discover_cookie_and_status) {
   const auto opts = rendezvous::options{};
   BOOST_TEST(opts.min_ttl == std::chrono::seconds{7'200});
   const auto identity = make_test_identity();
   const auto legacy_payload_type =
       std::vector<std::uint8_t>{'/', 'l', 'i', 'b', 'p', '2', 'p', '/', 'r', 'o', 'u', 't', 'i', 'n',
                                 'g', '-', 's', 't', 'a', 't', 'e', '-', 'r', 'e', 'c', 'o', 'r', 'd'};
   BOOST_TEST(rendezvous::codec::peer_record_payload_type() == legacy_payload_type, boost::test_tools::per_element());
   const auto record = make_signed_rendezvous_peer_record(identity, {}, 42);
   const auto encoded_register = rendezvous::codec::encode(rendezvous::message{
       .type = rendezvous::message_type::register_peer,
       .register_value =
           rendezvous::register_request{
               .namespace_name = "forge.discovery",
               .signed_peer_record = record,
               .ttl = std::chrono::seconds{3},
           },
   });
   const auto decoded_register = rendezvous::codec::decode(encoded_register, opts);
   BOOST_TEST(static_cast<int>(decoded_register.type) == static_cast<int>(rendezvous::message_type::register_peer));
   BOOST_REQUIRE(decoded_register.register_value.has_value());
   BOOST_TEST(decoded_register.register_value->namespace_name == "forge.discovery");
   BOOST_TEST(decoded_register.register_value->signed_peer_record == record, boost::test_tools::per_element());
   BOOST_TEST(decoded_register.register_value->ttl == std::chrono::seconds{3});

   const auto append_varint = [](std::vector<std::uint8_t>& out, std::uint64_t value) {
      do {
         auto byte = static_cast<std::uint8_t>(value & 0x7fU);
         value >>= 7U;
         if (value != 0) {
            byte |= 0x80U;
         }
         out.push_back(byte);
      } while (value != 0);
   };
   const auto frame = [&append_varint](std::vector<std::uint8_t> payload) {
      auto out = std::vector<std::uint8_t>{};
      append_varint(out, payload.size());
      out.insert(out.end(), payload.begin(), payload.end());
      return out;
   };
   const auto register_with_ttl = [&append_varint, &frame](std::uint64_t ttl) {
      auto register_value = std::vector<std::uint8_t>{0x0a, 0x01, 'n', 0x12, 0x01, 0x01, 0x18};
      append_varint(register_value, ttl);
      auto payload = std::vector<std::uint8_t>{0x08, 0x00, 0x12};
      append_varint(payload, register_value.size());
      payload.insert(payload.end(), register_value.begin(), register_value.end());
      return frame(std::move(payload));
   };
   const auto register_response_with_ttl = [&append_varint, &frame](std::uint64_t ttl) {
      auto response_value = std::vector<std::uint8_t>{0x08, 0x00, 0x18};
      append_varint(response_value, ttl);
      auto payload = std::vector<std::uint8_t>{0x08, 0x01, 0x1a};
      append_varint(payload, response_value.size());
      payload.insert(payload.end(), response_value.begin(), response_value.end());
      return frame(std::move(payload));
   };
   auto unbounded_ttl = opts;
   unbounded_ttl.max_ttl = std::chrono::seconds::max();
   const auto signed_maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
   const auto decoded_maximum_register = rendezvous::codec::decode(register_with_ttl(signed_maximum), unbounded_ttl);
   BOOST_REQUIRE(decoded_maximum_register.register_value.has_value());
   BOOST_TEST(decoded_maximum_register.register_value->ttl == std::chrono::seconds::max());
   const auto decoded_maximum_response =
       rendezvous::codec::decode(register_response_with_ttl(signed_maximum), unbounded_ttl);
   BOOST_REQUIRE(decoded_maximum_response.register_response_value.has_value());
   BOOST_TEST(decoded_maximum_response.register_response_value->ttl == std::chrono::seconds::max());
   BOOST_CHECK_THROW(
       (void)rendezvous::codec::decode(register_with_ttl((std::numeric_limits<std::uint64_t>::max)()), unbounded_ttl),
       exceptions::codec_error);
   BOOST_CHECK_THROW((void)rendezvous::codec::decode(
                         register_response_with_ttl((std::numeric_limits<std::uint64_t>::max)()), unbounded_ttl),
                     exceptions::codec_error);

   BOOST_CHECK_THROW((void)rendezvous::codec::encode(rendezvous::message{
                         .type = rendezvous::message_type::register_peer,
                         .register_value =
                             rendezvous::register_request{
                                 .namespace_name = "forge.discovery",
                                 .signed_peer_record = record,
                                 .ttl = std::chrono::seconds{-1},
                             },
                     }),
                     exceptions::invalid_options);

   const auto decoded_peer_record = rendezvous::codec::open_peer_record(signed_envelope::decode(record), identity.peer);
   BOOST_TEST(decoded_peer_record.peer.to_string() == identity.peer.to_string());
   BOOST_REQUIRE_EQUAL(decoded_peer_record.endpoints.size(), 1U);
   BOOST_TEST(decoded_peer_record.sequence == 42U);

   const auto other_identity = make_test_identity();
   const auto mismatched_record = rendezvous::codec::seal_peer_record(
       rendezvous::peer_record{
           .peer = other_identity.peer,
           .endpoints = {},
           .sequence = 43,
       },
       identity.key, forge::crypto::pki::pem::read_private_key(identity.private_key_pem));
   BOOST_CHECK_THROW((void)rendezvous::codec::open_peer_record(mismatched_record), exceptions::invalid_identity);

   const auto cookie = rendezvous::codec::make_cookie(42, "forge.discovery");
   BOOST_TEST(rendezvous::codec::read_cookie(cookie) == 42U);
   BOOST_TEST(rendezvous::codec::read_cookie_namespace(cookie) == "forge.discovery");
   const auto encoded_discover = rendezvous::codec::encode(rendezvous::message{
       .type = rendezvous::message_type::discover_response,
       .discover_response_value =
           rendezvous::discover_response{
               .registrations = std::vector<rendezvous::registration>{rendezvous::registration{
                   .namespace_name = "forge.discovery",
                   .signed_peer_record = record,
                   .ttl = std::chrono::seconds{7'200},
               }},
               .cookie = cookie,
               .status_value = rendezvous::status::ok,
           },
   });
   const auto decoded_discover = rendezvous::codec::decode(encoded_discover, opts);
   BOOST_TEST(static_cast<int>(decoded_discover.type) == static_cast<int>(rendezvous::message_type::discover_response));
   BOOST_REQUIRE(decoded_discover.discover_response_value.has_value());
   BOOST_REQUIRE_EQUAL(decoded_discover.discover_response_value->registrations.size(), 1U);
   BOOST_TEST(decoded_discover.discover_response_value->registrations.front().namespace_name == "forge.discovery");
   BOOST_TEST(decoded_discover.discover_response_value->registrations.front().peer.to_string() ==
              identity.peer.to_string());
   BOOST_TEST(decoded_discover.discover_response_value->registrations.front().signed_peer_record == record,
              boost::test_tools::per_element());
   BOOST_TEST(rendezvous::codec::read_cookie(decoded_discover.discover_response_value->cookie) == 42U);

   BOOST_CHECK_THROW((void)rendezvous::codec::read_cookie(std::vector<std::uint8_t>{1, 2, 3}), forge::exceptions::base);
   BOOST_CHECK_THROW((void)rendezvous::codec::encode(rendezvous::message{
                         .type = rendezvous::message_type::discover,
                         .discover_value = rendezvous::discover_request{.namespace_name = std::string(300, 'x')},
                     }),
                     forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_peer_record_uses_canonical_libp2p_domain_type_and_wire_bytes) {
   const auto expected_payload_type = std::vector<std::uint8_t>{0x03, 0x01};

   const auto fixture = rendezvous::codec::encode_peer_record(rendezvous::peer_record{
       .peer = peer(42),
       .endpoints = addresses_for({parse_endpoint("/ip4/127.0.0.1/tcp/4001")}),
       .sequence = 42,
   });
   const auto canonical_go_rust_payload = std::vector<std::uint8_t>{
       0x0a, 0x03, 0x00, 0x01, 0x2a, 0x10, 0x2a, 0x1a, 0x0a, 0x0a, 0x08, 0x04, 0x7f, 0x00, 0x00, 0x01, 0x06, 0x0f, 0xa1,
   };
   BOOST_TEST(fixture == canonical_go_rust_payload, boost::test_tools::per_element());

   const auto dnsaddr =
       forge::multiformats::multiaddr::parse("/dnsaddr/rendezvous.example/p2p/" + peer(42).to_string());
   const auto dnsaddr_decoded = rendezvous::codec::decode_peer_record(rendezvous::codec::encode_peer_record(
       rendezvous::peer_record{.peer = peer(42), .endpoints = {dnsaddr}, .sequence = 43}));
   BOOST_REQUIRE_EQUAL(dnsaddr_decoded.endpoints.size(), 1U);
   BOOST_TEST(dnsaddr_decoded.endpoints.front().to_string() == dnsaddr.to_string());

   auto mixed_valid_and_malformed = fixture;
   const auto malformed_address_info = std::array<std::uint8_t, 5>{0x1a, 0x03, 0x0a, 0x01, 0xff};
   mixed_valid_and_malformed.insert(mixed_valid_and_malformed.end(), malformed_address_info.begin(),
                                    malformed_address_info.end());
   BOOST_CHECK_THROW((void)rendezvous::codec::decode_peer_record(mixed_valid_and_malformed), exceptions::codec_error);

   const auto identity = make_test_identity();
   const auto envelope = signed_envelope::decode(make_signed_identify_peer_record(identity));
   BOOST_TEST(envelope.payload_type == expected_payload_type, boost::test_tools::per_element());
   BOOST_CHECK_NO_THROW(envelope.verify("libp2p-peer-record", identity.peer));
   BOOST_CHECK_THROW(envelope.verify("libp2p-routing-state", identity.peer), forge::exceptions::base);
   BOOST_CHECK_THROW((void)rendezvous::codec::open_peer_record(envelope, identity.peer), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_peer_record_canonicalizes_relay_suffixes_and_rejects_invalid_carriers) {
   const auto target = peer(42);
   const auto relay = peer(43);
   const auto other = peer(44);
   const auto relay_carrier = forge::multiformats::multiaddr::parse(
       "/ip4/8.8.8.8/tcp/4001/p2p/" + relay.to_string() + "/p2p-circuit/p2p/" + target.to_string());

   const auto normalized = rendezvous::codec::decode_peer_record(rendezvous::codec::encode_peer_record(
       rendezvous::peer_record{.peer = target, .endpoints = {relay_carrier}, .sequence = 44}));
   BOOST_REQUIRE_EQUAL(normalized.endpoints.size(), 1U);
   BOOST_TEST(normalized.endpoints.front().to_string() == relay_carrier.to_string());

   const auto anonymous_relay_carrier = forge::multiformats::multiaddr::parse(
       "/ip4/8.8.8.8/tcp/4001/p2p-circuit/p2p/" + target.to_string());
   const auto anonymous_normalized = rendezvous::codec::decode_peer_record(rendezvous::codec::encode_peer_record(
       rendezvous::peer_record{.peer = target, .endpoints = {anonymous_relay_carrier}, .sequence = 45}));
   BOOST_REQUIRE_EQUAL(anonymous_normalized.endpoints.size(), 1U);
   BOOST_TEST(anonymous_normalized.endpoints.front().to_string() == anonymous_relay_carrier.to_string());

   const auto malformed_relay = forge::multiformats::multiaddr::parse(
       "/dnsaddr/relay.example/p2p/1/p2p-circuit/p2p/" + target.to_string());
   BOOST_CHECK_THROW((void)rendezvous::codec::encode_peer_record(
                         rendezvous::peer_record{.peer = target, .endpoints = {malformed_relay}, .sequence = 46}),
                     exceptions::invalid_identity);

   const auto unrelated_after_circuit = forge::multiformats::multiaddr::parse(
       "/dnsaddr/relay.example/p2p-circuit/tcp/4001/p2p/" + target.to_string());
   BOOST_CHECK_THROW((void)rendezvous::codec::encode_peer_record(
                         rendezvous::peer_record{.peer = target, .endpoints = {unrelated_after_circuit}, .sequence = 47}),
                     exceptions::invalid_identity);

   const auto nonterminal_target = forge::multiformats::multiaddr::parse(
       "/dnsaddr/target.example/p2p/" + target.to_string() + "/tcp/4001");
   BOOST_CHECK_THROW((void)rendezvous::codec::encode_peer_record(
                         rendezvous::peer_record{.peer = target, .endpoints = {nonterminal_target}, .sequence = 48}),
                     exceptions::invalid_identity);

   const auto conflicting_terminal =
       forge::multiformats::multiaddr::parse("/dnsaddr/target.example/p2p/" + other.to_string());
   BOOST_CHECK_THROW((void)rendezvous::codec::encode_peer_record(
                         rendezvous::peer_record{.peer = target, .endpoints = {conflicting_terminal}, .sequence = 49}),
                     exceptions::invalid_identity);

   const auto valid_relay = forge::multiformats::multiaddr::parse(
       relay_carrier.to_string());
   const auto conflicting_relay = forge::multiformats::multiaddr::parse(
       "/ip4/8.8.8.8/tcp/4001/p2p/" + relay.to_string() + "/p2p-circuit/p2p/" + other.to_string());
   auto mixed = rendezvous::codec::encode_peer_record(
       rendezvous::peer_record{.peer = target, .endpoints = {valid_relay, valid_relay}, .sequence = 50});
   const auto valid_bytes = valid_relay.to_bytes();
   const auto conflicting_bytes = conflicting_relay.to_bytes();
   BOOST_REQUIRE_EQUAL(valid_bytes.size(), conflicting_bytes.size());
   const auto first = std::search(mixed.begin(), mixed.end(), valid_bytes.begin(), valid_bytes.end());
   BOOST_REQUIRE(first != mixed.end());
   const auto second =
       std::search(std::next(first), mixed.end(), valid_bytes.begin(), valid_bytes.end());
   BOOST_REQUIRE(second != mixed.end());
   std::copy(conflicting_bytes.begin(), conflicting_bytes.end(), second);
   BOOST_CHECK_THROW((void)rendezvous::codec::decode_peer_record(mixed), exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_codec_roundtrips_v11_rpc_and_rejects_malformed) {
   const auto identity = make_test_identity();
   auto message = pubsub::message{
       .from = identity.peer,
       .data = std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 7},
       .subject = pubsub::topic{.value = "forge.topic"},
       .key = encode_public_key(identity.key),
   };
   pubsub::codec::sign_message(message, forge::crypto::pki::pem::read_private_key(identity.private_key_pem));
   BOOST_TEST(pubsub::codec::verify_message(message));

   const auto id = pubsub::codec::message_id(message);
   BOOST_TEST(!id.empty());

   const auto encoded =
       pubsub::codec::encode(
           pubsub::rpc{
               .subscriptions =
                   std::vector<pubsub::subscription>{
                       pubsub::subscription{.subscribe = true, .subject = pubsub::topic{.value = "forge.topic"}},
                       pubsub::subscription{.subscribe = false, .subject = pubsub::topic{.value = "forge.old"}},
                   },
               .messages = std::vector<pubsub::message>{message},
               .control_value =
                   pubsub::control{
                       .have = std::vector<pubsub::control::ihave>{pubsub::control::ihave{
                           .subject = pubsub::topic{.value = "forge.topic"},
                           .message_ids = std::vector<std::vector<std::uint8_t>>{id}}},
                       .want = std::vector<pubsub::control::iwant>{pubsub::control::iwant{
                           .message_ids = std::vector<std::vector<std::uint8_t>>{id}}},
                       .grafts = std::vector<pubsub::control::graft>{pubsub::control::graft{
                           .subject = pubsub::topic{.value = "forge.topic"}}},
                       .prunes = std::vector<pubsub::control::prune>{pubsub::control::prune{
                           .subject = pubsub::topic{.value = "forge.topic"},
                           .peers = std::vector<pubsub::peer_info>{pubsub::peer_info{.peer = identity.peer}},
                           .backoff = std::chrono::seconds{60},
                       }},
                   },
           });

   const auto decoded = pubsub::codec::decode(encoded, pubsub::options{});
   BOOST_REQUIRE_EQUAL(decoded.subscriptions.size(), 2U);
   BOOST_TEST(decoded.subscriptions.front().subscribe);
   BOOST_TEST(decoded.subscriptions.front().subject.value == "forge.topic");
   BOOST_REQUIRE_EQUAL(decoded.messages.size(), 1U);
   BOOST_TEST(decoded.messages.front().data == message.data, boost::test_tools::per_element());
   BOOST_TEST(pubsub::codec::verify_message(decoded.messages.front()));
   BOOST_REQUIRE(decoded.control_value.has_value());
   BOOST_REQUIRE_EQUAL(decoded.control_value->have.size(), 1U);
   BOOST_REQUIRE_EQUAL(decoded.control_value->want.size(), 1U);
   BOOST_REQUIRE_EQUAL(decoded.control_value->grafts.size(), 1U);
   BOOST_REQUIRE_EQUAL(decoded.control_value->prunes.size(), 1U);
   BOOST_TEST(decoded.control_value->prunes.front().backoff == std::chrono::seconds{60});

   BOOST_CHECK_THROW((void)pubsub::codec::decode(std::vector<std::uint8_t>{0xff, 0xff, 0xff, 0xff}, pubsub::options{}),
                     forge::exceptions::base);
   auto strict = pubsub::options{};
   strict.limits.max_rpc_size = 4;
   BOOST_CHECK_THROW((void)pubsub::codec::decode(encoded, strict), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_signing_rejects_tampered_payload) {
   const auto identity = make_test_identity();
   auto message = pubsub::message{
       .from = identity.peer,
       .data = std::vector<std::uint8_t>{'s', 'i', 'g', 'n', 'e', 'd'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 1},
       .subject = pubsub::topic{.value = "forge.signed"},
       .key = encode_public_key(identity.key),
   };
   pubsub::codec::sign_message(message, forge::crypto::pki::pem::read_private_key(identity.private_key_pem));
   BOOST_TEST(pubsub::codec::verify_message(message));

   message.data.push_back('!');
   BOOST_TEST(!pubsub::codec::verify_message(message));
}

BOOST_AUTO_TEST_CASE(p2p_signed_envelope_seals_and_verifies_domain_payload_and_signer) {
   const auto identity = make_test_identity();
   const auto payload_type = forge::multiformats::varint_encode(0x0302);
   const auto payload = std::vector<std::uint8_t>{1, 2, 3, 4, 5};

   const auto envelope =
       signed_envelope::seal(identity.key, forge::crypto::pki::pem::read_private_key(identity.private_key_pem),
                             "libp2p-relay-rsvp", payload_type, payload);
   const auto encoded = envelope.encode();
   const auto decoded = signed_envelope::decode(encoded);

   BOOST_TEST(decoded.payload_type == payload_type, boost::test_tools::per_element());
   BOOST_TEST(decoded.payload == payload, boost::test_tools::per_element());
   BOOST_TEST(decoded.signer().to_string() == identity.peer.to_string());
   BOOST_CHECK_NO_THROW(decoded.verify("libp2p-relay-rsvp", identity.peer));
   BOOST_CHECK_THROW(decoded.verify("wrong-domain", identity.peer), forge::exceptions::base);

   auto tampered = decoded;
   tampered.payload.back() ^= 0x01U;
   BOOST_CHECK_THROW(tampered.verify("libp2p-relay-rsvp", identity.peer), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_relay_voucher_uses_signed_envelope_and_rejects_stale_or_wrong_signer) {
   const auto relay_identity = make_test_identity();
   const auto other_identity = make_test_identity();
   const auto reservation = relay::voucher{
       .relay_peer = relay_identity.peer,
       .peer = peer(44),
       .expires_at = 4'102'444'800ULL,
   };

   const auto envelope = relay::codec::seal_reservation_voucher(
       reservation, relay_identity.key, forge::crypto::pki::pem::read_private_key(relay_identity.private_key_pem));
   const auto decoded = relay::codec::open_reservation_voucher(envelope, relay_identity.peer, 4'102'444'799ULL);

   BOOST_TEST(decoded.relay_peer.to_string() == reservation.relay_peer.to_string());
   BOOST_TEST(decoded.peer.to_string() == reservation.peer.to_string());
   BOOST_TEST(decoded.expires_at == reservation.expires_at);

   BOOST_CHECK_THROW(
       (void)relay::codec::open_reservation_voucher(envelope, relay_identity.peer, reservation.expires_at),
       forge::exceptions::base);
   BOOST_CHECK_THROW((void)relay::codec::open_reservation_voucher(envelope, other_identity.peer, 4'102'444'799ULL),
                     forge::exceptions::base);
}

[[nodiscard]] std::optional<resource_manager::stream_reservation> reserve_resource_stream_for_test(
    resource_manager& manager, const peer_id& owner,
    resource_manager::session_direction direction = resource_manager::session_direction::outbound,
    protocol_id protocol = builtins::echo, std::string service = "p2p.echo") {
   auto reservation = manager.reserve_stream(owner, direction);
   if (!reservation ||
       reservation->bind_protocol(protocol) != resource_manager::stream_reservation::bind_result::accepted ||
       reservation->bind_service(service) != resource_manager::stream_reservation::bind_result::accepted) {
      return std::nullopt;
   }
   return std::optional<resource_manager::stream_reservation>{std::move(*reservation)};
}

BOOST_AUTO_TEST_CASE(p2p_resource_manager_relay_stream_uses_normal_stream_and_memory_limits) {
   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_memory = 8, .max_streams = 2},
       .stream = {.max_memory = 8},
   }};
   auto relay_stream = reserve_resource_stream_for_test(manager, peer(90), resource_manager::session_direction::inbound,
                                                        builtins::relay_hop, "p2p.relay");
   BOOST_REQUIRE(relay_stream);
   auto queued = relay_stream->reserve_memory(8);
   BOOST_REQUIRE(queued);
   BOOST_TEST(!relay_stream->reserve_memory(1));

   auto regular_stream = reserve_resource_stream_for_test(manager, peer(91));
   BOOST_REQUIRE(regular_stream);
   BOOST_TEST(!manager.reserve_stream(peer(92), resource_manager::session_direction::outbound));

   auto snapshot = manager.current();
   BOOST_TEST(snapshot.streams.inbound_streams == 1U);
   BOOST_TEST(snapshot.streams.outbound_streams == 1U);
   BOOST_TEST(snapshot.streams.memory == 8U);
   BOOST_TEST(snapshot.denied_memory >= 1U);
   BOOST_TEST(snapshot.denied_streams >= 1U);

   regular_stream.reset();
   queued.reset();
   relay_stream.reset();
   snapshot = manager.current();
   BOOST_TEST(active_stream_count(snapshot) == 0U);
   BOOST_TEST(snapshot.streams.memory == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_stream_cancel_releases_reservation_when_transport_cancel_throws) {
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(93));
   BOOST_REQUIRE(reservation);
   auto backend = std::make_shared<queued_transport_stream>(503);
   backend->throw_on_cancel = true;
   auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));

   BOOST_TEST(active_stream_count(manager.current()) == 1U);
   BOOST_CHECK_THROW(resource->cancel(), std::runtime_error);
   BOOST_TEST(backend->cancel_calls == 1U);
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_stream_foreign_request_cancel_defers_reservation_release_to_owner) {
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(94));
   BOOST_REQUIRE(reservation);
   auto backend = std::make_shared<queued_transport_stream>(506);
   backend->throw_on_cancel = true;
   auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));

   auto requester = std::thread{[resource] { resource->request_cancel(); }};
   requester.join();
   BOOST_TEST(backend->cancel_calls == 1U);
   BOOST_TEST(active_stream_count(manager.current()) == 1U);
   resource.reset();
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
   BOOST_TEST(backend->cancel_calls == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_stream_close_forwards_cancel_without_releasing_owner_reservation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(95));
   BOOST_REQUIRE(reservation);
   auto close_entered = std::barrier{2};
   auto close_release = std::barrier{2};
   auto backend = std::make_shared<queued_transport_stream>(507);
   backend->close_entered = &close_entered;
   backend->close_release = &close_release;
   backend->throw_on_close = true;
   auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));

   auto closed = boost::asio::co_spawn(runtime.context(), resource->async_close(), boost::asio::use_future);
   close_entered.arrive_and_wait();
   auto requester = std::thread{[resource] { resource->request_cancel(); }};
   requester.join();

   BOOST_TEST(active_stream_count(manager.current()) == 1U);
   BOOST_TEST(backend->cancel_calls == 1U);
   close_release.arrive_and_wait();
   BOOST_CHECK_THROW(closed.get(), std::runtime_error);
   BOOST_TEST(backend->closed);
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
   BOOST_TEST(backend->close_calls == 1U);
   BOOST_TEST(backend->cancel_calls == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_owner_admission_commit_detaches_negotiation_cancellation) {
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(96));
   BOOST_REQUIRE(reservation);
   auto backend = std::make_shared<queued_transport_stream>(509);
   auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));
   auto signal = boost::asio::cancellation_signal{};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto admission =
       detail::make_owner_stream_admission(signal.slot(), stop, detail::owner_stream_lifetime::negotiation);

   admission(resource);
   BOOST_CHECK_NO_THROW(admission.commit());
   signal.emit(boost::asio::cancellation_type::all);

   BOOST_TEST(!backend->closed);
   BOOST_TEST(active_stream_count(manager.current()) == 1U);
   resource.reset();
   BOOST_TEST(backend->closed);
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_owner_admission_commit_rejects_sticky_stop_before_publication) {
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(97));
   BOOST_REQUIRE(reservation);
   auto backend = std::make_shared<queued_transport_stream>(510);
   auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));
   auto signal = boost::asio::cancellation_signal{};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto admission =
       detail::make_owner_stream_admission(signal.slot(), stop, detail::owner_stream_lifetime::negotiation);

   admission(resource);
   stop->request_stop();

   BOOST_CHECK_THROW(admission.commit(), forge::exceptions::base);
   BOOST_TEST(backend->closed);
   resource.reset();
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_owner_admission_commit_preserves_operation_cancellation) {
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(98));
   BOOST_REQUIRE(reservation);
   auto backend = std::make_shared<queued_transport_stream>(511);
   auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));
   auto signal = boost::asio::cancellation_signal{};
   auto stop = std::make_shared<detail::worker_stop_bridge>();
   auto admission = detail::make_owner_stream_admission(signal.slot(), stop);

   admission(resource);
   BOOST_CHECK_NO_THROW(admission.commit());
   stop->request_stop();
   signal.emit(boost::asio::cancellation_type::all);

   BOOST_TEST(backend->closed);
   resource.reset();
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_stream_close_preserves_error_and_releases_reservation_when_cancel_throws) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(99));
   BOOST_REQUIRE(reservation);
   auto backend = std::make_shared<queued_transport_stream>(504);
   backend->throw_on_close = true;
   backend->throw_on_cancel = true;
   auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));

   try {
      forge::asio::blocking::run(runtime, resource->async_close());
      BOOST_FAIL("expected injected stream close failure");
   } catch (const std::runtime_error& error) {
      BOOST_TEST(std::string_view{error.what()} == "injected stream close failure");
   }
   BOOST_TEST(backend->close_calls == 1U);
   BOOST_TEST(backend->cancel_calls == 1U);
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_stream_destructor_absorbs_cancel_failure_and_releases_reservation) {
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto backend = std::make_shared<queued_transport_stream>(505);
   backend->throw_on_cancel = true;
   {
      auto reservation = reserve_resource_stream_for_test(manager, peer(100));
      BOOST_REQUIRE(reservation);
      auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
      resource->attach(forge::net::transport::detail::stream_access::make(backend));
      BOOST_TEST(active_stream_count(manager.current()) == 1U);
   }

   BOOST_TEST(backend->cancel_calls == 1U);
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_stream_facade_abandonment_defers_to_dispatcher_graceful_close) {
   auto runtime = forge::asio::runtime{};
   auto manager = resource_manager{resource_manager::limits{.system = {.max_streams = 1}}};
   auto reservation = reserve_resource_stream_for_test(manager, peer(105));
   BOOST_REQUIRE(reservation);
   auto backend = std::make_shared<queued_transport_stream>(512);
   auto [facade, resource] = detail::prepare_resource_stream(std::move(*reservation));
   resource->attach(forge::net::transport::detail::stream_access::make(backend));

   facade = {};
   BOOST_TEST(backend->cancel_calls == 0U);
   BOOST_TEST(active_stream_count(manager.current()) == 1U);

   forge::asio::blocking::run(runtime, detail::async_close_unescaped(resource));
   BOOST_TEST(backend->close_calls == 1U);
   BOOST_TEST(backend->cancel_calls == 0U);
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_manager_enforces_peer_protocol_dial_and_reservation_scopes) {
   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_streams = 4},
       .peer = {.max_streams = 1},
       .protocol = {.max_streams = 1},
       .max_dial_attempts_per_peer = 1,
       .max_relay_reservations = 1,
       .max_malformed_messages_per_peer = 1,
   }};
   const auto owner = peer(101);
   const auto other = peer(102);

   auto scoped = manager.reserve_stream(owner, resource_manager::session_direction::outbound);
   BOOST_REQUIRE(scoped);
   BOOST_REQUIRE(bind_matches(scoped->bind_protocol(builtins::relay_hop),
                              resource_manager::stream_reservation::bind_result::accepted));
   BOOST_REQUIRE(bind_matches(scoped->bind_service("p2p.relay"),
                              resource_manager::stream_reservation::bind_result::accepted));
   BOOST_TEST(!manager.reserve_stream(owner, resource_manager::session_direction::outbound));

   auto other_stream = manager.reserve_stream(other, resource_manager::session_direction::outbound);
   BOOST_REQUIRE(other_stream);
   BOOST_TEST(bind_matches(other_stream->bind_protocol(builtins::relay_hop),
                           resource_manager::stream_reservation::bind_result::policy_rejected));
   BOOST_REQUIRE(bind_matches(other_stream->bind_protocol(builtins::ping),
                              resource_manager::stream_reservation::bind_result::accepted));
   BOOST_REQUIRE(bind_matches(other_stream->bind_service("p2p.ping"),
                              resource_manager::stream_reservation::bind_result::accepted));
   other_stream.reset();
   scoped.reset();
   const auto scoped_snapshot = manager.current();
   BOOST_TEST(active_stream_count(scoped_snapshot) == 0U);
   BOOST_TEST(scoped_snapshot.active_peer_scopes == 0U);
   BOOST_TEST(scoped_snapshot.active_protocol_scopes == 0U);

   auto saturated = resource_manager{resource_manager::limits{
       .system = {.max_streams = 1},
   }};
   auto saturated_stream = saturated.reserve_stream(owner, resource_manager::session_direction::outbound);
   BOOST_REQUIRE(saturated_stream);
   BOOST_TEST(!saturated.reserve_stream(other, resource_manager::session_direction::outbound));
   const auto saturated_snapshot = saturated.current();
   BOOST_TEST(active_stream_count(saturated_snapshot) == 1U);
   BOOST_TEST(saturated_snapshot.active_peer_scopes == 1U);

   auto relay = manager.reserve_relay(owner);
   BOOST_REQUIRE(relay);
   BOOST_TEST(!manager.reserve_relay(other));
   relay.reset();

   auto dial = manager.reserve_dial();
   BOOST_REQUIRE(dial);
   BOOST_TEST(transition_matches(dial->bind(owner), resource_manager::transition_result::accepted));
   BOOST_TEST(!manager.reserve_dial(owner));
   BOOST_TEST(manager.current().active_dials == 1U);
   dial.reset();
   BOOST_TEST(manager.current().active_dials == 0U);
   BOOST_TEST(transition_matches(manager.record_malformed(owner), resource_manager::transition_result::accepted));
   BOOST_TEST(
       transition_matches(manager.record_malformed(owner), resource_manager::transition_result::policy_rejected));

   const auto snapshot = manager.current();
   BOOST_TEST(snapshot.denied_streams >= 1U);
   BOOST_TEST(snapshot.denied_scope_migrations >= 1U);
   BOOST_TEST(snapshot.denied_dials >= 1U);
   BOOST_TEST(snapshot.denied_relays >= 1U);
   BOOST_TEST(snapshot.denied_malformed >= 1U);
}

BOOST_AUTO_TEST_CASE(p2p_resource_manager_enforces_connection_session_scopes) {
   auto manager = resource_manager{resource_manager::limits{
       .system = {.max_inbound_connections = 2, .max_outbound_connections = 1, .max_connections = 2},
       .transient = {.max_inbound_connections = 1, .max_outbound_connections = 1, .max_connections = 2},
       .peer = {.max_inbound_connections = 1, .max_outbound_connections = 1, .max_connections = 1},
       .max_dial_attempts_per_peer = 1,
   }};
   const auto inbound = resource_manager::session_scope{
       .peer = peer(103),
       .direction = resource_manager::session_direction::inbound,
   };
   const auto outbound = resource_manager::session_scope{
       .peer = peer(104),
       .direction = resource_manager::session_direction::outbound,
   };

   auto pending_inbound = manager.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(pending_inbound);
   BOOST_TEST(!manager.reserve_session(resource_manager::session_direction::inbound));
   pending_inbound.reset();
   auto pending_outbound = manager.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(pending_outbound);
   BOOST_TEST(!manager.reserve_session(resource_manager::session_direction::outbound));
   pending_outbound.reset();

   auto logical_dial = manager.reserve_dial(outbound.peer);
   BOOST_REQUIRE(logical_dial);
   BOOST_TEST(!manager.reserve_dial(outbound.peer));
   BOOST_TEST(manager.current().active_dials == 1U);
   BOOST_TEST(manager.current().dial_attempt_scopes == 1U);
   logical_dial.reset();
   BOOST_TEST(manager.current().active_dials == 0U);
   BOOST_TEST(manager.current().dial_attempt_scopes == 0U);

   auto active_inbound = manager.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(active_inbound);
   BOOST_TEST(transition_matches(active_inbound->establish(inbound),
                                 resource_manager::transition_result::accepted));
   auto duplicate = manager.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(duplicate);
   BOOST_TEST(transition_matches(duplicate->establish(inbound),
                                 resource_manager::transition_result::policy_rejected));
   duplicate.reset();
   auto active_outbound = manager.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(active_outbound);
   BOOST_TEST(transition_matches(active_outbound->establish(outbound),
                                 resource_manager::transition_result::accepted));
   active_inbound.reset();
   active_outbound.reset();

   const auto snapshot = manager.current();
   BOOST_TEST(snapshot.system.inbound_connections == 0U);
   BOOST_TEST(snapshot.system.outbound_connections == 0U);
   BOOST_TEST(snapshot.transient.inbound_connections == 0U);
   BOOST_TEST(snapshot.transient.outbound_connections == 0U);
   BOOST_TEST(snapshot.denied_connections >= 2U);
   BOOST_TEST(snapshot.denied_scope_migrations >= 1U);

   auto saturated = resource_manager{resource_manager::limits{
       .system = {.max_inbound_connections = 1, .max_outbound_connections = 0, .max_connections = 1},
       .peer = {.max_inbound_connections = 1, .max_connections = 1},
   }};
   auto saturated_session = saturated.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(saturated_session);
   BOOST_TEST(transition_matches(
       saturated_session->establish(
           resource_manager::session_scope{.peer = peer(105),
                                           .direction = resource_manager::session_direction::inbound}),
       resource_manager::transition_result::accepted));
   auto rejected_session = saturated.reserve_session(resource_manager::session_direction::inbound);
   BOOST_TEST(!rejected_session);
   auto saturated_snapshot = saturated.current();
   BOOST_TEST(saturated_snapshot.peers.inbound_connections == 1U);
   saturated_session.reset();
   saturated_snapshot = saturated.current();
   BOOST_TEST(saturated_snapshot.peers.inbound_connections == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_owner_cancellation_reaches_published_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto entered = std::make_shared<forge::asio::notification>();
   auto backend = std::make_shared<stalling_transport_stream>(508, entered);
   auto stream = std::make_shared<forge::net::p2p::stream>(forge::net::transport::detail::stream_access::make(backend));
   auto signal = boost::asio::cancellation_signal{};
   const auto observed = entered->epoch();

   auto pending = boost::asio::co_spawn(
       runtime.context(),
       [stream, slot = signal.slot()]() mutable -> boost::asio::awaitable<void> {
          auto cancellation = detail::owner_stream_cancellation{std::move(slot), stream};
          static_cast<void>(co_await stream->async_read());
       },
       boost::asio::use_future);
   static_cast<void>(forge::asio::blocking::run(runtime, entered->async_wait(observed)));

   signal.emit(boost::asio::cancellation_type::all);
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   try {
      pending.get();
      BOOST_FAIL("expected published P2P stream cancellation");
   } catch (const boost::system::system_error& error) {
      BOOST_TEST(error.code() == boost::asio::error::operation_aborted);
   }
   BOOST_TEST(backend->closed);
}

BOOST_AUTO_TEST_CASE(p2p_relay_byte_limits_are_independent_per_circuit_direction) {
   auto left_to_right = detail::relay_budget{4};
   auto right_to_left = detail::relay_budget{4};

   BOOST_TEST(left_to_right.consume(4));
   BOOST_TEST(right_to_left.consume(4));
   BOOST_TEST(left_to_right.used() == 4U);
   BOOST_TEST(right_to_left.used() == 4U);
   BOOST_TEST(left_to_right.exhausted());
   BOOST_TEST(right_to_left.exhausted());
   BOOST_TEST(!left_to_right.consume(1));
   BOOST_TEST(!right_to_left.consume(1));

   auto next_circuit = detail::relay_budget{4};
   BOOST_TEST(next_circuit.consume(4));
}

[[nodiscard]] std::optional<resource_manager::relay_reservation>
reserve_relay_circuit_for_test(resource_manager& manager, const peer_id& owner) {
   auto reservation = manager.reserve_relay(owner);
   return reservation ? std::optional<resource_manager::relay_reservation>{std::move(*reservation)} : std::nullopt;
}

[[nodiscard]] std::optional<resource_manager::stream_reservation>
reserve_relay_hop_stream_for_test(resource_manager& manager, const peer_id& owner) {
   auto reservation = manager.reserve_stream(owner, resource_manager::session_direction::inbound);
   if (!reservation ||
       reservation->bind_protocol(builtins::relay_hop) != resource_manager::stream_reservation::bind_result::accepted ||
       reservation->bind_service("p2p.relay") != resource_manager::stream_reservation::bind_result::accepted) {
      return std::nullopt;
   }
   return std::optional<resource_manager::stream_reservation>{std::move(*reservation)};
}

BOOST_AUTO_TEST_CASE(p2p_relay_deadline_cancel_before_wait_is_latched) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto manager = resource_manager{};
   auto reservation = reserve_relay_circuit_for_test(manager, peer(50));
   BOOST_REQUIRE(reservation);
   auto left_backend = std::make_shared<queued_transport_stream>(501);
   auto right_backend = std::make_shared<queued_transport_stream>(502);
   auto pair = std::make_shared<detail::relay_pair>(
       peer(50), forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(left_backend)},
       forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(right_backend)},
       std::move(*reservation), runtime.context().get_executor(), std::chrono::seconds{60}, 1024);

   BOOST_TEST(!pair->mark_finished());
   BOOST_TEST(pair->mark_finished());
   auto deadline = boost::asio::co_spawn(runtime.context(), pair->async_wait_deadline(), boost::asio::use_future);
   BOOST_REQUIRE(deadline.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_TEST(!deadline.get());
   BOOST_TEST(active_stream_count(manager.current()) == 0U);
   BOOST_TEST(manager.current().active_relay_reservations == 1U);
   pair.reset();
   BOOST_TEST(manager.current().active_relay_reservations == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_relay_hop_timeout_cancels_only_the_owned_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto operation_backend = std::make_shared<stalling_transport_stream>(503);
   auto operation_stream =
       std::make_shared<forge::net::p2p::stream>(forge::net::transport::detail::stream_access::make(operation_backend));
   auto sibling_backend = std::make_shared<queued_transport_stream>(504);
   auto sibling = forge::net::p2p::stream{forge::net::transport::detail::stream_access::make(sibling_backend)};

   try {
      (void)forge::asio::blocking::run(
          runtime,
          detail::async_exchange_relay_hop(
              runtime.context(), std::chrono::milliseconds{20}, "test relay HOP",
              [operation_stream](detail::stream_admission_handler) -> boost::asio::awaitable<forge::net::p2p::stream> {
                 co_return std::move(*operation_stream);
              },
              relay::hop_message{.kind = relay::hop_message::message_kind::reserve}, 4 * 1024));
      BOOST_FAIL("expected relay HOP timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(exceptions::code_of(error).value()) == static_cast<int>(exceptions::code::timeout));
   }

   BOOST_TEST(operation_backend->closed);
   BOOST_TEST(!sibling_backend->closed);
   sibling_backend->reads.push_back(std::vector<std::uint8_t>{'o', 'k'});
   const auto reply = forge::asio::blocking::run(runtime, sibling.async_read());
   BOOST_TEST(reply == std::vector<std::uint8_t>({'o', 'k'}), boost::test_tools::per_element());
   forge::asio::blocking::run(runtime, sibling.async_write(std::vector<std::uint8_t>{'u', 'p'}));
   BOOST_REQUIRE_EQUAL(sibling_backend->writes.size(), 1U);
   BOOST_TEST(sibling_backend->writes.front() == std::vector<std::uint8_t>({'u', 'p'}),
              boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(p2p_relay_hop_timeout_cancels_before_stream_publication) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto manager = resource_manager{};
   auto backend = std::make_shared<stalling_transport_stream>(507);
   auto open_started = std::make_shared<std::atomic_bool>(false);
   const auto started = std::chrono::steady_clock::now();

   try {
      (void)forge::asio::blocking::run(
          runtime,
          detail::async_exchange_relay_hop(
              runtime.context(), std::chrono::milliseconds{20}, "test relay HOP pre-open",
              [manager, backend, open_started](detail::stream_admission_handler admitted) mutable
                  -> boost::asio::awaitable<forge::net::p2p::stream> {
                 open_started->store(true, std::memory_order_release);
                 co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
                 auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
                 timer.expires_after(std::chrono::milliseconds{50});
                 co_await timer.async_wait(boost::asio::use_awaitable);
                 auto reservation = reserve_relay_hop_stream_for_test(manager, peer(50));
                 BOOST_REQUIRE(reservation);
                 auto [guarded, resource] = detail::prepare_resource_stream(std::move(*reservation));
                 resource->attach(forge::net::transport::detail::stream_access::make(backend));
                 admitted(resource);
                 admitted.commit();
                 co_return forge::net::p2p::stream{std::move(guarded)};
              },
              relay::hop_message{.kind = relay::hop_message::message_kind::reserve}, 4 * 1024));
      BOOST_FAIL("expected pre-open relay HOP timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(exceptions::code_of(error).value()) == static_cast<int>(exceptions::code::timeout));
   }

   BOOST_TEST(open_started->load(std::memory_order_acquire));
   BOOST_TEST(backend->closed);
   BOOST_TEST(manager.current().streams.inbound_streams == 0U);
   BOOST_TEST(
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() <
       1000);
}

BOOST_AUTO_TEST_CASE(p2p_relay_hop_timeout_cancels_admitted_stream_during_negotiation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto manager = resource_manager{};
   auto backend = std::make_shared<stalling_transport_stream>(506);

   try {
      (void)forge::asio::blocking::run(
          runtime, detail::async_exchange_relay_hop(
                       runtime.context(), std::chrono::milliseconds{20}, "test relay HOP negotiation",
                       [manager, backend](detail::stream_admission_handler admitted) mutable
                           -> boost::asio::awaitable<forge::net::p2p::stream> {
                          auto reservation = reserve_relay_hop_stream_for_test(manager, peer(50));
                          BOOST_REQUIRE(reservation);
                          auto [guarded, resource] = detail::prepare_resource_stream(std::move(*reservation));
                          resource->attach(forge::net::transport::detail::stream_access::make(backend));
                          admitted(resource);
                          (void)co_await guarded.async_read();
                          admitted.commit();
                          co_return forge::net::p2p::stream{std::move(guarded)};
                       },
                       relay::hop_message{.kind = relay::hop_message::message_kind::reserve}, 4 * 1024));
      BOOST_FAIL("expected relay HOP negotiation timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(exceptions::code_of(error).value()) == static_cast<int>(exceptions::code::timeout));
   }

   BOOST_TEST(backend->closed);
   BOOST_TEST(manager.current().streams.inbound_streams == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_relay_hop_inherits_caller_cancellation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto entered = std::make_shared<forge::asio::notification>();
   const auto observed = entered->epoch();
   auto operation_backend = std::make_shared<stalling_transport_stream>(505, entered);
   auto operation_stream =
       std::make_shared<forge::net::p2p::stream>(forge::net::transport::detail::stream_access::make(operation_backend));
   auto cancellation = boost::asio::cancellation_signal{};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::async_exchange_relay_hop(
           runtime.context(), std::chrono::seconds{30}, "test relay HOP caller cancellation",
           [operation_stream](detail::stream_admission_handler) -> boost::asio::awaitable<forge::net::p2p::stream> {
              co_return std::move(*operation_stream);
           },
           relay::hop_message{.kind = relay::hop_message::message_kind::reserve}, 4 * 1024),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));

   (void)forge::asio::blocking::run(runtime, entered->async_wait(observed));
   cancellation.emit(boost::asio::cancellation_type::terminal);
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   try {
      (void)pending.get();
      BOOST_FAIL("expected relay HOP caller cancellation");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(exceptions::code_of(error).value()) == static_cast<int>(exceptions::code::canceled));
   }
   BOOST_TEST(operation_backend->closed);
}

BOOST_AUTO_TEST_CASE(p2p_relay_hop_caller_cancellation_is_sticky_until_stream_admission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto manager = resource_manager{};
   auto backend = std::make_shared<stalling_transport_stream>(508);
   auto entered = std::make_shared<forge::asio::notification>();
   const auto observed = entered->epoch();
   auto cancellation = boost::asio::cancellation_signal{};
   auto pending = boost::asio::co_spawn(
       runtime.context(),
       detail::async_exchange_relay_hop(
           runtime.context(), std::chrono::seconds{30}, "test relay HOP late caller cancellation",
           [manager, backend, entered](
               detail::stream_admission_handler admitted) mutable -> boost::asio::awaitable<forge::net::p2p::stream> {
              entered->notify();
              co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
              auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
              timer.expires_after(std::chrono::milliseconds{50});
              co_await timer.async_wait(boost::asio::use_awaitable);
              auto reservation = reserve_relay_hop_stream_for_test(manager, peer(50));
              BOOST_REQUIRE(reservation);
              auto [guarded, resource] = detail::prepare_resource_stream(std::move(*reservation));
              resource->attach(forge::net::transport::detail::stream_access::make(backend));
              admitted(resource);
              admitted.commit();
              co_return forge::net::p2p::stream{std::move(guarded)};
           },
           relay::hop_message{.kind = relay::hop_message::message_kind::reserve}, 4 * 1024),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));

   (void)forge::asio::blocking::run(runtime, entered->async_wait(observed));
   cancellation.emit(boost::asio::cancellation_type::terminal);
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   try {
      static_cast<void>(pending.get());
      BOOST_FAIL("expected relay HOP caller cancellation before stream admission");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(exceptions::code_of(error).value()) == static_cast<int>(exceptions::code::canceled));
   }
   BOOST_TEST(backend->closed);
   BOOST_TEST(manager.current().streams.inbound_streams == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_node_peer_protection_api_is_tagged_and_additive) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto value = node{runtime, options_for(peer(233))};
   const auto protected_peer = peer(234);

   BOOST_TEST(!value.is_peer_protected(protected_peer));
   value.protect_peer(protected_peer, "bootstrap");
   BOOST_TEST(value.is_peer_protected(protected_peer));
   BOOST_TEST(value.unprotect_peer(protected_peer, "other"));
   BOOST_TEST(value.is_peer_protected(protected_peer));
   BOOST_TEST(!value.unprotect_peer(protected_peer, "bootstrap"));
   BOOST_TEST(!value.is_peer_protected(protected_peer));
   value.tag_peer(protected_peer, "topology", 10);
   BOOST_TEST(value.untag_peer(protected_peer, "topology"));
   BOOST_TEST(!value.untag_peer(protected_peer, "topology"));

   forge::asio::blocking::run(runtime, value.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_snapshot_defaults_persistence_state) {
   auto value = diagnostics::snapshot{};

   BOOST_TEST(!value.persistence.degraded);
   BOOST_TEST(value.persistence.pending_peer_mutations == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_black_holes_reflect_effective_public_and_private_policy) {
   static_assert(boost::describe::has_describe_members<dialing::black_hole_counter_status>::value);
   static_assert(boost::describe::has_describe_members<dialing::black_hole_status>::value);
   static_assert(boost::describe::has_describe_enumerators<dialing::black_hole_state>::value);
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto public_node = node{runtime, options_for(make_test_identity())};
   const auto status = public_node.diagnostics().black_holes;
   BOOST_TEST(status.udp.enabled);
   BOOST_TEST(status.ipv6.enabled);
   BOOST_CHECK(status.udp.state == dialing::black_hole_state::probing);
   BOOST_CHECK(status.ipv6.state == dialing::black_hole_state::probing);
   BOOST_TEST(status.udp.peer_requests == 0U);
   BOOST_TEST(status.ipv6.peer_requests == 0U);
   BOOST_TEST(status.udp.outcomes == 0U);
   BOOST_TEST(status.ipv6.outcomes == 0U);
   for (const auto ipv6_enabled : {false, true}) {
      auto options = private_network_options_for(make_test_identity());
      options.direct_dial.black_holes.ipv6_enabled = ipv6_enabled;
      auto private_node = node{runtime, std::move(options)};
      const auto private_status = private_node.diagnostics().black_holes;
      BOOST_TEST(!private_status.udp.enabled);
      BOOST_TEST(private_status.ipv6.enabled == ipv6_enabled);
      forge::asio::blocking::run(runtime, private_node.async_stop());
   }
   forge::asio::blocking::run(runtime, public_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_black_holes_report_neutral_gated_requests_without_mutation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = options_for(make_test_identity());
   options.connection_gater = std::make_shared<recording_connection_gater>(recorded_gater_stage::address_dial);
   auto client = node{runtime, std::move(options)};
   const auto target = parse_endpoint("/ip6/2400::1/udp/4001/quic-v1/p2p/" + peer(214).to_string());
   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(runtime, client.async_connect(target, node::connect_options{.allow_relay = false})),
       forge::exceptions::base, [](const auto& error) {
          return exceptions::code_of(error) == exceptions::code::connection_rejected;
       });
   for (auto read = 0; read < 2; ++read) {
      const auto status = client.diagnostics().black_holes;
      BOOST_TEST(status.udp.peer_requests == 1U);
      BOOST_TEST(status.ipv6.peer_requests == 1U);
      BOOST_TEST(status.udp.outcomes == 0U);
      BOOST_TEST(status.ipv6.outcomes == 0U);
      BOOST_TEST(status.udp.successes == 0U);
      BOOST_TEST(status.ipv6.successes == 0U);
      BOOST_TEST(status.udp.next_probe_after == 0U);
      BOOST_TEST(status.ipv6.next_probe_after == 0U);
      BOOST_CHECK(status.udp.state == dialing::black_hole_state::probing);
      BOOST_CHECK(status.ipv6.state == dialing::black_hole_state::probing);
   }
   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_snapshot_reports_live_network_state_without_mutation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = node{
       runtime, options_for(peer(250), capability_set{.bits = capabilities::direct_quic | capabilities::peer_exchange |
                                                              capabilities::pubsub})};
   auto client_options =
       options_for(peer(251), capability_set{.bits = capabilities::direct_quic | capabilities::peer_exchange |
                                                     capabilities::pubsub | capabilities::relay_reservation});
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto client = node{runtime, std::move(client_options)};
   register_echo(server);

   const auto server_endpoint = listen(server, runtime);
   (void)listen(client, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::peer_exchange |
                                                        capabilities::pubsub | capabilities::relay |
                                                        capabilities::relay_reservation});
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   client.protect_peer(server.local_peer(), "diagnostics-test");
   client.peers().mark_endpoint_failure(server.local_peer(), server_endpoint, path::kind::direct,
                                        std::chrono::system_clock::now() + std::chrono::seconds{30});

   const auto before = client.metrics();
   const auto before_pubsub = client.pubsub_snapshot();
   const auto snapshot = client.diagnostics(diagnostics::options{});
   const auto after = client.metrics();

   BOOST_TEST(snapshot.network.local_peer.to_string() == client.local_peer().to_string());
   BOOST_TEST(!snapshot.network.local_endpoints.empty());
   BOOST_TEST(snapshot.metrics.active_sessions == before.active_sessions);
   BOOST_TEST(snapshot.metrics.active_sessions == after.active_sessions);
   BOOST_TEST(snapshot.resources.system.outbound_connections >= 1U);
   BOOST_TEST(snapshot.pubsub.topics == before_pubsub.topics);
   BOOST_REQUIRE_EQUAL(snapshot.peers.size(), 1U);
   BOOST_TEST(snapshot.peers.front().peer.to_string() == server.local_peer().to_string());
   BOOST_TEST(snapshot.peers.front().protected_peer);
   BOOST_REQUIRE(!snapshot.peers.front().endpoints.empty());
   BOOST_TEST(snapshot.peers.front().endpoints.front().failures >= 1U);
   BOOST_REQUIRE_EQUAL(snapshot.sessions.size(), 1U);
   BOOST_TEST(snapshot.sessions.front().remote_peer.to_string() == server.local_peer().to_string());
   BOOST_TEST(snapshot.sessions.front().protected_peer);
   BOOST_TEST(static_cast<int>(snapshot.sessions.front().direction) ==
              static_cast<int>(diagnostics::session_direction::outbound));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_snapshot_caps_are_deterministic) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto value = node{runtime, options_for(peer(252))};
   value.peers().learn_endpoint(peer(253), make_quic_endpoint(4001), capability_set{.bits = capabilities::direct_quic});
   value.peers().learn_endpoint(peer(254), make_tcp_endpoint(4002),
                                capability_set{.bits = capabilities::peer_exchange});

   const auto snapshot = value.diagnostics(diagnostics::options{
       .max_peers = 1,
       .max_sessions = 0,
       .max_endpoints_per_peer = 1,
       .max_protocols_per_peer = 1,
       .max_relay_reservations_per_peer = 1,
   });

   BOOST_REQUIRE_EQUAL(snapshot.peers.size(), 1U);
   BOOST_TEST(snapshot.sessions.empty());
   BOOST_REQUIRE_EQUAL(snapshot.peers.front().endpoints.size(), 1U);

   forge::asio::blocking::run(runtime, value.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_prunes_unprotected_sessions_and_keeps_protected_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(235));
   set_resource_session_limits(client_options, 2, (std::numeric_limits<std::size_t>::max)(), 2,
                               (std::numeric_limits<std::size_t>::max)(), session_admission_headroom::one_session);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};

   auto first = node{runtime, options_for(peer(236))};
   auto second = node{runtime, options_for(peer(237))};
   auto third = node{runtime, options_for(peer(238))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(first);
   register_echo(second);
   register_echo(third);

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   const auto third_endpoint = listen(third, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   client.protect_peer(first.local_peer(), "bootstrap");
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(third_endpoint, node::connect_options{.expected_peer = third.local_peer()}));

   auto metrics = client.metrics();
   BOOST_TEST(metrics.active_sessions == 2U);
   BOOST_TEST(metrics.sessions_pruned >= 1U);
   const auto diagnostics = client.diagnostics();
   BOOST_TEST(diagnostics.connections.retained_identify_attempts <= diagnostics.connections.active_sessions);
   BOOST_TEST(client.is_peer_protected(first.local_peer()));

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(first.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'p', 'r', 'o', 't', 'e', 'c', 't'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, third.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_prunes_batch_to_low_watermark) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   auto client_options = options_for(peer(60));
   set_resource_session_limits(client_options, 4, (std::numeric_limits<std::size_t>::max)(), 4,
                               (std::numeric_limits<std::size_t>::max)(), session_admission_headroom::one_session);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{60'000};

   auto first = node{runtime, options_for(peer(61))};
   auto second = node{runtime, options_for(peer(62))};
   auto third = node{runtime, options_for(peer(63))};
   auto fourth = node{runtime, options_for(peer(64))};
   auto fifth = node{runtime, options_for(peer(65))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(fifth);

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   const auto third_endpoint = listen(third, runtime);
   const auto fourth_endpoint = listen(fourth, runtime);
   const auto fifth_endpoint = listen(fifth, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(third_endpoint, node::connect_options{.expected_peer = third.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(fourth_endpoint, node::connect_options{.expected_peer = fourth.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(fifth_endpoint, node::connect_options{.expected_peer = fifth.local_peer()}));

   auto metrics = client.metrics();
   BOOST_TEST(metrics.active_sessions == 2U);
   BOOST_TEST(metrics.sessions_pruned >= 3U);
   const auto diagnostics = client.diagnostics();
   BOOST_TEST(diagnostics.connections.retained_identify_attempts <= diagnostics.connections.active_sessions);

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(fifth.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'b', 'a', 't', 'c', 'h'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, fifth.async_stop());
   forge::asio::blocking::run(runtime, fourth.async_stop());
   forge::asio::blocking::run(runtime, third.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_rejects_when_all_sessions_are_protected) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(239));
   set_resource_session_limits(client_options, 2, (std::numeric_limits<std::size_t>::max)(), 2,
                               (std::numeric_limits<std::size_t>::max)(), session_admission_headroom::one_session);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};

   auto first = node{runtime, options_for(peer(240))};
   auto second = node{runtime, options_for(peer(241))};
   auto third = node{runtime, options_for(peer(242))};
   auto client = node{runtime, std::move(client_options)};

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   const auto third_endpoint = listen(third, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   client.protect_peer(first.local_peer(), "bootstrap");
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()}));
   client.protect_peer(second.local_peer(), "bootstrap");

   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_connect(third_endpoint, node::connect_options{.expected_peer = third.local_peer()}));
      BOOST_FAIL("expected all-protected session limit rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }
   const auto metrics = client.metrics();
   BOOST_TEST(metrics.active_sessions == 2U);
   BOOST_TEST(metrics.connection_rejections >= 1U);
   BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 2U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, third.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_rejects_direction_limit_before_pruning_unrelated_sessions) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(254));
   set_resource_session_limits(client_options, 2, 4, 1, 4);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};

   auto first = node{runtime, options_for(peer(255))};
   auto second = node{runtime, options_for(peer(1))};
   auto third = node{runtime, options_for(peer(2))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(first);
   register_echo(second);
   register_echo(client);

   const auto first_endpoint = listen(first, runtime);
   (void)listen(second, runtime);
   const auto client_endpoint = listen(client, runtime);
   const auto third_endpoint = listen(third, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   client.protect_peer(first.local_peer(), "bootstrap");
   (void)forge::asio::blocking::run(
       runtime, second.async_connect(client_endpoint, node::connect_options{.expected_peer = client.local_peer()}));
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "inbound session admission");
   const auto before = client.metrics();
   BOOST_TEST(before.active_sessions == 2U);

   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_connect(third_endpoint, node::connect_options{.expected_peer = third.local_peer()}));
      BOOST_FAIL("expected outbound session limit rejection");
   } catch (const forge::exceptions::base&) {
   }

   const auto after = client.metrics();
   BOOST_TEST(after.active_sessions == 2U);
   BOOST_TEST(after.sessions_pruned == before.sessions_pruned);
   BOOST_TEST(after.connection_rejections >= before.connection_rejections + 1U);

   auto stream =
       forge::asio::blocking::run(runtime, second.async_open_protocol_stream(client.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'s', 't', 'a', 'y'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, third.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_resource_manager_rejects_direction_saturation_before_global_soft_cap) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(3));
   set_resource_session_limits(client_options, 4, 4, 1, 4);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};

   auto first = node{runtime, options_for(peer(4))};
   auto second = node{runtime, options_for(peer(5))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(first);
   register_echo(second);

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   const auto before = client.metrics();
   BOOST_TEST(before.active_sessions == 1U);

   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()})),
       forge::exceptions::base);

   const auto after = client.metrics();
   BOOST_TEST(after.active_sessions == 1U);
   BOOST_TEST(after.sessions_pruned == before.sessions_pruned);
   BOOST_TEST(after.connection_rejections >= before.connection_rejections + 1U);

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(first.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'d', 'i', 'r'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_resource_manager_direction_cap_is_independent_of_soft_low_watermark) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(6));
   set_resource_session_limits(client_options, 100, 100, 1, 4);
   client_options.limits.session_low_watermark = 50;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};

   auto first = node{runtime, options_for(peer(7))};
   auto second = node{runtime, options_for(peer(8))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(first);
   register_echo(second);

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   const auto before = client.metrics();
   BOOST_TEST(before.active_sessions == 1U);

   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()})),
       forge::exceptions::base);

   const auto after = client.metrics();
   BOOST_TEST(after.active_sessions == 1U);
   BOOST_TEST(after.sessions_pruned == before.sessions_pruned);
   BOOST_TEST(after.connection_rejections >= before.connection_rejections + 1U);

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(first.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'l', 'o', 'w'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_low_active_limit_is_independent_of_topology_watermarks) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(243));
   set_resource_session_limits(client_options, 1, (std::numeric_limits<std::size_t>::max)(), 1,
                               (std::numeric_limits<std::size_t>::max)(), session_admission_headroom::one_session);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};
   BOOST_TEST(static_cast<int>(client_options.limits.topology.operating_mode) ==
              static_cast<int>(topology::mode::managed));
   BOOST_TEST(client_options.limits.topology.peers.high > client_options.limits.max_sessions);
   auto first = node{runtime, options_for(peer(244))};
   auto second = node{runtime, options_for(peer(245))};
   auto client = node{runtime, std::move(client_options)};
   const auto effective_topology = client.diagnostics().topology;
   BOOST_TEST(effective_topology.low_watermark == 1U);
   BOOST_TEST(effective_topology.target_watermark == 1U);
   BOOST_TEST(effective_topology.high_watermark > effective_topology.target_watermark);
   register_echo(first);

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   client.protect_peer(first.local_peer(), "bootstrap");
   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()}));
      BOOST_FAIL("expected outbound session limit rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(first.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'l', 'i', 'm', 'i', 't'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(client.metrics().connection_rejections >= 1U);
   BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_replaces_an_eligible_single_session) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(246));
   set_resource_session_limits(client_options, 1, (std::numeric_limits<std::size_t>::max)(), 1,
                               (std::numeric_limits<std::size_t>::max)(), session_admission_headroom::one_session);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};

   auto first = node{runtime, options_for(peer(247))};
   auto second = node{runtime, options_for(peer(248))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(second);

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()}));

   const auto metrics = client.metrics();
   BOOST_TEST(metrics.active_sessions == 1U);
   BOOST_TEST(metrics.sessions_pruned >= 1U);
   BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 1U);

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(second.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'o', 'n', 'e'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_single_session_honors_grace_and_protection) {
   const auto first_peer = peer(249);
   const auto second_peer = peer(250);
   const auto started = std::chrono::steady_clock::time_point{};
   auto manager = connection_manager{connection_manager::policy{
       .max_sessions = 1,
       .low_watermark = 1,
       .grace_period = std::chrono::milliseconds{10},
       .prune_silence = std::chrono::milliseconds{0},
   }};

   BOOST_REQUIRE(manager
                     .remember(
                         connection_manager::session_record{
                             .id = 1,
                             .peer = first_peer,
                             .opened_at = started,
                             .last_used_at = started,
                         },
                         started)
                     .accepted);

   const auto during_grace = manager.remember(
       connection_manager::session_record{
           .id = 2,
           .peer = second_peer,
           .opened_at = started + std::chrono::milliseconds{1},
           .last_used_at = started + std::chrono::milliseconds{1},
       },
       started + std::chrono::milliseconds{1});
   BOOST_TEST(!during_grace.accepted);
   BOOST_TEST(during_grace.pruned.empty());
   BOOST_TEST(manager.size() == 1U);

   const auto eligible_at = started + std::chrono::milliseconds{11};
   manager.protect(first_peer, "bootstrap");
   const auto while_protected = manager.remember(
       connection_manager::session_record{
           .id = 2,
           .peer = second_peer,
           .opened_at = eligible_at,
           .last_used_at = eligible_at,
       },
       eligible_at);
   BOOST_TEST(!while_protected.accepted);
   BOOST_TEST(while_protected.pruned.empty());
   BOOST_TEST(manager.size() == 1U);

   static_cast<void>(manager.unprotect(first_peer, "bootstrap"));
   const auto replacement = manager.remember(
       connection_manager::session_record{
           .id = 2,
           .peer = second_peer,
           .opened_at = eligible_at,
           .last_used_at = eligible_at,
       },
       eligible_at);
   BOOST_TEST(replacement.accepted);
   BOOST_REQUIRE_EQUAL(replacement.pruned.size(), 1U);
   BOOST_TEST(replacement.pruned.front() == 1U);
   BOOST_TEST(manager.size() == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_prepare_failure_is_instance_scoped) {
   const auto now = std::chrono::steady_clock::now();
   auto target = connection_manager{connection_manager::policy{}};
   auto other = connection_manager{connection_manager::policy{}};
   const auto record = connection_manager::session_record{.id = 1, .peer = peer(249)};

   target.fail_next_prepare_for_test();
   BOOST_REQUIRE(other.remember(record, now).accepted);
   BOOST_TEST(other.size() == 1U);
   BOOST_CHECK_THROW(static_cast<void>(target.remember(record, now)), std::bad_alloc);
   BOOST_TEST(target.size() == 0U);
   BOOST_TEST(target.current(8).active_peers == 0U);
   BOOST_TEST(other.size() == 1U);
   BOOST_TEST(other.current(8).active_peers == 1U);

   // Only the armed manager consumes the one-shot failure; its retry succeeds.
   BOOST_REQUIRE(target.remember(record, now).accepted);
   BOOST_TEST(target.size() == 1U);
   BOOST_TEST(target.current(8).active_peers == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_prepare_failure_preserves_sessions_and_retries) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(249));
   set_resource_session_limits(client_options, 1, (std::numeric_limits<std::size_t>::max)(), 1,
                               (std::numeric_limits<std::size_t>::max)(), session_admission_headroom::one_session);
   client_options.limits.session_low_watermark = 1;
   client_options.limits.session_grace_period = std::chrono::milliseconds{0};
   client_options.limits.session_prune_silence = std::chrono::milliseconds{1};

   auto first = node{runtime, options_for(peer(250))};
   auto second = node{runtime, options_for(peer(251))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(first);
   register_echo(second);

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(first_endpoint, node::connect_options{.expected_peer = first.local_peer()}));
   const auto before = client.metrics();

   detail::fail_next_connection_manager_prepare_for_test(client);
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()})),
       std::bad_alloc);

   const auto after_failure = client.metrics();
   BOOST_TEST(after_failure.active_sessions == before.active_sessions);
   BOOST_TEST(after_failure.sessions_pruned == before.sessions_pruned);
   BOOST_TEST(after_failure.sessions_opened == before.sessions_opened);
   BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 1U);
   BOOST_TEST(client.diagnostics().resources.transient.outbound_connections == 0U);
   BOOST_TEST(client.diagnostics().resources.system.file_descriptors == 1U);

   auto retained =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(first.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'r', 'e', 't', 'r', 'y'};
   forge::asio::blocking::run(runtime, retained.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, retained.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   (void)forge::asio::blocking::run(
       runtime, client.async_connect(second_endpoint, node::connect_options{.expected_peer = second.local_peer()}));
   const auto after_retry = client.metrics();
   BOOST_TEST(after_retry.active_sessions == 1U);
   BOOST_TEST(after_retry.sessions_pruned == before.sessions_pruned + 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_peer_index_failure_leaves_no_ghost_peer) {
   const auto now = std::chrono::steady_clock::now();
   auto manager = connection_manager{connection_manager::policy{
       .max_sessions = 2,
       .low_watermark = 1,
       .grace_period = std::chrono::milliseconds{0},
       .prune_silence = std::chrono::milliseconds{0},
   }};
   BOOST_REQUIRE(manager
                     .remember(
                         connection_manager::session_record{
                             .id = 1,
                             .peer = peer(252),
                             .opened_at = now,
                             .last_used_at = now,
                         },
                         now)
                     .accepted);
   const auto before = manager.current(8);

   detail::fail_next_connection_manager_peer_session_prepare_for_test();
   BOOST_CHECK_THROW(
       ([&] {
          static_cast<void>(manager.remember(
              connection_manager::session_record{
                  .id = 2,
                  .peer = peer(253),
                  .opened_at = now,
                  .last_used_at = now,
              },
              now));
       }()),
       std::bad_alloc);

   const auto after = manager.current(8);
   BOOST_TEST(after.active_sessions == before.active_sessions);
   BOOST_TEST(after.active_peers == before.active_peers);
   BOOST_TEST(after.sessions.size() == before.sessions.size());
   BOOST_TEST(manager.size() == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_rejects_duplicate_session_id_without_secondary_state) {
   const auto now = std::chrono::steady_clock::now();
   auto manager = connection_manager{connection_manager::policy{}};
   BOOST_REQUIRE(manager
                     .remember(connection_manager::session_record{
                                   .id = 1,
                                   .peer = peer(252),
                                   .opened_at = now,
                                   .last_used_at = now,
                               },
                               now)
                     .accepted);

   const auto duplicate = manager.remember(connection_manager::session_record{
                                               .id = 1,
                                               .peer = peer(253),
                                               .opened_at = now,
                                               .last_used_at = now,
                                           },
                                           now);

   BOOST_TEST(!duplicate.accepted);
   BOOST_TEST(duplicate.pruned.empty());
   const auto state = manager.current(8);
   BOOST_TEST(state.active_sessions == 1U);
   BOOST_TEST(state.active_peers == 1U);
   BOOST_REQUIRE_EQUAL(state.sessions.size(), 1U);
   BOOST_TEST(state.sessions.front().peer.value == peer(252).value);
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_allows_bounded_parallel_sessions_per_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(250));
   set_resource_session_limits(client_options, 4, (std::numeric_limits<std::size_t>::max)(), 4, 2);
   client_options.limits.session_low_watermark = 4;
   auto server = node{runtime, options_for(peer(251))};
   auto client = node{runtime, std::move(client_options)};
   register_echo(server);

   const auto endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(client.metrics().active_sessions == 2U);

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                                             node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'p', 'a', 'r', 'a', 'l', 'l', 'e', 'l'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_enforces_sessions_per_peer_limit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client_options = options_for(peer(252));
   set_resource_session_limits(client_options, 4, (std::numeric_limits<std::size_t>::max)(), 4, 1);
   client_options.limits.session_low_watermark = 4;
   auto server = node{runtime, options_for(peer(253))};
   auto client = node{runtime, std::move(client_options)};

   const auto endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_connect(endpoint, node::connect_options{.expected_peer = server.local_peer()}));
      BOOST_FAIL("expected per-peer session limit rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }
   BOOST_TEST(client.metrics().active_sessions == 1U);
   BOOST_TEST(client.metrics().connection_rejections >= 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_rejects_pending_outbound_limit_without_killing_first_attempt) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client_options = options_for(peer(246));
   client_options.limits.resources.transient.max_outbound_connections = 1;
   client_options.limits.resources.transient.max_connections = 1;
   auto client = node{runtime, std::move(client_options)};
   const auto stalled_endpoint = start_stalling_tcp_peer(runtime, std::chrono::milliseconds{500});

   auto first = boost::asio::co_spawn(runtime.context(),
                                      client.async_connect(stalled_endpoint,
                                                           node::connect_options{
                                                               .expected_peer = peer(247),
                                                               .allow_relay = false,
                                                               .timeout = std::chrono::milliseconds{500},
                                                           }),
                                      boost::asio::use_future);
   wait_on_runtime(runtime, std::chrono::milliseconds{50}, "pending outbound admission");

   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_connect(stalled_endpoint, node::connect_options{
                                                              .expected_peer = peer(247),
                                                              .allow_relay = false,
                                                              .timeout = std::chrono::milliseconds{100},
                                                          }));
      BOOST_FAIL("expected pending outbound session limit rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }

   try {
      (void)first.get();
   } catch (const forge::exceptions::base&) {
   }
   BOOST_TEST(client.metrics().connection_rejections >= 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_stop_cancels_pending_direct_connect) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = node{runtime, options_for(peer(248))};
   const auto stalled_endpoint = start_stalling_tcp_peer(runtime, std::chrono::seconds{5});

   auto pending = boost::asio::co_spawn(runtime.context(),
                                        client.async_connect(stalled_endpoint,
                                                             node::connect_options{
                                                                 .expected_peer = peer(249),
                                                                 .allow_relay = false,
                                                                 .timeout = std::chrono::seconds{5},
                                                             }),
                                        boost::asio::use_future);
   wait_on_runtime(runtime, std::chrono::milliseconds{50}, "pending direct connect");

   const auto started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(runtime, client.async_stop());
   const auto elapsed = std::chrono::steady_clock::now() - started;

   BOOST_TEST(elapsed < std::chrono::milliseconds{750});
   BOOST_REQUIRE(pending.wait_for(std::chrono::milliseconds{750}) == std::future_status::ready);
   try {
      (void)pending.get();
      BOOST_FAIL("expected pending direct connect cancellation");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::canceled));
   }
}

BOOST_AUTO_TEST_CASE(p2p_stop_rejects_session_completing_during_shutdown) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};

   for (auto remaining = 32U; remaining != 0U; --remaining) {
      auto server = node{runtime, options_for(peer(250))};
      auto client = node{runtime, options_for(peer(251))};
      const auto endpoint = listen(server, runtime);

      auto pending = boost::asio::co_spawn(runtime.context(),
                                           client.async_connect(endpoint,
                                                                node::connect_options{
                                                                    .expected_peer = server.local_peer(),
                                                                    .allow_relay = false,
                                                                    .timeout = std::chrono::seconds{2},
                                                                }),
                                           boost::asio::use_future);

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (server.metrics().active_sessions == 0U &&
             pending.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
         BOOST_REQUIRE(std::chrono::steady_clock::now() < deadline);
         wait_on_runtime(runtime, std::chrono::milliseconds{1}, "outbound shutdown race");
      }

      forge::asio::blocking::run(runtime, client.async_stop());
      BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
      try {
         (void)pending.get();
      } catch (const forge::exceptions::base& error) {
         BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
         const auto code = *forge::net::p2p::exceptions::code_of(error);
         const auto canceled_or_closed = code == exceptions::code::canceled || code == exceptions::code::closed;
         BOOST_TEST(canceled_or_closed);
      }

      const auto snapshot = client.diagnostics();
      BOOST_TEST(snapshot.metrics.active_sessions == 0U);
      BOOST_TEST(snapshot.sessions.empty());
      BOOST_TEST(snapshot.network.stopped);

      forge::asio::blocking::run(runtime, server.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_libp2p_relay_hop_codec_matches_spec_shape) {
   auto reserve = relay::hop_message{.kind = relay::hop_message::message_kind::reserve};
   BOOST_TEST(relay::codec::encode_hop(reserve) == std::vector<std::uint8_t>({0x02, 0x08, 0x00}),
              boost::test_tools::per_element());

   auto status = relay::hop_message{
       .kind = relay::hop_message::message_kind::status,
       .status = relay::status::ok,
   };
   BOOST_TEST(relay::codec::encode_hop(status) == std::vector<std::uint8_t>({0x04, 0x08, 0x02, 0x28, 0x64}),
              boost::test_tools::per_element());
   auto decoded = relay::codec::decode_hop(relay::codec::encode_hop(status));
   BOOST_TEST(static_cast<int>(decoded.kind) == static_cast<int>(relay::hop_message::message_kind::status));
   BOOST_TEST(static_cast<int>(decoded.status) == static_cast<int>(relay::status::ok));
}

BOOST_AUTO_TEST_CASE(p2p_libp2p_relay_wire_roundtrips_statuses_limits_and_voucher) {
   const auto identity = make_test_identity();
   const auto relay_peer = identity.peer;
   const auto target_peer = peer(97);
   const auto relay_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4103/quic-v1/p2p/" + relay_peer.to_string());
   const auto voucher = relay::codec::seal_reservation_voucher(
       relay::voucher{
           .relay_peer = relay_peer,
           .peer = target_peer,
           .expires_at = 1'777'000'000,
       },
       identity.key, forge::crypto::pki::pem::read_private_key(identity.private_key_pem));

   auto decoded_voucher = relay::codec::open_reservation_voucher(voucher, relay_peer, 1'776'999'999);
   BOOST_TEST(decoded_voucher.relay_peer.to_string() == relay_peer.to_string());
   BOOST_TEST(decoded_voucher.peer.to_string() == target_peer.to_string());
   BOOST_TEST(decoded_voucher.expires_at == 1'777'000'000ULL);

   for (auto status :
        {relay::status::ok, relay::status::reservation_refused, relay::status::resource_limit_exceeded,
         relay::status::permission_denied, relay::status::connection_failed, relay::status::no_reservation,
         relay::status::malformed_message, relay::status::unexpected_message}) {
      auto decoded = relay::codec::decode_hop(relay::codec::encode_hop(relay::hop_message{
          .kind = relay::hop_message::message_kind::status,
          .reservation_value =
              relay::reservation{
                  .expires_at = 1'777'000'000,
                  .relay_endpoints = std::vector<endpoint>{relay_endpoint},
                  .voucher = voucher,
              },
          .limit_value = relay::limit{.duration = std::chrono::seconds{60}, .data = 4096},
          .status = status,
      }));
      BOOST_TEST(static_cast<int>(decoded.kind) == static_cast<int>(relay::hop_message::message_kind::status));
      BOOST_TEST(static_cast<int>(decoded.status) == static_cast<int>(status));
      BOOST_REQUIRE(decoded.limit_value.has_value());
      BOOST_TEST(decoded.limit_value->duration == std::chrono::seconds{60});
      BOOST_TEST(decoded.limit_value->data == 4096U);
      if (status == relay::status::ok) {
         BOOST_REQUIRE(decoded.reservation_value.has_value());
         BOOST_REQUIRE(decoded.reservation_value->voucher.has_value());
         BOOST_TEST(decoded.reservation_value->voucher->encode() == voucher.encode(), boost::test_tools::per_element());
      }
   }

   BOOST_CHECK_THROW((void)relay::codec::decode_hop(std::vector<std::uint8_t>{0x02, 0x28, 0x64}),
                     forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_libp2p_dcutr_codec_matches_spec_shape) {
   auto connect = hole_punch::message{.kind = hole_punch::message::message_kind::connect};
   BOOST_TEST(hole_punch::codec::encode(connect) == std::vector<std::uint8_t>({0x02, 0x08, 0x64}),
              boost::test_tools::per_element());

   auto decoded = hole_punch::codec::decode(hole_punch::codec::encode(hole_punch::message{
       .kind = hole_punch::message::message_kind::sync,
   }));
   BOOST_TEST(static_cast<int>(decoded.kind) == static_cast<int>(hole_punch::message::message_kind::sync));
}

BOOST_AUTO_TEST_CASE(p2p_dcutr_attempt_tracks_rtt_retry_and_inflight_state) {
   auto attempt = hole_punch::attempt{};
   attempt.peer = peer(98);
   attempt.relay_peer = peer(99);
   attempt.rtt = std::chrono::milliseconds{80};
   attempt.max_attempts = 2;
   BOOST_TEST(attempt.sync_delay() == std::chrono::milliseconds{40});
   BOOST_TEST(attempt.try_begin());
   BOOST_TEST(!attempt.try_begin());
   attempt.finish(hole_punch::status::failed);
   BOOST_TEST(attempt.can_retry());
   BOOST_TEST(attempt.try_begin());
   attempt.finish(hole_punch::status::succeeded);
   BOOST_TEST(!attempt.can_retry());
   BOOST_TEST(static_cast<int>(attempt.result().value) == static_cast<int>(hole_punch::status::succeeded));
}

BOOST_AUTO_TEST_CASE(p2p_libp2p_autonat_v1_codec_matches_spec_shape) {
   const auto id = peer(91);
   const auto endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4101/quic-v1/p2p/" + id.to_string());
   auto dial = reachability::message{
       .kind = reachability::message::message_kind::dial,
       .peer =
           reachability::peer_info{
               .peer = id,
               .endpoints = std::vector<forge::net::p2p::endpoint>{endpoint},
           },
   };

   auto decoded = reachability::codec::decode_v1(reachability::codec::encode_v1(dial));
   BOOST_TEST(static_cast<int>(decoded.kind) == static_cast<int>(reachability::message::message_kind::dial));
   BOOST_REQUIRE(decoded.peer.has_value());
   BOOST_TEST(decoded.peer->peer.to_string() == id.to_string());
   BOOST_REQUIRE_EQUAL(decoded.peer->endpoints.size(), 1U);
   BOOST_TEST(decoded.peer->endpoints.front().to_string() == endpoint.to_string());

   auto response = reachability::codec::decode_v1(reachability::codec::encode_v1(reachability::message{
       .kind = reachability::message::message_kind::dial_response,
       .response =
           reachability::dial_response{
               .status = reachability::dial_status::ok,
               .endpoint = endpoint,
           },
   }));
   BOOST_REQUIRE(response.response.has_value());
   BOOST_TEST(static_cast<int>(response.response->status) == static_cast<int>(reachability::dial_status::ok));
   BOOST_REQUIRE(response.response->endpoint.has_value());
   BOOST_TEST(response.response->endpoint->to_string() == endpoint.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_libp2p_autonat_v2_codec_covers_nonce_data_and_statuses) {
   const auto id = peer(92);
   const auto remote_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4102/quic-v1/p2p/" + id.to_string());
   auto request = reachability::v2::message{
       .type = reachability::v2::message::kind::dial_request,
       .dial_request =
           reachability::v2::dial_request{
               .endpoints = std::vector<forge::net::p2p::endpoint>{remote_endpoint},
               .nonce = 0x0102'0304'0506'0708ULL,
           },
   };

   auto decoded = reachability::codec::decode_v2(reachability::codec::encode_v2(request));
   BOOST_TEST(static_cast<int>(decoded.type) == static_cast<int>(reachability::v2::message::kind::dial_request));
   BOOST_REQUIRE(decoded.dial_request.has_value());
   BOOST_TEST(decoded.dial_request->nonce == 0x0102'0304'0506'0708ULL);
   BOOST_REQUIRE_EQUAL(decoded.dial_request->endpoints.size(), 1U);
   BOOST_TEST(decoded.dial_request->endpoints.front().to_string() == remote_endpoint.to_string());

   auto data_request = reachability::codec::decode_v2(reachability::codec::encode_v2(reachability::v2::message{
       .type = reachability::v2::message::kind::dial_data_request,
       .dial_data_request = reachability::v2::dial_data_request{.index = 1, .bytes = 30 * 1024},
   }));
   BOOST_REQUIRE(data_request.dial_data_request.has_value());
   BOOST_TEST(data_request.dial_data_request->index == 1U);
   BOOST_TEST(data_request.dial_data_request->bytes == 30U * 1024U);

   auto data_response = reachability::codec::decode_v2(reachability::codec::encode_v2(reachability::v2::message{
       .type = reachability::v2::message::kind::dial_data_response,
       .dial_data_response = reachability::v2::dial_data_response{.data = std::vector<std::uint8_t>(4096, 0x5a)},
   }));
   BOOST_REQUIRE(data_response.dial_data_response.has_value());
   BOOST_TEST(data_response.dial_data_response->data.size() == 4096U);

   auto response = reachability::codec::decode_v2(reachability::codec::encode_v2(reachability::v2::message{
       .type = reachability::v2::message::kind::dial_response,
       .dial_response =
           reachability::v2::dial_response{
               .status = reachability::v2::response_status::ok,
               .index = 1,
               .dial_status = reachability::v2::dial_status::dial_back_error,
           },
   }));
   BOOST_REQUIRE(response.dial_response.has_value());
   BOOST_TEST(static_cast<int>(response.dial_response->status) ==
              static_cast<int>(reachability::v2::response_status::ok));
   BOOST_TEST(response.dial_response->index == 1U);
   BOOST_TEST(static_cast<int>(response.dial_response->dial_status) ==
              static_cast<int>(reachability::v2::dial_status::dial_back_error));

   auto dial_back = reachability::codec::decode_v2_dial_back(
       reachability::codec::encode_v2_dial_back(reachability::v2::dial_back{.nonce = request.dial_request->nonce}));
   BOOST_TEST(dial_back.nonce == request.dial_request->nonce);

   auto dial_back_response =
       reachability::codec::decode_v2_dial_back_response(reachability::codec::encode_v2_dial_back_response(
           reachability::v2::dial_back_response{.status = reachability::v2::dial_back_status::ok}));
   BOOST_TEST(static_cast<int>(dial_back_response.status) == static_cast<int>(reachability::v2::dial_back_status::ok));
}

BOOST_AUTO_TEST_CASE(p2p_libp2p_autonat_v2_rejects_oversized_data_response_and_unknown_status) {
   BOOST_CHECK_THROW(
       (void)reachability::codec::encode_v2(reachability::v2::message{
           .type = reachability::v2::message::kind::dial_data_response,
           .dial_data_response = reachability::v2::dial_data_response{.data = std::vector<std::uint8_t>(4097, 0x42)},
       }),
       forge::exceptions::base);

   // Message { dialResponse: DialResponse { status: 999 } }
   BOOST_CHECK_THROW(
       (void)reachability::codec::decode_v2(std::vector<std::uint8_t>{0x06, 0x12, 0x04, 0x08, 0xe7, 0x07}),
       forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_autonat_loopback_does_not_claim_public_reachability) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto observer_identity = make_test_certificate_identity("autonat-public-observer");
   const auto subject_identity = make_test_certificate_identity("autonat-public-subject");
   auto observer_options = options_for(
       observer_identity, capability_set{.bits = capabilities::direct_quic | capabilities::autonat});
   auto subject_options = options_for(
       subject_identity, capability_set{.bits = capabilities::direct_quic | capabilities::autonat});
   observer_options.reachability_policy.service_v2_enabled = true;
   observer_options.allow_insecure_test_mode = false;
   subject_options.allow_insecure_test_mode = false;
   observer_options.peer_state.persistence = peer_store::make_memory_persistence();
   subject_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto observer = node{runtime, std::move(observer_options)};
   auto subject = node{runtime, std::move(subject_options)};
   BOOST_TEST(observer.local_peer().to_string() == observer_identity.peer.to_string());
   BOOST_TEST(subject.local_peer().to_string() == subject_identity.peer.to_string());
   BOOST_TEST(observer.local_peer().to_string() != subject.local_peer().to_string());

   const auto observer_endpoint = listen(observer, runtime);
   (void)listen(subject, runtime);
   subject.peers().learn_endpoint(observer.local_peer(), observer_endpoint,
                                  capability_set{.bits = capabilities::direct_quic | capabilities::autonat});

   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(
       runtime, subject.async_probe_reachability(observer.local_peer()))), exceptions::unsupported_protocol);
   BOOST_CHECK(subject.reachability_status().effective == reachability::state::unknown);
   BOOST_TEST(!subject.peers().find(subject.local_peer()).has_value());

   forge::asio::blocking::run(runtime, subject.async_stop());
   forge::asio::blocking::run(runtime, observer.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_reservation_persists_candidate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto relay_node =
       node{runtime, options_for(peer(102), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto client = node{runtime, options_for(peer(103), capability_set{.bits = capabilities::direct_quic |
                                                                             capabilities::relay_reservation})};

   const auto relay_endpoint = listen(relay_node, runtime);
   client.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});

   const auto info = forge::asio::blocking::run(runtime, client.async_reserve_relay(relay_node.local_peer()));
   BOOST_TEST(info.relay_peer.to_string() == relay_node.local_peer().to_string());
   BOOST_TEST(!info.voucher.has_value());

   const auto stored = client.peers().find(relay_node.local_peer());
   BOOST_REQUIRE(stored.has_value());
   BOOST_REQUIRE_EQUAL(stored->relay_reservations.size(), 1U);
   BOOST_TEST(stored->relay_reservations.front().relay.to_string() == relay_node.local_peer().to_string());
   BOOST_TEST(stored->relay_reservations.front().voucher.empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_hop_timeout_preserves_shared_authenticated_session) {
   auto relay_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-hop-session-relay");
   const auto source_identity = make_test_certificate_identity("relay-hop-session-source");
   const auto target_identity = make_test_certificate_identity("relay-hop-session-target");
   auto relay_node =
       node{relay_runtime,
            options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                               capabilities::relay_reservation})};
   auto source = node{client_runtime,
                      options_for(source_identity,
                                  capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation})};
   auto target = node{client_runtime,
                      options_for(target_identity,
                                  capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation})};
   relay_node.register_protocol_handler(
       builtins::echo, [](node::incoming_protocol_stream incoming) mutable -> boost::asio::awaitable<void> {
          for (auto request = 0U; request < 2U; ++request) {
             auto payload = co_await incoming.stream.async_read_frame();
             co_await incoming.stream.async_write_frame(payload);
          }
          co_await incoming.stream.async_close();
       });

   const auto relay_endpoint = listen(relay_node, relay_runtime);
   const auto relay_capabilities =
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation};
   source.peers().learn_endpoint(relay_node.local_peer(), relay_endpoint, relay_capabilities);
   target.peers().learn_endpoint(relay_node.local_peer(), relay_endpoint, relay_capabilities);
   static_cast<void>(forge::asio::blocking::run(client_runtime, source.async_reserve_relay(relay_node.local_peer())));
   static_cast<void>(forge::asio::blocking::run(client_runtime, target.async_reserve_relay(relay_node.local_peer())));

   const auto before = source.diagnostics();
   const auto established = std::ranges::find_if(before.sessions, [&](const diagnostics::session& session) {
      return session.remote_peer == relay_node.local_peer() && !session.closed &&
             session.identify_state == identify::state::identified;
   });
   BOOST_REQUIRE(established != before.sessions.end());
   const auto session_id = established->id;
   const auto sessions_opened = source.metrics().sessions_opened;
   auto sibling = forge::asio::blocking::run(
       client_runtime, source.async_open_protocol_stream(relay_node.local_peer(), builtins::echo,
                                                         node::open_options{
                                                             .allow_relay = false,
                                                             .timeout = std::chrono::seconds{2},
                                                             .direct_attempt_timeout = std::chrono::seconds{2},
                                                             .max_direct_endpoints = 1,
                                                         }));
   const auto first_payload = std::vector<std::uint8_t>{'b', 'e', 'f', 'o', 'r', 'e'};
   forge::asio::blocking::run(client_runtime, sibling.async_write_frame(first_payload));
   BOOST_TEST(forge::asio::blocking::run(client_runtime, sibling.async_read_frame()) == first_payload,
              boost::test_tools::per_element());

   auto release_relay = block_runtime(relay_runtime, "relay HOP stream negotiation barrier");
   auto open = boost::asio::co_spawn(
       client_runtime.context(),
       source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                         node::open_options{
                                             .allow_relay = true,
                                             .relay_peer = relay_node.local_peer(),
                                             .direct_attempt_timeout = std::chrono::milliseconds{50},
                                             .relay_attempt_timeout = std::chrono::milliseconds{200},
                                             .allow_hole_punch = false,
                                         }),
       boost::asio::use_future);
   BOOST_REQUIRE(open.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   try {
      static_cast<void>(open.get());
      BOOST_FAIL("expected relay HOP timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }
   BOOST_TEST(std::ranges::any_of(source.diagnostics().sessions, [&](const diagnostics::session& session) {
      return session.id == session_id && session.remote_peer == relay_node.local_peer() && !session.closed;
   }));
   release_relay->set_value();

   const auto second_payload = std::vector<std::uint8_t>{'a', 'f', 't', 'e', 'r'};
   forge::asio::blocking::run(client_runtime, sibling.async_write_frame(second_payload));
   BOOST_TEST(forge::asio::blocking::run(client_runtime, sibling.async_read_frame()) == second_payload,
              boost::test_tools::per_element());
   forge::asio::blocking::run(client_runtime, sibling.async_close());
   BOOST_TEST(source.metrics().sessions_opened == sessions_opened);
   BOOST_TEST(std::ranges::any_of(source.diagnostics().sessions, [&](const diagnostics::session& session) {
      return session.id == session_id && session.remote_peer == relay_node.local_peer() && !session.closed;
   }));

   forge::asio::blocking::run(client_runtime, target.async_stop());
   forge::asio::blocking::run(client_runtime, source.async_stop());
   forge::asio::blocking::run(relay_runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_autorelay_refresh_reserves_peer_store_candidate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto relay_node =
       node{runtime, options_for(peer(104), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto client_options =
       options_for(peer(105), capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   client_options.relay_policy.target_reservations = 1;
   client_options.relay_policy.max_candidates_per_refresh = 2;
   client_options.relay_policy.max_parallel_reservations = 1;
   auto client = node{runtime, std::move(client_options)};

   const auto relay_endpoint = listen(relay_node, runtime);
   client.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});

   const auto reservations = forge::asio::blocking::run(runtime, client.async_refresh_relay_candidates());
   BOOST_REQUIRE_EQUAL(reservations.size(), 1U);
   BOOST_TEST(reservations.front().relay_peer.to_string() == relay_node.local_peer().to_string());

   const auto stored = client.peers().find(relay_node.local_peer());
   BOOST_REQUIRE(stored.has_value());
   BOOST_REQUIRE_EQUAL(stored->relay_reservations.size(), 1U);
   BOOST_TEST(stored->relay_reservations.front().relay.to_string() == relay_node.local_peer().to_string());
   BOOST_TEST(client.metrics().relay_discovery_refreshes == 1U);
   BOOST_TEST(client.metrics().relay_discovery_attempts == 1U);
   BOOST_TEST(client.metrics().relay_discovery_successes == 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_autorelay_refresh_backs_off_failed_candidate_and_tries_next) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto bad_options = options_for(peer(106), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                    capabilities::relay_reservation});
   bad_options.relay_policy.service_enabled = false;
   auto bad_relay = node{runtime, std::move(bad_options)};
   auto good_relay =
       node{runtime, options_for(peer(107), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto client_options =
       options_for(peer(108), capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   client_options.relay_policy.target_reservations = 1;
   client_options.relay_policy.max_candidates_per_refresh = 2;
   client_options.relay_policy.max_parallel_reservations = 1;
   client_options.relay_policy.candidate_backoff = std::chrono::seconds{30};
   auto client = node{runtime, std::move(client_options)};

   const auto bad_endpoint = listen(bad_relay, runtime);
   const auto good_endpoint = listen(good_relay, runtime);
   client.peers().learn_endpoint(
       bad_relay.local_peer(), bad_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   client.peers().learn_endpoint(
       good_relay.local_peer(), good_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   auto bad_record = client.peers().find(bad_relay.local_peer()).value();
   bad_record.score = 100.0;
   client.peers().upsert(std::move(bad_record));

   const auto reservations = forge::asio::blocking::run(runtime, client.async_refresh_relay_candidates());
   BOOST_REQUIRE_EQUAL(reservations.size(), 1U);
   BOOST_TEST(reservations.front().relay_peer.to_string() == good_relay.local_peer().to_string());
   BOOST_TEST(client.metrics().relay_discovery_attempts == 2U);
   BOOST_TEST(client.metrics().relay_discovery_failures == 1U);
   BOOST_TEST(client.metrics().relay_discovery_successes == 1U);

   const auto failed = client.peers().find(bad_relay.local_peer());
   BOOST_REQUIRE(failed.has_value());
   BOOST_TEST(failed->discovery_backoff_until > std::chrono::system_clock::now());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, good_relay.async_stop());
   forge::asio::blocking::run(runtime, bad_relay.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_autorelay_refresh_accepts_dht_and_rendezvous_sourced_candidates) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto dht_relay =
       node{runtime, options_for(peer(116), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto rendezvous_relay =
       node{runtime, options_for(peer(117), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto client_options =
       options_for(peer(120), capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   client_options.relay_policy.target_reservations = 2;
   client_options.relay_policy.max_candidates_per_refresh = 2;
   client_options.relay_policy.max_parallel_reservations = 1;
   auto client = node{runtime, std::move(client_options)};

   const auto dht_endpoint = listen(dht_relay, runtime);
   const auto rendezvous_endpoint = listen(rendezvous_relay, runtime);
   client.peers().upsert(peer_store::record{
       .peer = dht_relay.local_peer(),
       .capabilities =
           capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation},
       .discovered_by = discovery::source::dht,
       .endpoints = std::vector<peer_store::endpoint_record>{
           peer_store::endpoint_record{.address = address_for(dht_endpoint)}},
       .discovery_expires_at = std::chrono::system_clock::now() + std::chrono::minutes{5},
   });
   client.peers().upsert(peer_store::record{
       .peer = rendezvous_relay.local_peer(),
       .capabilities =
           capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation},
       .discovered_by = discovery::source::rendezvous,
       .endpoints =
           std::vector<peer_store::endpoint_record>{peer_store::endpoint_record{
               .address = address_for(rendezvous_endpoint)}},
       .discovery_expires_at = std::chrono::system_clock::now() + std::chrono::minutes{5},
   });

   const auto reservations = forge::asio::blocking::run(runtime, client.async_refresh_relay_candidates());
   BOOST_REQUIRE_EQUAL(reservations.size(), 2U);
   BOOST_TEST(client.metrics().relay_discovery_attempts == 2U);
   BOOST_TEST(client.metrics().relay_discovery_successes == 2U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, rendezvous_relay.async_stop());
   forge::asio::blocking::run(runtime, dht_relay.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_autorelay_refresh_respects_candidate_and_target_limits) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto first =
       node{runtime, options_for(peer(113), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto second =
       node{runtime, options_for(peer(114), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto client_options =
       options_for(peer(115), capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   client_options.relay_policy.target_reservations = 1;
   client_options.relay_policy.max_candidates_per_refresh = 1;
   client_options.relay_policy.max_parallel_reservations = 1;
   auto client = node{runtime, std::move(client_options)};

   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   client.peers().learn_endpoint(
       first.local_peer(), first_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   client.peers().learn_endpoint(
       second.local_peer(), second_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});

   const auto reservations = forge::asio::blocking::run(runtime, client.async_refresh_relay_candidates());
   BOOST_REQUIRE_EQUAL(reservations.size(), 1U);
   BOOST_TEST(client.metrics().relay_discovery_attempts == 1U);
   BOOST_TEST(client.metrics().relay_discovery_successes == 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_fallback_refreshes_candidate_without_explicit_relay_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("autorelay-fallback-relay");
   const auto source_identity = make_test_certificate_identity("autorelay-fallback-source");
   const auto target_identity = make_test_certificate_identity("autorelay-fallback-target");
   auto relay_node = node{
       runtime, options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto source_options = options_for(
       source_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   source_options.relay_policy.target_reservations = 1;
   source_options.relay_policy.max_candidates_per_refresh = 2;
   source_options.relay_policy.max_parallel_reservations = 1;
   auto source = node{runtime, std::move(source_options)};
   auto target = node{runtime, options_for(target_identity, capability_set{.bits = capabilities::direct_quic |
                                                                                   capabilities::relay_reservation})};
   register_echo(target);

   const auto relay_endpoint = listen(relay_node, runtime);
   (void)listen(target, runtime);
   const auto closed_direct_endpoint = start_stalling_tcp_peer(runtime, std::chrono::milliseconds{10});
   source.peers().learn_endpoint(target.local_peer(), closed_direct_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   target.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));

   auto stream = forge::asio::blocking::run(
       runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                                  node::open_options{
                                                      .allow_relay = true,
                                                      .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                      .relay_attempt_timeout = std::chrono::milliseconds{2'000},
                                                      .allow_hole_punch = false,
                                                  }));
   const auto payload = std::vector<std::uint8_t>{'a', 'u', 't', 'o'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(source.metrics().relay_discovery_refreshes >= 1U);
   BOOST_TEST(source.metrics().relay_discovery_successes >= 1U);
   BOOST_TEST(source.metrics().path_relay_opens >= 1U);

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_policy_options_are_behavioral) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto relay_options = options_for(peer(110), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                      capabilities::relay_reservation});
   relay_options.relay_policy.service_enabled = false;
   auto relay_node = node{runtime, std::move(relay_options)};
   auto client = node{runtime, options_for(peer(111), capability_set{.bits = capabilities::direct_quic |
                                                                             capabilities::relay_reservation})};

   const auto relay_endpoint = listen(relay_node, runtime);
   client.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});

   try {
      (void)forge::asio::blocking::run(runtime, client.async_reserve_relay(relay_node.local_peer()));
      BOOST_FAIL("expected relay service policy rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::relay_rejected));
   }

   auto disabled_client_options = options_for(peer(112), capability_set{.bits = capabilities::direct_quic});
   disabled_client_options.relay_policy.client_enabled = false;
   auto disabled_client = node{runtime, std::move(disabled_client_options)};
   try {
      (void)forge::asio::blocking::run(runtime, disabled_client.async_reserve_relay(relay_node.local_peer()));
      BOOST_FAIL("expected relay client policy rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::relay_not_available));
   }

   forge::asio::blocking::run(runtime, disabled_client.async_stop());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_duration_requires_exact_wire_seconds) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto relay_options = options_for(peer(109), capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                      capabilities::relay_reservation});
   relay_options.limits.relay.max_duration = std::chrono::milliseconds{1'500};

   BOOST_CHECK_THROW((void)node(runtime, std::move(relay_options)), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_relay_connect_requires_target_reservation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-reservation-owner-relay");
   const auto source_identity = make_test_certificate_identity("relay-reservation-owner-source");
   const auto target_identity = make_test_certificate_identity("relay-reservation-owner-target");
   auto relay_node = node{
       runtime, options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto source = node{runtime, options_for(source_identity, capability_set{.bits = capabilities::direct_quic})};
   auto target = node{runtime, options_for(target_identity, capability_set{.bits = capabilities::direct_quic})};
   register_echo(target);

   const auto relay_endpoint = listen(relay_node, runtime);
   const auto target_endpoint = listen(target, runtime);
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   relay_node.peers().learn_endpoint(target.local_peer(), target_endpoint,
                                     capability_set{.bits = capabilities::direct_quic});
   (void)forge::asio::blocking::run(runtime, source.async_reserve_relay(relay_node.local_peer()));

   try {
      (void)forge::asio::blocking::run(
          runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                                     node::open_options{
                                                         .allow_relay = true,
                                                         .relay_peer = relay_node.local_peer(),
                                                         .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                         .relay_attempt_timeout = std::chrono::milliseconds{2'000},
                                                         .allow_hole_punch = false,
                                                     }));
      BOOST_FAIL("expected relay CONNECT rejection without target reservation");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::relay_rejected));
   }

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_stop_connect_passes_frames_after_target_reservation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-stop-connect-relay");
   const auto source_identity = make_test_certificate_identity("relay-stop-connect-source");
   const auto target_identity = make_test_certificate_identity("relay-stop-connect-target");
   auto relay_node = node{
       runtime, options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto source = node{runtime, options_for(source_identity, capability_set{.bits = capabilities::direct_quic})};
   auto target = node{runtime, options_for(target_identity, capability_set{.bits = capabilities::direct_quic |
                                                                                   capabilities::relay_reservation})};
   register_echo(target);

   const auto relay_endpoint = listen(relay_node, runtime);
   (void)listen(target, runtime);
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   target.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));

   auto stream = forge::asio::blocking::run(
       runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                                  node::open_options{
                                                      .allow_relay = true,
                                                      .relay_peer = relay_node.local_peer(),
                                                      .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                      .relay_attempt_timeout = std::chrono::milliseconds{2'000},
                                                      .allow_hole_punch = false,
                                                  }));
   const auto payload = std::vector<std::uint8_t>{'r', 'e', 'l', 'a', 'y'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(relay_node.metrics().relays_opened >= 1U);
   BOOST_TEST(source.metrics().path_relay_opens >= 1U);

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_hop_and_stop_apply_connection_gater_without_inventing_direct_addresses) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-gater-relay");
   const auto source_identity = make_test_certificate_identity("relay-gater-source");
   const auto target_identity = make_test_certificate_identity("relay-gater-target");
   auto relay_node = node{
       runtime, options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto source_gater = std::make_shared<recording_connection_gater>();
   auto target_gater = std::make_shared<recording_connection_gater>();
   auto source_options = options_for(source_identity, capability_set{.bits = capabilities::direct_quic});
   source_options.connection_gater = source_gater;
   source_options.path_policy.allow_direct = false;
   source_options.path_policy.allow_hole_punch = false;
   auto target_options = options_for(
       target_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   target_options.connection_gater = target_gater;
   auto source = node{runtime, std::move(source_options)};
   auto target = node{runtime, std::move(target_options)};
   register_echo(target);

   const auto relay_endpoint = listen(relay_node, runtime);
   (void)listen(target, runtime);
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   target.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));
   target_gater->clear();

   auto stream =
       forge::asio::blocking::run(runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                                                             node::open_options{
                                                                                 .allow_relay = true,
                                                                                 .relay_peer = relay_node.local_peer(),
                                                                                 .allow_hole_punch = false,
                                                                             }));
   const auto payload = std::vector<std::uint8_t>{'g', 'a', 't', 'e'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, stream.async_read_frame()) == payload,
              boost::test_tools::per_element());

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (target_gater->stages_for(source.local_peer()).size() != 2U && std::chrono::steady_clock::now() < deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "relayed STOP connection gater stages");
   }
   check_gater_stages(source_gater->stages_for(target.local_peer()),
                      {recorded_gater_stage::peer_dial, recorded_gater_stage::address_dial,
                       recorded_gater_stage::secured, recorded_gater_stage::upgraded});
   check_gater_stages(target_gater->stages_for(source.local_peer()),
                      {recorded_gater_stage::secured, recorded_gater_stage::upgraded});
   BOOST_TEST(std::ranges::count(target_gater->stages(), recorded_gater_stage::accept) == 1U);
   BOOST_TEST(std::ranges::count(source_gater->stages_for(target.local_peer()), recorded_gater_stage::address_dial) ==
              1U);

   auto source_circuit = relay_endpoint;
   source_circuit.peer = relay_node.local_peer();
   source_circuit.relayed = endpoint::circuit{.target = source.local_peer()};
   auto target_circuit = relay_endpoint;
   target_circuit.peer = relay_node.local_peer();
   target_circuit.relayed = endpoint::circuit{.target = target.local_peer()};
   const auto source_calls = source_gater->calls();
   const auto target_calls = target_gater->calls();
   const auto source_call = [&](recorded_gater_stage stage) -> const recorded_gater_call& {
      const auto found = std::ranges::find_if(
          source_calls, [&](const auto& call) { return call.stage == stage && call.peer == target.local_peer(); });
      BOOST_REQUIRE(found != source_calls.end());
      BOOST_REQUIRE(found->endpoints);
      return *found;
   };
   const auto target_call = [&](recorded_gater_stage stage) -> const recorded_gater_call& {
      const auto found = std::ranges::find_if(target_calls, [&](const auto& call) {
         return call.stage == stage && (stage == recorded_gater_stage::accept || call.peer == source.local_peer());
      });
      BOOST_REQUIRE(found != target_calls.end());
      BOOST_REQUIRE(found->endpoints);
      return *found;
   };
   BOOST_TEST(source_call(recorded_gater_stage::address_dial).endpoints->remote.to_string() ==
              target_circuit.to_string());
   BOOST_TEST(source_call(recorded_gater_stage::secured).endpoints->local.to_string() == source_circuit.to_string());
   BOOST_TEST(source_call(recorded_gater_stage::secured).endpoints->remote.to_string() == target_circuit.to_string());
   BOOST_TEST(source_call(recorded_gater_stage::upgraded).endpoints->local.to_string() == source_circuit.to_string());
   BOOST_TEST(source_call(recorded_gater_stage::upgraded).endpoints->remote.to_string() == target_circuit.to_string());
   BOOST_TEST(target_call(recorded_gater_stage::accept).endpoints->local.to_string() == target_circuit.to_string());
   BOOST_TEST(target_call(recorded_gater_stage::accept).endpoints->remote.to_string() == source_circuit.to_string());
   BOOST_TEST(target_call(recorded_gater_stage::secured).endpoints->local.to_string() == target_circuit.to_string());
   BOOST_TEST(target_call(recorded_gater_stage::secured).endpoints->remote.to_string() == source_circuit.to_string());
   BOOST_TEST(target_call(recorded_gater_stage::upgraded).endpoints->local.to_string() == target_circuit.to_string());
   BOOST_TEST(target_call(recorded_gater_stage::upgraded).endpoints->remote.to_string() == source_circuit.to_string());

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_address_gater_denial_prevents_hop_connect) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-gater-denial-relay");
   const auto source_identity = make_test_certificate_identity("relay-gater-denial-source");
   const auto target_identity = make_test_certificate_identity("relay-gater-denial-target");
   auto relay_node = node{
       runtime, options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto source_gater =
       std::make_shared<recording_connection_gater>(recorded_gater_stage::address_dial, target_identity.peer);
   auto source_options = options_for(source_identity, capability_set{.bits = capabilities::direct_quic});
   source_options.connection_gater = source_gater;
   source_options.path_policy.allow_direct = false;
   source_options.path_policy.allow_hole_punch = false;
   auto target_options = options_for(
       target_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   auto source = node{runtime, std::move(source_options)};
   auto target = node{runtime, std::move(target_options)};

   const auto relay_endpoint = listen(relay_node, runtime);
   (void)listen(target, runtime);
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   target.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   static_cast<void>(forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer())));

   auto rejected = false;
   try {
      static_cast<void>(forge::asio::blocking::run(
          runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo, node::open_options{
                                                                                   .allow_relay = true,
                                                                                   .relay_peer = relay_node.local_peer(),
                                                                                   .allow_hole_punch = false,
                                                                               })));
   } catch (const forge::exceptions::base& error) {
      rejected = exceptions::code_of(error) == exceptions::code::connection_rejected;
   }

   BOOST_TEST(rejected);
   check_gater_stages(source_gater->stages_for(target.local_peer()),
                      {recorded_gater_stage::peer_dial, recorded_gater_stage::address_dial});
   BOOST_TEST(relay_node.metrics().relays_opened == 0U);
   BOOST_TEST(relay_node.metrics().active_relays == 0U);
   BOOST_TEST(source.metrics().path_relay_opens == 0U);

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
   const auto resources = source.diagnostics().resources;
   BOOST_TEST(resources.transient.outbound_connections == 0U);
   BOOST_TEST(resources.peers.outbound_connections == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_relay_duration_closes_circuit_and_releases_resources) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-duration-relay");
   const auto source_identity = make_test_certificate_identity("relay-duration-source");
   const auto target_identity = make_test_certificate_identity("relay-duration-target");
   auto relay_options =
       options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                          capabilities::relay_reservation});
   relay_options.limits.relay.max_duration = std::chrono::seconds{1};
   auto relay_node = node{runtime, std::move(relay_options)};
   auto source = node{runtime, options_for(source_identity, capability_set{.bits = capabilities::direct_quic})};
   auto target = node{runtime, options_for(target_identity, capability_set{.bits = capabilities::direct_quic |
                                                                                   capabilities::relay_reservation})};
   register_echo(target);

   const auto relay_endpoint = listen(relay_node, runtime);
   (void)listen(target, runtime);
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   target.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));

   auto stream = forge::asio::blocking::run(
       runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                                  node::open_options{
                                                      .allow_relay = true,
                                                      .relay_peer = relay_node.local_peer(),
                                                      .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                      .relay_attempt_timeout = std::chrono::milliseconds{2'000},
                                                      .allow_hole_punch = false,
                                                  }));
   const auto opened_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (relay_node.metrics().active_relays == 0 && std::chrono::steady_clock::now() < opened_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relay duration admission");
   }
   BOOST_REQUIRE(relay_node.metrics().active_relays == 1U);
   const auto admitted_resources = relay_node.diagnostics().resources;
   BOOST_REQUIRE(active_stream_count(admitted_resources) > 0U);

   const auto closed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
   while ((relay_node.metrics().active_relays != 0 || active_stream_count(relay_node.diagnostics().resources) != 0) &&
          std::chrono::steady_clock::now() < closed_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relay duration cleanup");
   }
   BOOST_TEST(relay_node.metrics().active_relays == 0U);
   BOOST_TEST(active_stream_count(relay_node.diagnostics().resources) == 0U);

   auto read = boost::asio::co_spawn(runtime.context(), stream.async_read_frame(), boost::asio::use_future);
   if (read.wait_for(std::chrono::seconds{1}) != std::future_status::ready) {
      stream.cancel();
      BOOST_FAIL("relay duration did not close the client stream");
   } else {
      BOOST_CHECK_THROW((void)read.get(), forge::exceptions::base);
   }

   auto next = forge::asio::blocking::run(
       runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                                  node::open_options{
                                                      .allow_relay = true,
                                                      .relay_peer = relay_node.local_peer(),
                                                      .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                      .relay_attempt_timeout = std::chrono::milliseconds{2'000},
                                                      .allow_hole_punch = false,
                                                  }));
   const auto payload = std::vector<std::uint8_t>{'n', 'e', 'x', 't'};
   forge::asio::blocking::run(runtime, next.async_write_frame(payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, next.async_read_frame()) == payload,
              boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_renewal_preserves_active_circuit_limit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-renewal-relay");
   const auto first_identity = make_test_certificate_identity("relay-renewal-first");
   const auto second_identity = make_test_certificate_identity("relay-renewal-second");
   const auto target_identity = make_test_certificate_identity("relay-renewal-target");
   auto relay_options =
       options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                          capabilities::relay_reservation});
   relay_options.limits.relay.max_streams_per_reservation = 1;
   relay_options.limits.relay.max_duration = std::chrono::seconds{10};
   auto relay_node = node{runtime, std::move(relay_options)};
   auto first = node{runtime, options_for(first_identity, capability_set{.bits = capabilities::direct_quic})};
   auto second = node{runtime, options_for(second_identity, capability_set{.bits = capabilities::direct_quic})};
   auto target = node{runtime, options_for(target_identity, capability_set{.bits = capabilities::direct_quic |
                                                                                   capabilities::relay_reservation})};
   register_echo(target);

   const auto relay_endpoint = listen(relay_node, runtime);
   (void)listen(target, runtime);
   const auto relay_capabilities =
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation};
   first.peers().learn_endpoint(relay_node.local_peer(), relay_endpoint, relay_capabilities);
   second.peers().learn_endpoint(relay_node.local_peer(), relay_endpoint, relay_capabilities);
   target.peers().learn_endpoint(relay_node.local_peer(), relay_endpoint, relay_capabilities);
   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));

   const auto open_options = node::open_options{
       .allow_relay = true,
       .relay_peer = relay_node.local_peer(),
       .direct_attempt_timeout = std::chrono::milliseconds{100},
       .relay_attempt_timeout = std::chrono::milliseconds{2'000},
       .allow_hole_punch = false,
   };
   auto first_stream = forge::asio::blocking::run(
       runtime, first.async_open_protocol_stream(target.local_peer(), builtins::echo, open_options));
   const auto admitted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (relay_node.metrics().active_relays != 1U && std::chrono::steady_clock::now() < admitted_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relay renewal initial circuit");
   }
   BOOST_REQUIRE(relay_node.metrics().active_relays == 1U);

   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));
   BOOST_CHECK_THROW((void)forge::asio::blocking::run(
                         runtime, second.async_open_protocol_stream(target.local_peer(), builtins::echo, open_options)),
                     forge::exceptions::base);
   BOOST_TEST(relay_node.metrics().active_relays == 1U);

   first_stream.cancel();
   forge::asio::blocking::run(runtime, first.async_stop());
   const auto released_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (relay_node.metrics().active_relays != 0U && std::chrono::steady_clock::now() < released_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relay renewal circuit release");
   }
   BOOST_REQUIRE(relay_node.metrics().active_relays == 0U);

   auto second_stream = forge::asio::blocking::run(
       runtime, second.async_open_protocol_stream(target.local_peer(), builtins::echo, open_options));
   const auto payload = std::vector<std::uint8_t>{'r', 'e', 'n', 'e', 'w'};
   forge::asio::blocking::run(runtime, second_stream.async_write_frame(payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, second_stream.async_read_frame()) == payload,
              boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_reservation_is_released_after_last_disconnect) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-disconnect-relay");
   const auto first_identity = make_test_certificate_identity("relay-disconnect-first");
   const auto second_identity = make_test_certificate_identity("relay-disconnect-second");
   auto relay_options =
       options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                          capabilities::relay_reservation});
   relay_options.limits.relay.max_reservations = 1;
   relay_options.limits.resources.max_relay_reservations = 1;
   auto relay_node = node{runtime, std::move(relay_options)};
   auto first = node{runtime, options_for(first_identity, capability_set{.bits = capabilities::direct_quic |
                                                                                 capabilities::relay_reservation})};
   auto second = node{runtime, options_for(second_identity, capability_set{.bits = capabilities::direct_quic |
                                                                                   capabilities::relay_reservation})};

   const auto relay_endpoint = listen(relay_node, runtime);
   const auto relay_capabilities =
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation};
   first.peers().learn_endpoint(relay_node.local_peer(), relay_endpoint, relay_capabilities);
   second.peers().learn_endpoint(relay_node.local_peer(), relay_endpoint, relay_capabilities);
   (void)forge::asio::blocking::run(runtime, first.async_reserve_relay(relay_node.local_peer()));
   BOOST_REQUIRE(relay_node.metrics().active_relay_reservations == 1U);

   forge::asio::blocking::run(runtime, first.async_stop());
   const auto released_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (relay_node.metrics().active_relay_reservations != 0U &&
          std::chrono::steady_clock::now() < released_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relay reservation disconnect release");
   }
   BOOST_REQUIRE(relay_node.metrics().active_relay_reservations == 0U);

   (void)forge::asio::blocking::run(runtime, second.async_reserve_relay(relay_node.local_peer()));
   BOOST_TEST(relay_node.metrics().active_relay_reservations == 1U);

   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_only_open_does_not_record_direct_failure_or_evict_dht_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto relay_identity = make_test_certificate_identity("relay-only-accounting-relay");
   const auto source_identity = make_test_certificate_identity("relay-only-accounting-source");
   const auto target_identity = make_test_certificate_identity("relay-only-accounting-target");
   auto relay_options =
       options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                          capabilities::relay_reservation});
   auto custom_limits = dht::options{};
   custom_limits.failure_threshold = 1;
   custom_limits.query_timeout = std::chrono::milliseconds{250};
   auto source_options = dht_options_for(source_identity, custom_test_dht_profile(dht::mode::client, custom_limits));
   source_options.path_policy.allow_direct = false;
   auto target_options =
       dht_options_for(target_identity, custom_test_dht_profile(dht::mode::server, custom_limits),
                       capability_set{.bits = capabilities::direct_quic | capabilities::relay_reservation});
   auto relay_node = node{runtime, std::move(relay_options)};
   auto source = node{runtime, std::move(source_options)};
   auto target = node{runtime, std::move(target_options)};
   register_echo(target);

   const auto relay_endpoint = listen(relay_node, runtime);
   const auto source_endpoint = listen(source, runtime);
   const auto target_endpoint = listen(target, runtime);
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   target.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   (void)forge::asio::blocking::run(
       runtime, target.async_connect(source_endpoint, node::connect_options{.expected_peer = source.local_peer()}));
   for (auto attempt = 0U; attempt < 20U && source.routing_status(content_swarm_test_dht).active == 0U; ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "inbound DHT Identify admission");
   }
   BOOST_REQUIRE(source.routing_status(content_swarm_test_dht).active > 0U);
   for (auto attempt = 0U; attempt < 40U; ++attempt) {
      const auto state = source.diagnostics();
      const auto profile = std::ranges::find_if(
          state.dht_profiles, [](const auto& value) { return value.protocol == content_swarm_test_dht; });
      BOOST_REQUIRE(profile != state.dht_profiles.end());
      if (!profile->maintenance_startup_pending && !profile->maintenance_in_flight) {
         break;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "startup DHT maintenance completion");
   }
   const auto stable = source.diagnostics();
   const auto stable_profile = std::ranges::find_if(
       stable.dht_profiles, [](const auto& value) { return value.protocol == content_swarm_test_dht; });
   BOOST_REQUIRE(stable_profile != stable.dht_profiles.end());
   BOOST_REQUIRE(!stable_profile->maintenance_startup_pending);
   BOOST_REQUIRE(!stable_profile->maintenance_in_flight);
   (void)forge::asio::blocking::run(runtime, source.async_reserve_relay(relay_node.local_peer()));
   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));

   const auto routing_before = source.routing_status(content_swarm_test_dht);
   const auto metrics_before = source.metrics();
   const auto target_before = source.peers().find(target.local_peer());
   BOOST_REQUIRE(target_before.has_value());
   BOOST_REQUIRE(routing_before.active > 0U);

   auto stream = forge::asio::blocking::run(
       runtime, source.async_open_protocol_stream(target.local_peer(), builtins::echo,
                                                  node::open_options{
                                                      .allow_relay = true,
                                                      .relay_peer = relay_node.local_peer(),
                                                      .relay_attempt_timeout = std::chrono::milliseconds{2'000},
                                                      .allow_hole_punch = false,
                                                  }));
   const auto payload = std::vector<std::uint8_t>{'r', 'e', 'l', 'a', 'y', '-', 'o', 'n', 'l', 'y'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());

   const auto metrics_after = source.metrics();
   const auto target_after = source.peers().find(target.local_peer());
   BOOST_REQUIRE(target_after.has_value());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(metrics_after.path_direct_attempts == metrics_before.path_direct_attempts);
   BOOST_TEST(metrics_after.direct_failures == metrics_before.direct_failures);
   BOOST_TEST(target_after->failures == target_before->failures);
   BOOST_TEST(source.routing_status(content_swarm_test_dht).active == routing_before.active);

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_relay_transport_opens_arbitrary_registered_protocol) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto protocol = protocol_id{.value = "/product/relay-arbitrary/1"};
   const auto relay_identity = make_test_certificate_identity("relay-arbitrary-protocol-relay");
   const auto source_identity = make_test_certificate_identity("relay-arbitrary-protocol-source");
   const auto target_identity = make_test_certificate_identity("relay-arbitrary-protocol-target");
   auto relay_node = node{
       runtime, options_for(relay_identity, capability_set{.bits = capabilities::direct_quic | capabilities::relay |
                                                                   capabilities::relay_reservation})};
   auto source = node{runtime, options_for(source_identity, capability_set{.bits = capabilities::direct_quic})};
   auto target = node{runtime, options_for(target_identity, capability_set{.bits = capabilities::direct_quic |
                                                                                   capabilities::relay_reservation})};
   auto observed_session = std::make_shared<std::optional<node::session_info>>();
   auto observed_authentication = std::make_shared<std::optional<peer_authentication>>();
   auto observed_session_mutex = std::make_shared<std::mutex>();
   target.register_protocol_handler(protocol,
                                    [observed_session, observed_authentication, observed_session_mutex](
                                        node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                       {
                                          auto lock = std::scoped_lock{*observed_session_mutex};
                                          if (!observed_session->has_value()) {
                                             observed_session->emplace(incoming.session);
                                             observed_authentication->emplace(incoming.stream.authentication());
                                          }
                                       }
                                       auto payload = co_await incoming.stream.async_read_frame();
                                       co_await incoming.stream.async_write_frame(payload);
                                       co_await incoming.stream.async_close();
                                    });

   const auto relay_endpoint = listen(relay_node, runtime);
   (void)listen(target, runtime);
   source.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   target.peers().learn_endpoint(
       relay_node.local_peer(), relay_endpoint,
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   (void)forge::asio::blocking::run(runtime, target.async_reserve_relay(relay_node.local_peer()));

   const auto open_options = node::open_options{
       .allow_relay = true,
       .relay_peer = relay_node.local_peer(),
       .direct_attempt_timeout = std::chrono::milliseconds{100},
       .relay_attempt_timeout = std::chrono::milliseconds{2'000},
       .allow_hole_punch = false,
   };
   auto stream = forge::asio::blocking::run(
       runtime, source.async_open_protocol_stream(target.local_peer(), protocol, open_options));
   const auto payload = std::vector<std::uint8_t>{'p', 'r', 'o', 'd', 'u', 'c', 't'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   forge::asio::blocking::run(runtime, stream.async_close());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(relay_node.metrics().relays_opened >= 1U);
   BOOST_TEST(source.metrics().path_relay_opens >= 1U);
   auto source_session = std::optional<node::session_info>{};
   auto source_authentication = std::optional<peer_authentication>{};
   {
      auto lock = std::scoped_lock{*observed_session_mutex};
      source_session = *observed_session;
      source_authentication = *observed_authentication;
   }
   BOOST_REQUIRE(source_session.has_value());
   BOOST_REQUIRE(source_authentication.has_value());
   BOOST_TEST(source_session->remote_peer.value == source.local_peer().value);
   BOOST_TEST(static_cast<std::uint8_t>(*source_authentication) ==
              static_cast<std::uint8_t>(peer_authentication::noise));

   const auto relay_session_count = [](const node& value, const peer_id& remote, const peer_id& relay,
                                       diagnostics::session_direction direction, bool require_identified) {
      return std::ranges::count_if(value.diagnostics().sessions, [&](const diagnostics::session& session) {
         return !session.closed && session.remote_peer == remote && session.path == path::kind::relay &&
                session.relay_peer == relay && session.direction == direction &&
                (!require_identified || session.identify_state == identify::state::identified);
      });
   };
   auto relay_sessions_identified = false;
   for (auto attempt = 0U; attempt < 200U && !relay_sessions_identified; ++attempt) {
      relay_sessions_identified = relay_session_count(source, target.local_peer(), relay_node.local_peer(),
                                                      diagnostics::session_direction::outbound, true) == 1 &&
                                  relay_session_count(target, source.local_peer(), relay_node.local_peer(),
                                                      diagnostics::session_direction::inbound, true) == 1;
      if (!relay_sessions_identified) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relayed session Identify");
      }
   }
   BOOST_REQUIRE(relay_sessions_identified);

   auto second_stream = forge::asio::blocking::run(
       runtime, source.async_open_protocol_stream(target.local_peer(), protocol, open_options));
   const auto second_payload = std::vector<std::uint8_t>{'r', 'e', 'u', 's', 'e'};
   forge::asio::blocking::run(runtime, second_stream.async_write_frame(second_payload));
   const auto second_reply = forge::asio::blocking::run(runtime, second_stream.async_read_frame());
   forge::asio::blocking::run(runtime, second_stream.async_close());

   BOOST_TEST(second_reply == second_payload, boost::test_tools::per_element());
   BOOST_TEST(relay_session_count(source, target.local_peer(), relay_node.local_peer(),
                                  diagnostics::session_direction::outbound, false) == 1);
   BOOST_TEST(relay_session_count(target, source.local_peer(), relay_node.local_peer(),
                                  diagnostics::session_direction::inbound, false) == 1);

   const auto pushed_protocol = protocol_id{.value = "/product/relay-identify-push/1"};
   register_echo(source, pushed_protocol);
   auto push_observed = false;
   for (auto attempt = 0U; attempt < 200U && !push_observed; ++attempt) {
      const auto source_record = target.peers().find(source.local_peer());
      push_observed = source_record && std::ranges::contains(source_record->protocols, pushed_protocol);
      if (!push_observed) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relayed Identify Push add");
      }
   }
   BOOST_REQUIRE(push_observed);

   BOOST_REQUIRE(source.unregister_protocol_handler(pushed_protocol));
   auto removal_observed = false;
   for (auto attempt = 0U; attempt < 200U && !removal_observed; ++attempt) {
      const auto source_record = target.peers().find(source.local_peer());
      removal_observed = source_record && !std::ranges::contains(source_record->protocols, pushed_protocol);
      if (!removal_observed) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "relayed Identify Push remove");
      }
   }
   BOOST_REQUIRE(removal_observed);

   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_document_roundtrips_libp2p_fields) {
   const auto id = peer(74);
   auto doc = identify::document{
       .protocol_version = "/forge/test/1",
       .agent_version = "forge-test/1",
       .public_key = std::vector<std::uint8_t>{1, 2, 3},
       .listen_endpoints =
           {forge::multiformats::multiaddr::parse("/dnsaddr/identify.example/p2p/" + id.to_string())},
       .observed_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/5001/quic-v1/p2p/" + id.to_string()),
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify},
       .signed_peer_record = std::vector<std::uint8_t>{9, 8, 7},
   };

   auto decoded = identify::decode(identify::encode(doc));

   BOOST_TEST(decoded.protocol_version == doc.protocol_version);
   BOOST_TEST(decoded.agent_version == doc.agent_version);
   BOOST_TEST(decoded.public_key == doc.public_key, boost::test_tools::per_element());
   BOOST_REQUIRE_EQUAL(decoded.listen_endpoints.size(), 1U);
   BOOST_TEST(decoded.listen_endpoints.front().to_string() == doc.listen_endpoints.front().to_string());
   BOOST_REQUIRE(decoded.observed_endpoint.has_value());
   BOOST_TEST(decoded.observed_endpoint->to_string() == doc.observed_endpoint->to_string());
   BOOST_REQUIRE_EQUAL(decoded.protocols.size(), 2U);
   BOOST_TEST(decoded.protocols.front().value == builtins::ping.value);
   BOOST_TEST(decoded.signed_peer_record == doc.signed_peer_record, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(p2p_identify_ignores_malformed_optional_observed_address) {
   auto encoded = identify::encode(identify::document{
       .protocol_version = "ipfs/0.1.0",
       .agent_version = "forge/test",
       .protocols = {builtins::ping},
   });
   const auto malformed_observed = std::array<std::uint8_t, 3>{0x22, 0x01, 0xff};
   encoded.insert(encoded.end(), malformed_observed.begin(), malformed_observed.end());

   const auto decoded = identify::decode(encoded);
   BOOST_TEST(decoded.protocol_version == "ipfs/0.1.0");
   BOOST_TEST(decoded.agent_version == "forge/test");
   BOOST_REQUIRE_EQUAL(decoded.protocols.size(), 1U);
   BOOST_TEST(decoded.protocols.front().value == builtins::ping.value);
   BOOST_TEST(!decoded.observed_endpoint.has_value());
   BOOST_TEST(!decoded.present.observed_endpoint);
}

BOOST_AUTO_TEST_CASE(p2p_direct_nodes_negotiate_protocol_and_echo_frames) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(2))};
   auto client = node{runtime, options_for(peer(1))};
   register_echo(server);

   const auto server_endpoint = listen(server, runtime);
   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(session.remote_peer.value == server.local_peer().value);

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo));
   const auto payload = std::vector<std::uint8_t>{'p', '2', 'p'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(client.metrics().protocol_streams_opened >= 1U);
   BOOST_TEST(server.metrics().protocol_streams_accepted >= 1U);
   BOOST_TEST(active_stream_count(client.diagnostics().resources) == 1U);
   forge::asio::blocking::run(runtime, stream.async_close());
   BOOST_TEST(active_stream_count(client.diagnostics().resources) == 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_inbound_stream_saturation_does_not_stop_session_acceptance) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = options_for(peer(249));
   server_options.limits.resources.system.max_streams = 1;
   server_options.limits.resources.peer.max_streams = 1;
   server_options.limits.resources.protocol.max_streams = 1;
   server_options.transport_limits.max_streams_per_connection = 1;
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, options_for(peer(250))};
   register_echo(server);

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   auto identify_streams_closed = false;
   for (auto attempt = 0U; attempt < 100U && !identify_streams_closed; ++attempt) {
      identify_streams_closed = active_stream_count(server.diagnostics().resources) == 0U &&
                                active_stream_count(client.diagnostics().resources) == 0U;
      if (!identify_streams_closed) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "inbound stream Identify cleanup");
      }
   }
   BOOST_REQUIRE(identify_streams_closed);

   for (auto attempt = 0U; attempt < 2U; ++attempt) {
      auto stream =
          forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo));
      const auto payload = std::vector<std::uint8_t>{static_cast<std::uint8_t>(attempt + 1U)};
      forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
      BOOST_TEST(forge::asio::blocking::run(runtime, stream.async_read_frame()) == payload,
                 boost::test_tools::per_element());
      forge::asio::blocking::run(runtime, stream.async_close());
   }

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_queued_byte_budget_is_held_until_quic_ack) {
   auto server_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{server_runtime, options_for(peer(247))};
   auto client_options = options_for(peer(248));
   constexpr auto payload_size = std::uint64_t{32 * 1024};
   constexpr auto high_priority_denominator = std::uint64_t{204};
   constexpr auto memory_priority_scale = std::uint64_t{256};
   client_options.limits.resources.stream.max_memory =
       (payload_size * memory_priority_scale + high_priority_denominator - 1) / high_priority_denominator;
   client_options.identify.max_total_message_size = client_options.identify.max_message_size;
   auto client = node{client_runtime, std::move(client_options)};
   register_echo(server);

   const auto server_endpoint = listen(server, server_runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   static_cast<void>(forge::asio::blocking::run(
       client_runtime,
       client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   auto stream = forge::asio::blocking::run(
       client_runtime,
       client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                         node::open_options{.allow_relay = false, .allow_hole_punch = false}));

   auto release_server = block_runtime(server_runtime, "P2P queued-byte ACK barrier");
   const auto first = std::vector<std::uint8_t>(payload_size, 0x31);
   forge::asio::blocking::run(client_runtime, stream.async_write(first));
   BOOST_TEST(client.diagnostics().resources.streams.memory == first.size());

   try {
      const auto extra = std::vector<std::uint8_t>{0x32};
      forge::asio::blocking::run(client_runtime, stream.async_write(extra));
      BOOST_FAIL("expected P2P node queued-byte rejection before transport enqueue");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }
   BOOST_TEST(client.diagnostics().resources.streams.memory == first.size());

   release_server->set_value();
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (client.diagnostics().resources.streams.memory != 0) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < deadline);
      wait_on_runtime(client_runtime, std::chrono::milliseconds{5}, "P2P queued-byte ACK release");
   }

   try {
      const auto oversized = std::vector<std::uint8_t>(16 * 1024 * 1024 + 1, 0x33);
      forge::asio::blocking::run(client_runtime, stream.async_write_frame(oversized));
      BOOST_FAIL("expected P2P byte admission before framed payload allocation");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }
   BOOST_TEST(client.diagnostics().resources.streams.memory == 0U);

   stream.cancel();
   forge::asio::blocking::run(client_runtime, client.async_stop());
   forge::asio::blocking::run(server_runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_api_wire_v2_duplex_streams_over_direct_quic) {
   run_live_api_over(endpoint::protocol_kind::quic_v1);
}

BOOST_AUTO_TEST_CASE(p2p_api_wire_v2_duplex_streams_over_tcp_yamux) {
   run_live_api_over(endpoint::protocol_kind::tcp);
}

BOOST_AUTO_TEST_CASE(p2p_api_binding_rejects_unverified_quic_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(make_test_certificate_identity("api-unverified-server"))};
   auto client = node{runtime, options_for(make_test_certificate_identity("api-unverified-client"))};
   auto implementation = std::make_shared<live_impl>();
   auto registry = forge::api::core::registry{};
   registry.install<live_api>(live_api::describe(), implementation);
   auto binding = forge::api::p2p::api(server).use(forge::api::core::binding().serve(registry).build()).build();
   server.register_protocol_handler(binding.protocol(), binding.handler());

   const auto server_endpoint = listen(server, runtime);
   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{
                                                          .expected_peer = server.local_peer(),
                                                          .allow_relay = false,
                                                      }));
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), binding.protocol(),
                                                                             node::open_options{.allow_relay = false}));
   BOOST_TEST(static_cast<std::uint8_t>(stream.authentication()) ==
              static_cast<std::uint8_t>(peer_authentication::unverified));
   auto connection = forge::api::transport::connection{std::move(stream).into_transport_stream(), binding.options()};
   forge::asio::blocking::run(runtime, exercise_unverified_live_api(connection));
   BOOST_TEST(implementation->calls.load(std::memory_order_acquire) == 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_authenticated_peer_ignores_spoofed_legacy_metadata) {
   const auto authenticated = peer(243);
   const auto spoofed = peer(244);
   const auto legacy_metadata = forge::api::core::metadata{
       forge::api::core::metadata_entry{
           .key = "forge.p2p.remote_peer",
           .value = spoofed.to_string(),
       },
   };
   const auto authenticated_fingerprint = forge::crypto::digest::sha256::hash(authenticated.to_bytes());
   const auto trusted = forge::api::core::trusted_invocation_builder{}
                            .set(forge::api::p2p::authenticated_peer{.id = authenticated})
                            .set(forge::api::auth::authenticated_caller{
                                forge::api::auth::caller_source::p2p_peer,
                                authenticated_fingerprint,
                            })
                            .build();
   auto value = p2p_live_types::caller_peer{.id = spoofed};

   auto traversed = std::tuple{value};
   forge::api::core::reset_server_supplied(traversed);
   BOOST_TEST(std::get<0>(traversed).id.value.empty());
   forge::api::core::apply_server_supplied(traversed, trusted);
   BOOST_TEST(std::get<0>(traversed).id.value == authenticated.value);

   auto api_fields = std::tuple{
       value,
       forge::api::auth::authenticated_caller{
           forge::api::auth::caller_source::tls_certificate,
           forge::crypto::digest::sha256::hash(spoofed.to_bytes()),
       },
   };
   const auto descriptor = p2p_live_types::live_api::describe();
   const auto* method = forge::api::core::find_method(descriptor, "exchange");
   BOOST_REQUIRE(method != nullptr);
   const auto* fields = forge::api::core::detail::server_fields_for(*method);
   BOOST_REQUIRE(fields != nullptr);
   fields->reset_fixed(&api_fields);
   BOOST_TEST(std::get<0>(api_fields).id.value.empty());
   BOOST_TEST(std::get<1>(api_fields).fingerprint.empty());
   fields->apply_fixed(&api_fields, trusted);
   BOOST_TEST(std::get<0>(api_fields).id.value == authenticated.value);
   BOOST_TEST(static_cast<std::uint8_t>(std::get<1>(api_fields).source) ==
              static_cast<std::uint8_t>(forge::api::auth::caller_source::p2p_peer));
   BOOST_TEST(std::get<1>(api_fields).fingerprint == authenticated_fingerprint);
   BOOST_TEST(std::get<1>(api_fields).transport_authenticated());

   forge::api::core::server_supplied<p2p_live_types::caller_peer>::reset(value);
   BOOST_REQUIRE(forge::api::core::server_supplied<p2p_live_types::caller_peer>::apply(value, trusted));
   BOOST_TEST(legacy_metadata.front().value == spoofed.to_string());
   BOOST_TEST(value.id.value == authenticated.value);
   BOOST_TEST(value.id.value != spoofed.value);
}

BOOST_AUTO_TEST_CASE(p2p_direct_quic_uses_endpoint_peer_when_expected_peer_is_absent) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = options_for(make_test_certificate_identity("quic-endpoint-peer-server"));
   auto client_options = options_for(make_test_certificate_identity("quic-endpoint-peer-client"));
   server_options.allow_insecure_test_mode = false;
   client_options.allow_insecure_test_mode = false;
   server_options.peer_state.persistence = peer_store::make_memory_persistence();
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   auto server_endpoint = listen(server, runtime);
   server_endpoint.peer = server.local_peer();
   const auto session = forge::asio::blocking::run(runtime, client.async_connect(server_endpoint));

   BOOST_TEST(session.remote_peer.value == server.local_peer().value);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_quic_rejects_endpoint_peer_mismatch) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = options_for(make_test_certificate_identity("quic-endpoint-peer-mismatch-server"));
   auto client_options = options_for(make_test_certificate_identity("quic-endpoint-peer-mismatch-client"));
   server_options.allow_insecure_test_mode = false;
   client_options.allow_insecure_test_mode = false;
   server_options.peer_state.persistence = peer_store::make_memory_persistence();
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   auto server_endpoint = listen(server, runtime);
   server_endpoint.peer = peer(150);
   try {
      (void)forge::asio::blocking::run(runtime, client.async_connect(server_endpoint));
      BOOST_FAIL("expected QUIC endpoint peer mismatch");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::peer_verification_failed));
   }

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_quic_explicit_expected_peer_matches_certificate_peer_id) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = options_for(make_test_certificate_identity("quic-expected-peer-server"));
   auto client_options = options_for(make_test_certificate_identity("quic-expected-peer-client"));
   server_options.allow_insecure_test_mode = false;
   client_options.allow_insecure_test_mode = false;
   server_options.peer_state.persistence = peer_store::make_memory_persistence();
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   const auto server_endpoint = listen(server, runtime);
   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   BOOST_TEST(session.remote_peer.value == server.local_peer().value);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_quic_rejects_missing_certificate_identity_without_expected_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = node::options{
       .certificate_pem = std::string{test_certificate()},
       .private_key_pem = std::string{test_private_key()},
       .explicit_peer_id = legacy_cert_hash_peer_id(test_certificate()),
       .peer_state = peer_store::options{.persistence = peer_store::make_memory_persistence()},
       .allow_insecure_test_mode = true,
   };
   auto client_options = options_for(make_test_certificate_identity("quic-missing-extension-client"));
   client_options.allow_insecure_test_mode = false;
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   const auto server_endpoint = listen(server, runtime);
   try {
      (void)forge::asio::blocking::run(runtime, client.async_connect(server_endpoint));
      BOOST_FAIL("expected missing QUIC certificate identity extension to fail");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::peer_verification_failed));
   }

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_quic_rejects_missing_certificate_identity_with_endpoint_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto legacy_peer = legacy_cert_hash_peer_id(test_certificate());
   auto server_options = node::options{
       .certificate_pem = std::string{test_certificate()},
       .private_key_pem = std::string{test_private_key()},
       .explicit_peer_id = legacy_peer,
       .peer_state = peer_store::options{.persistence = peer_store::make_memory_persistence()},
       .allow_insecure_test_mode = true,
   };
   auto client_options = options_for(make_test_certificate_identity("quic-missing-extension-endpoint-client"));
   client_options.allow_insecure_test_mode = false;
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   auto server_endpoint = listen(server, runtime);
   server_endpoint.peer = legacy_peer;
   try {
      (void)forge::asio::blocking::run(runtime, client.async_connect(server_endpoint));
      BOOST_FAIL("expected missing QUIC certificate identity extension to fail");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::peer_verification_failed));
   }

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_ping_protocol_uses_libp2p_payload_echo) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(72))};
   auto client = node{runtime, options_for(peer(73))};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::ping));
   const auto payload = std::vector<std::uint8_t>(32, 0x42);
   forge::asio::blocking::run(runtime, stream.async_write(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_ping_api_returns_rtt) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(82))};
   auto client = node{runtime, options_for(peer(83))};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   const auto rtt = forge::asio::blocking::run(runtime, client.async_ping(server.local_peer()));
   BOOST_TEST(rtt.count() >= 0);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_protocol_advertises_supported_protocols) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(75))};
   auto client = node{runtime, options_for(peer(76))};
   register_echo(server);

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify));
   const auto payload = forge::asio::blocking::run(runtime, read_length_delimited(stream));
   const auto doc = identify::decode(payload);

   BOOST_TEST(doc.agent_version == "forge/1.0.0");
   BOOST_TEST(std::ranges::any_of(doc.protocols, [](const protocol_id& value) { return value == builtins::ping; }));
   BOOST_TEST(std::ranges::any_of(doc.protocols, [](const protocol_id& value) { return value == builtins::identify; }));
   BOOST_TEST(std::ranges::any_of(doc.protocols, [](const protocol_id& value) { return value == builtins::echo; }));
   BOOST_TEST(!doc.listen_endpoints.empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_verified_identify_dht_advertisement_admits_server_before_exchange) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-dht-candidate-server");
   const auto client_identity = make_test_certificate_identity("identify-dht-candidate-client");
   auto server = node{runtime, options_for(server_identity, capability_set{.bits = capabilities::direct_quic})};
   server.register_protocol_handler(builtins::kad_dht,
                                    [](node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; });
   auto client_options = dht_options_for(client_identity, amino_v1());
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   for (auto attempt = 0U; attempt < 20U && client.routing_status(builtins::kad_dht).active == 0U; ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "Identify DHT protocol fact");
   }
   const auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   BOOST_TEST(
       std::ranges::any_of(record->protocols, [](const auto& protocol) { return protocol == builtins::kad_dht; }));
   BOOST_TEST(client.routing_status(builtins::kad_dht).active == 1U);
   BOOST_TEST(client.routing_status(builtins::kad_dht).candidates == 0U);
   BOOST_TEST(server.metrics().dht_queries == 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_discovery_candidate_probes_share_one_overall_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client_options = dht_options_for(make_test_identity(), amino_v1());
   client_options.limits.topology.max_parallel_queries = 4;
   client_options.limits.topology.query_timeout = std::chrono::milliseconds{75};
   client_options.limits.topology.peer_exchange_enabled = false;
   auto client = node{runtime, std::move(client_options)};
   for (const auto value : {22U, 23U, 24U, 25U}) {
      const auto candidate = peer(static_cast<std::uint8_t>(value));
      auto endpoint = start_stalling_tcp_peer(runtime);
      endpoint.peer = candidate;
      client.peers().upsert_routing_peer(
          builtins::kad_dht,
          dht::peer{.id = candidate,
                    .endpoints = addresses_for({endpoint}),
                    .connection = dht::connection_type::can_connect},
          discovery::source::explicit_config, std::chrono::system_clock::now() + std::chrono::hours{1});
   }
   forge::asio::blocking::run(runtime, client.peers().async_flush());
   forge::asio::blocking::run(runtime, client.async_hydrate_peer_state());
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));

   const auto started = std::chrono::steady_clock::now();
   try {
      (void)forge::asio::blocking::run(runtime, client.async_refresh_discovery());
      BOOST_FAIL("expected bounded discovery timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   BOOST_TEST(elapsed.count() < 225);

   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_discovery_tcp_candidate_timeout_does_not_poison_parent_cancellation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 3}};
   const auto client_peer = peer(240);
   const auto target = make_dht_key(client_peer);
   const auto first_candidate = peer(238);
   const auto second_candidate = peer(239);
   const auto closer_to_target = [&](const peer_id& left, const peer_id& right) {
      const auto left_distance = distance_between(left.to_bytes(), target.bytes);
      const auto right_distance = distance_between(right.to_bytes(), target.bytes);
      return left_distance != right_distance ? left_distance < right_distance : left < right;
   };
   const auto stalled_peer = closer_to_target(first_candidate, second_candidate) ? first_candidate : second_candidate;
   const auto donor_peer = stalled_peer == first_candidate ? second_candidate : first_candidate;
   const auto overall_timeout = std::chrono::seconds{5};

   auto donor = node{runtime, dht_options_for(donor_peer, amino_v1(dht::mode::server))};
   const auto donor_endpoint = listen(donor, runtime);
   auto client_options = dht_options_for(client_peer, amino_v1());
   client_options.limits.topology.max_parallel_queries = 1;
   client_options.limits.topology.query_timeout = overall_timeout;
   client_options.limits.topology.peer_exchange_enabled = false;
   client_options.limits.topology.rendezvous_enabled = false;
   auto client = node{runtime, std::move(client_options)};

   auto stalled_accepted = std::make_shared<std::promise<void>>();
   auto stalled_accepted_future = stalled_accepted->get_future();
   auto stalled_endpoint = start_stalling_tcp_peer(runtime, std::chrono::seconds{5}, stalled_accepted);
   stalled_endpoint.peer = stalled_peer;
   client.peers().upsert_routing_peer(builtins::kad_dht,
                                      dht::peer{.id = stalled_peer,
                                                .endpoints = addresses_for({stalled_endpoint}),
                                                .connection = dht::connection_type::can_connect},
                                      discovery::source::explicit_config,
                                      std::chrono::system_clock::now() + std::chrono::hours{1});
   client.peers().upsert_routing_peer(builtins::kad_dht,
                                      dht::peer{.id = donor.local_peer(),
                                                .endpoints = addresses_for({donor_endpoint}),
                                                .connection = dht::connection_type::can_connect},
                                      discovery::source::explicit_config,
                                      std::chrono::system_clock::now() + std::chrono::hours{1});
   forge::asio::blocking::run(runtime, client.peers().async_flush());
   forge::asio::blocking::run(runtime, client.async_hydrate_peer_state());

   const auto donor_queries_before = donor.metrics().dht_queries;
   const auto started = std::chrono::steady_clock::now();
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   const auto discovered = forge::asio::blocking::run(runtime, client.async_refresh_discovery());
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

   BOOST_REQUIRE(stalled_accepted_future.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   BOOST_TEST(donor.metrics().dht_queries > donor_queries_before);
   BOOST_TEST(std::ranges::any_of(discovered,
                                  [&](const discovery::result& value) { return value.peer == donor.local_peer(); }));
   BOOST_TEST(elapsed.count() < std::chrono::duration_cast<std::chrono::milliseconds>(overall_timeout).count());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, donor.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_dht_profiles_isolate_a_timed_out_profile) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 3}};
   const auto slow_protocol = protocol_id{.value = "/forge/test/slow-topology/kad/1.0.0"};
   const auto fast_protocol = protocol_id{.value = "/forge/test/fast-topology/kad/1.0.0"};
   auto limits = dht::options{};
   limits.alpha = 2;
   const auto slow_profile =
       custom_dht_profile(slow_protocol, dht::mode::client,
                          dht::profile_capabilities{.peers = true, .providers = false, .values = false}, {}, limits);
   const auto fast_client_profile =
       custom_dht_profile(fast_protocol, dht::mode::client,
                          dht::profile_capabilities{.peers = true, .providers = false, .values = false}, {}, limits);
   const auto fast_server_profile =
       custom_dht_profile(fast_protocol, dht::mode::server,
                          dht::profile_capabilities{.peers = true, .providers = false, .values = false}, {}, limits);
   auto server = node{runtime, dht_options_for(peer(239), fast_server_profile)};
   auto client_options = dht_options_for(peer(240), slow_profile);
   client_options.dht_profiles.push_back(fast_client_profile);
   client_options.limits.topology.query_timeout = std::chrono::milliseconds{75};
   client_options.limits.topology.max_parallel_queries = 2;
   client_options.limits.topology.peer_exchange_enabled = false;
   client_options.limits.topology.rendezvous_enabled = false;
   auto client = node{runtime, std::move(client_options)};

   const auto server_endpoint = listen(server, runtime);
   client.peers().upsert_routing_peer(fast_protocol,
                                      dht::peer{
                                          .id = server.local_peer(),
                                          .endpoints = addresses_for({server_endpoint}),
                                          .connection = dht::connection_type::can_connect,
                                      },
                                      discovery::source::explicit_config,
                                      std::chrono::system_clock::now() + std::chrono::hours{1});
   auto stalled_endpoint = start_stalling_tcp_peer(runtime, std::chrono::seconds{2});
   const auto stalled_peer = peer(241);
   stalled_endpoint.peer = stalled_peer;
   client.peers().upsert_routing_peer(slow_protocol,
                                      dht::peer{
                                          .id = stalled_peer,
                                          .endpoints = addresses_for({stalled_endpoint}),
                                          .connection = dht::connection_type::can_connect,
                                      },
                                      discovery::source::explicit_config,
                                      std::chrono::system_clock::now() + std::chrono::hours{1});
   forge::asio::blocking::run(runtime, client.peers().async_flush());
   forge::asio::blocking::run(runtime, client.async_hydrate_peer_state());

   const auto fast_queries_before = server.metrics().dht_queries;
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_refresh_discovery()));
   BOOST_TEST(server.metrics().dht_queries > fast_queries_before);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_hydrated_candidate_bootstraps_lookup_without_refresh) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = dht_options_for(peer(239), amino_v1(dht::mode::server));
   auto server = node{runtime, std::move(server_options)};
   const auto server_endpoint = listen(server, runtime);

   auto persistence = peer_store::make_memory_persistence();
   forge::asio::blocking::run(runtime,
                              persistence->async_apply(peer_store::mutation_batch{
                                  .peer_upserts = std::vector<peer_store::record>{peer_store::record{
                                      .peer = server.local_peer(),
                                      .discovered_by = discovery::source::explicit_config,
                                      .protocols = std::vector<protocol_id>{builtins::kad_dht},
                                      .endpoints = std::vector<peer_store::endpoint_record>{peer_store::endpoint_record{
                                          .address = address_for(server_endpoint)}},
                                      .discovery_expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                                  }},
                              }));
   auto client_options = dht_options_for(peer(240), amino_v1());
   client_options.peer_state.persistence = persistence;
   auto client = node{runtime, std::move(client_options)};
   forge::asio::blocking::run(runtime, client.async_hydrate_peer_state());

   BOOST_TEST(client.routing_status(builtins::kad_dht).active == 0U);
   BOOST_TEST(client.routing_status(builtins::kad_dht).candidates == 1U);
   const auto found =
       forge::asio::blocking::run(runtime, client.async_find_peer(builtins::kad_dht, server.local_peer()));
   BOOST_TEST(found.complete);
   BOOST_TEST(std::ranges::any_of(found.closest_peers,
                                  [&](const dht::peer& value) { return value.id == server.local_peer(); }));
   BOOST_TEST(server.metrics().dht_queries >= 1U);
   BOOST_TEST(client.routing_status(builtins::kad_dht).active == 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_put_value_local_store_does_not_satisfy_remote_quorum) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto local = node{runtime, dht_options_for(peer(62), custom_test_value_dht_profile())};
   const auto key = dht::key{.bytes = {'/', 'l', 'o', 'c', 'a', 'l'}};
   const auto value = std::vector<std::uint8_t>{'v', 'a', 'l', 'u', 'e'};

   auto pending = boost::asio::co_spawn(
       runtime.context(),
       local.async_put_value(content_swarm_value_test_dht, dht::record{.key_value = key, .value = value},
                             dht::query_options{.quorum = 1, .timeout = std::chrono::milliseconds{250}}),
       boost::asio::use_future);
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = pending.get();

   BOOST_TEST(result.selected.key_value.bytes == key.bytes, boost::test_tools::per_element());
   BOOST_TEST(result.selected.value == value, boost::test_tools::per_element());
   BOOST_TEST(result.accepted == 0U);
   BOOST_TEST(result.attempted == 0U);
   BOOST_TEST(!result.quorum_reached);

   auto read_pending = boost::asio::co_spawn(
       runtime.context(),
       local.async_get_value(content_swarm_value_test_dht, key,
                             dht::query_options{.quorum = 1, .timeout = std::chrono::milliseconds{250}}),
       boost::asio::use_future);
   BOOST_REQUIRE(read_pending.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto stored = read_pending.get();
   BOOST_REQUIRE(stored.selected.has_value());
   BOOST_TEST(stored.selected->value == value, boost::test_tools::per_element());
   BOOST_TEST(stored.valid_records == 1U);
   BOOST_TEST(stored.quorum_reached);

   forge::asio::blocking::run(runtime, local.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_get_value_reports_remaining_record_ttl) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("dht-value-ttl-server");
   const auto client_identity = make_test_certificate_identity("dht-value-ttl-client");
   const auto reader_identity = make_test_certificate_identity("dht-value-ttl-reader");
   const auto server_profile = custom_test_value_dht_profile(dht::mode::server);
   const auto client_profile = custom_test_value_dht_profile();
   auto server = node{runtime, dht_options_for(server_identity, server_profile)};
   auto client = node{runtime, dht_options_for(client_identity, client_profile)};
   const auto server_endpoint = listen(server, runtime);
   verify_dht_server(runtime, client, server, server_endpoint, content_swarm_value_test_dht);

   const auto key = make_dht_key(std::vector<std::uint8_t>{'/', 't', 't', 'l'});
   const auto put = forge::asio::blocking::run(
       runtime, client.async_put_value(content_swarm_value_test_dht,
                                       dht::record{.key_value = key, .value = {'v'}, .ttl = std::chrono::seconds{10}}));
   BOOST_REQUIRE(put.quorum_reached);
   wait_on_runtime(runtime, std::chrono::milliseconds{1'100}, "DHT value TTL aging");

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), content_swarm_value_test_dht));
   forge::asio::blocking::run(
       runtime, stream.async_write(dht::codec::encode(
                    dht::message{.type = dht::message_type::get_value, .key_value = key}, client_profile)));
   const auto response = dht::codec::decode(
       wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream))), client_profile);
   BOOST_REQUIRE(response.record_value.has_value());
   BOOST_TEST(response.record_value->ttl > std::chrono::seconds::zero());
   BOOST_TEST(response.record_value->ttl < std::chrono::seconds{10});
   forge::asio::blocking::run(runtime, stream.async_close());

   auto reader_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto reader_options = dht_options_for(reader_identity, client_profile);
   reader_options.dht_record_persistence.emplace(client_profile.protocol, reader_persistence);
   auto reader = node{runtime, std::move(reader_options)};
   forge::asio::blocking::run(runtime, reader.async_hydrate_peer_state());
   verify_dht_server(runtime, reader, server, server_endpoint, content_swarm_value_test_dht);
   const auto read_started = std::chrono::system_clock::now();
   const auto selected = forge::asio::blocking::run(
       runtime, reader.async_get_value(content_swarm_value_test_dht, key,
                                       dht::query_options{.requested_count = 1, .quorum = 1}));
   BOOST_REQUIRE(selected.quorum_reached);
   BOOST_REQUIRE(selected.selected.has_value());
   BOOST_TEST(selected.selected->value == std::vector<std::uint8_t>{'v'}, boost::test_tools::per_element());

   const auto persisted =
       forge::asio::blocking::run(runtime, reader_persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::values,
                                               .limit = 2,
                                           }));
   BOOST_REQUIRE_EQUAL(persisted.values.size(), 1U);
   BOOST_TEST(persisted.values.front().expires_at > read_started);
   BOOST_TEST(persisted.values.front().expires_at <= read_started + std::chrono::seconds{10});

   forge::asio::blocking::run(runtime, reader.async_stop());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_get_value_propagates_local_persistence_failures) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("dht-value-persistence-server");
   const auto client_identity = make_test_certificate_identity("dht-value-persistence-client");
   const auto server_profile = custom_test_value_dht_profile(dht::mode::server);
   const auto client_profile = custom_test_value_dht_profile();
   const auto key = make_dht_key(std::vector<std::uint8_t>{'/', 'd', 'u', 'r', 'a', 'b', 'l', 'e'});
   const auto value = dht::record{.key_value = key, .value = {'v'}, .ttl = std::chrono::minutes{5}};

   auto server_persistence = dht::record_store::make_memory_persistence();
   auto seed = dht::record_store::mutation_batch{};
   seed.value_upserts.push_back(dht::record_store::value_record{
       .record = value,
       .expires_at = std::chrono::system_clock::now() + std::chrono::minutes{5},
   });
   static_cast<void>(forge::asio::blocking::run(runtime, server_persistence->async_apply(std::move(seed))));

   auto server_options = dht_options_for(server_identity, server_profile);
   server_options.dht_record_persistence.emplace(server_profile.protocol, server_persistence);
   auto client_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto client_options = dht_options_for(client_identity, client_profile);
   client_options.dht_record_persistence.emplace(client_profile.protocol, client_persistence);
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   forge::asio::blocking::run(runtime, server.async_hydrate_peer_state());
   forge::asio::blocking::run(runtime, client.async_hydrate_peer_state());
   const auto server_endpoint = listen(server, runtime);
   verify_dht_server(runtime, client, server, server_endpoint, content_swarm_value_test_dht);

   client_persistence->reject_next_value_upsert.store(true, std::memory_order_relaxed);
   try {
      static_cast<void>(forge::asio::blocking::run(
          runtime, client.async_get_value(content_swarm_value_test_dht, key,
                                          dht::query_options{.requested_count = 1, .quorum = 1})));
      BOOST_FAIL("expected local DHT value persistence backpressure");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(exceptions::is(error, exceptions::code::backpressure_rejected));
   }
   const auto degraded = client.diagnostics();
   const auto degraded_profile = std::ranges::find(degraded.dht_profiles, content_swarm_value_test_dht,
                                                   &diagnostics::dht_profile_state::protocol);
   BOOST_REQUIRE(degraded_profile != degraded.dht_profiles.end());
   BOOST_TEST(degraded_profile->persistence_degraded);

   client_persistence->uncertain_next_value_upsert.store(true, std::memory_order_relaxed);
   try {
      static_cast<void>(forge::asio::blocking::run(
          runtime, client.async_get_value(content_swarm_value_test_dht, key,
                                          dht::query_options{.requested_count = 1, .quorum = 1})));
      BOOST_FAIL("expected uncertain local DHT value durability");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(exceptions::is(error, exceptions::code::durability_uncertain));
   }
   const auto persisted =
       forge::asio::blocking::run(runtime, client_persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::values,
                                               .limit = 1,
                                           }));
   BOOST_REQUIRE_EQUAL(persisted.values.size(), 1U);
   BOOST_TEST(persisted.values.front().record.value == value.value, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_get_value_skips_rejected_record_and_reaches_quorum) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.replication = 2;
   limits.alpha = 1;
   const auto client_profile = custom_test_value_dht_profile(dht::mode::client, limits);
   const auto valid_server_profile = custom_test_value_dht_profile(dht::mode::server, limits);
   auto invalid_server_profile = valid_server_profile;
   invalid_server_profile.value_policies.front().validate = [](const dht::record&, dht::value_validation_context) {};
   const auto first_identity = make_test_certificate_identity("dht-rejected-value-first");
   const auto second_identity = make_test_certificate_identity("dht-rejected-value-second");
   const auto client_identity = make_test_certificate_identity("dht-rejected-value-client");
   const auto key = make_dht_key(std::vector<std::uint8_t>{'/', 'r', 'e', 'j', 'e', 'c', 't'});
   const auto ordered = dht_query::sorted_peers(
       std::map<peer_id, dht::peer>{{first_identity.peer, dht::peer{.id = first_identity.peer}},
                                    {second_identity.peer, dht::peer{.id = second_identity.peer}}},
       key);
   BOOST_REQUIRE_EQUAL(ordered.size(), 2U);
   const auto* invalid_identity = ordered.front().id == first_identity.peer ? &first_identity : &second_identity;
   const auto* valid_identity = invalid_identity == &first_identity ? &second_identity : &first_identity;

   auto invalid_persistence = dht::record_store::make_memory_persistence();
   auto valid_persistence = dht::record_store::make_memory_persistence();
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::minutes{5};
   auto invalid_seed = dht::record_store::mutation_batch{};
   invalid_seed.value_upserts.push_back(dht::record_store::value_record{
       .record = dht::record{.key_value = key, .value = {}, .ttl = std::chrono::minutes{5}},
       .expires_at = expires_at,
   });
   auto valid_seed = dht::record_store::mutation_batch{};
   valid_seed.value_upserts.push_back(dht::record_store::value_record{
       .record = dht::record{.key_value = key, .value = {'v'}, .ttl = std::chrono::minutes{5}},
       .expires_at = expires_at,
   });
   static_cast<void>(forge::asio::blocking::run(runtime, invalid_persistence->async_apply(std::move(invalid_seed))));
   static_cast<void>(forge::asio::blocking::run(runtime, valid_persistence->async_apply(std::move(valid_seed))));

   auto invalid_options = dht_options_for(*invalid_identity, invalid_server_profile);
   invalid_options.dht_record_persistence.emplace(invalid_server_profile.protocol, invalid_persistence);
   auto valid_options = dht_options_for(*valid_identity, valid_server_profile);
   valid_options.dht_record_persistence.emplace(valid_server_profile.protocol, valid_persistence);
   auto invalid_server = node{runtime, std::move(invalid_options)};
   auto valid_server = node{runtime, std::move(valid_options)};
   auto client = node{runtime, dht_options_for(client_identity, client_profile)};
   forge::asio::blocking::run(runtime, invalid_server.async_hydrate_peer_state());
   forge::asio::blocking::run(runtime, valid_server.async_hydrate_peer_state());
   forge::asio::blocking::run(runtime, client.async_hydrate_peer_state());
   const auto invalid_endpoint = listen(invalid_server, runtime);
   const auto valid_endpoint = listen(valid_server, runtime);
   verify_dht_server(runtime, client, invalid_server, invalid_endpoint, content_swarm_value_test_dht);
   verify_dht_server(runtime, client, valid_server, valid_endpoint, content_swarm_value_test_dht);

   const auto result = forge::asio::blocking::run(
       runtime, client.async_get_value(content_swarm_value_test_dht, key,
                                       dht::query_options{.requested_count = 2, .quorum = 1}));
   BOOST_REQUIRE(result.quorum_reached);
   BOOST_REQUIRE(result.selected.has_value());
   BOOST_TEST(result.selected->value == std::vector<std::uint8_t>{'v'}, boost::test_tools::per_element());
   BOOST_TEST(result.responses == 2U);
   BOOST_TEST(result.valid_records == 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, valid_server.async_stop());
   forge::asio::blocking::run(runtime, invalid_server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_positive_wire_ttl_caps_pk_ipns_and_custom_value_expiry) {
   const auto now = std::chrono::system_clock::now();
   const auto wire_ttl = std::chrono::minutes{7};
   const auto amino_profile = amino_v1();
   auto custom_limits = dht::options{};
   custom_limits.value_record_ttl = std::chrono::minutes{3};
   const auto custom_profile = custom_test_value_dht_profile(dht::mode::client, custom_limits);
   for (const auto& key : std::vector<std::vector<std::uint8_t>>{
            {'/', 'p', 'k', '/', 'p'},
            {'/', 'i', 'p', 'n', 's', '/', 'p'},
        }) {
      const auto value = dht::record{.key_value = make_dht_key(key), .value = {'v'}, .ttl = wire_ttl};
      BOOST_TEST(dht_value_expiry(value, now, amino_profile) == now + wire_ttl);
   }
   const auto custom = dht::record{
       .key_value = make_dht_key(std::vector<std::uint8_t>{'/', 'c', 'u', 's', 't', 'o', 'm'}),
       .value = {'v'},
       .ttl = wire_ttl,
   };
   BOOST_TEST(dht_value_expiry(custom, now, custom_profile) == now + custom_limits.value_record_ttl);
   auto without_wire_ttl = custom;
   without_wire_ttl.ttl = std::chrono::seconds::zero();
   BOOST_TEST(dht_value_expiry(without_wire_ttl, now, custom_profile) == now + custom_limits.value_record_ttl);
}

BOOST_AUTO_TEST_CASE(p2p_node_creates_ipns_record_with_its_identity) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto identity = make_test_identity();
   auto local = node{runtime, options_for(identity)};
   const auto value = std::string_view{"/ipfs/bafkqaaa"};
   const auto eol = forge::chrono::timestamp{
       std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::sys_days{std::chrono::year{2030} / 1 / 1})};
   const auto record = local.create_ipns_record(
       std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}, 7, eol,
       std::chrono::minutes{5});

   BOOST_TEST(record.sequence() == 7U);
   BOOST_TEST(std::vector<std::uint8_t>(record.value().begin(), record.value().end()) ==
                  std::vector<std::uint8_t>(value.begin(), value.end()),
              boost::test_tools::per_element());
   ipns::validate(record, local.local_peer(), identity.key,
                  forge::chrono::timestamp{std::chrono::time_point_cast<std::chrono::seconds>(
                      std::chrono::sys_days{std::chrono::year{2029} / 1 / 1})});

   forge::asio::blocking::run(runtime, local.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_amino_ipns_preserves_typed_keybook_failures) {
   const auto identity = make_rsa_identity();
   auto create_options = ipns::create_options{};
   create_options.embed_public_key = false;
   const auto eol = forge::chrono::timestamp{
       std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::sys_days{std::chrono::year{2030} / 1 / 1})};
   const auto value = std::string_view{"/ipfs/bafkqaaa"};
   const auto record = ipns::create(
       identity.key,
       [&identity](std::span<const std::uint8_t> message) { return sign_test_identity(identity, message); },
       std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}, 1, eol,
       std::chrono::minutes{5}, std::move(create_options));
   const auto key = ipns::routing_key(identity.peer);
   const auto profile = amino_v1();
   const auto* policy = value_policy_for(profile, key);
   BOOST_REQUIRE(policy != nullptr);
   const auto resolver = public_key_resolver{[](const peer_id&) -> std::optional<public_key> {
      FORGE_THROW_CODE(exceptions::code::codec_error, "injected KeyBook codec failure");
   }};

   try {
      policy->validate(dht::record{.key_value = dht::key{.bytes = key}, .value = ipns::encode(record)},
                       dht::value_validation_context{.now = std::chrono::sys_days{std::chrono::year{2029} / 1 / 1},
                                                     .public_keys = &resolver});
      BOOST_FAIL("expected typed KeyBook failure");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(exceptions::is(error, exceptions::code::codec_error));
      BOOST_TEST(!exceptions::is(error, exceptions::code::record_rejected));
   }
}

BOOST_AUTO_TEST_CASE(p2p_dht_find_node_returns_exact_peer_store_and_self_before_active_closest) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.replication = 2;
   limits.alpha = 1;
   const auto server_profile = custom_test_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_dht_profile(dht::mode::client, limits);
   const auto server_identity = make_test_certificate_identity("dht-find-node-server");
   const auto active_identity = make_test_certificate_identity("dht-find-node-active");
   const auto client_identity = make_test_certificate_identity("dht-find-node-client");
   auto server_options = dht_options_for(server_identity, server_profile);
   auto active_options = dht_options_for(active_identity, server_profile);
   auto server = node{runtime, std::move(server_options)};
   auto active = node{runtime, std::move(active_options)};
   auto client = node{runtime, dht_options_for(client_identity, client_profile)};
   const auto server_endpoint = listen(server, runtime);
   const auto active_endpoint = listen(active, runtime);
   verify_dht_server(runtime, server, active, active_endpoint, content_swarm_test_dht);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   const auto exact = peer(29);
   auto exact_endpoint = make_dns_tcp_endpoint(4029, "exact-find-node.example.com");
   exact_endpoint.peer = exact;
   server.peers().learn_endpoint(exact, exact_endpoint);

   const auto exact_response =
       exchange_find_node(runtime, client, server.local_peer(), client_profile, make_dht_key(exact));
   BOOST_REQUIRE_EQUAL(exact_response.closer_peers.size(), 2U);
   BOOST_TEST(exact_response.closer_peers.front().id.to_string() == exact.to_string());
   BOOST_REQUIRE_EQUAL(exact_response.closer_peers.front().endpoints.size(), 1U);
   BOOST_TEST(exact_response.closer_peers.front().endpoints.front().to_string() == exact_endpoint.to_string());
   BOOST_TEST(exact_response.closer_peers[1].id.to_string() == active.local_peer().to_string());

   const auto self_response =
       exchange_find_node(runtime, client, server.local_peer(), client_profile, make_dht_key(server.local_peer()));
   BOOST_REQUIRE_EQUAL(self_response.closer_peers.size(), 2U);
   BOOST_TEST(self_response.closer_peers.front().id.to_string() == server.local_peer().to_string());
   BOOST_TEST(!self_response.closer_peers.front().endpoints.empty());
   BOOST_TEST(self_response.closer_peers[1].id.to_string() == active.local_peer().to_string());

   const auto arbitrary_response = exchange_find_node(runtime, client, server.local_peer(), client_profile,
                                                      make_dht_key(std::vector<std::uint8_t>{0xffU}));
   BOOST_REQUIRE_EQUAL(arbitrary_response.closer_peers.size(), 1U);
   BOOST_TEST(arbitrary_response.closer_peers.front().id.to_string() == active.local_peer().to_string());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, active.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_node_finds_peer_and_provider_over_negotiated_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("dht-node-server");
   const auto client_identity = make_test_certificate_identity("dht-node-client");
   const auto target_identity = make_test_certificate_identity("dht-node-target");
   const auto provider_identity = make_test_certificate_identity("dht-node-provider");
   auto server_options = dht_options_for(server_identity, amino_v1(dht::mode::server));
   auto server_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   server_options.dht_record_persistence.emplace(builtins::kad_dht, server_persistence);
   auto client_options = dht_options_for(client_identity, amino_v1());
   auto target_options = dht_options_for(target_identity, amino_v1(dht::mode::server));
   auto provider_endpoint = make_dns_tcp_endpoint(4121, "provider.example.com");
   provider_endpoint.peer = provider_identity.peer;
   auto provider_options = dht_options_for(provider_identity, amino_v1());
   provider_options.advertised_endpoints.push_back(provider_endpoint);
   auto provider_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   provider_options.dht_record_persistence.emplace(builtins::kad_dht, provider_persistence);
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   auto target_node = node{runtime, std::move(target_options)};
   auto provider_node = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   const auto target_listen_endpoint = listen(target_node, runtime);
   static_cast<void>(listen(provider_node, runtime));
   auto target_endpoint = make_dns_tcp_endpoint(4120, "target.example.com");
   target_endpoint.peer = target_node.local_peer();
   verify_dht_server(runtime, server, target_node, target_listen_endpoint, builtins::kad_dht, target_endpoint);
   verify_dht_server(runtime, client, server, server_endpoint, builtins::kad_dht);
   verify_dht_server(runtime, provider_node, server, server_endpoint, builtins::kad_dht);
   forge::asio::blocking::run(runtime, provider_node.async_hydrate_peer_state());

   const auto target = target_node.local_peer();
   const auto provider = provider_node.local_peer();
   const auto key =
       amino_provider_key(std::vector<std::uint8_t>{'f', 'c', 'l', '-', 'd', 'h', 't', '-', 'n', 'o', 'd', 'e'});
   auto endpoints_changed_during_commit = false;
   provider_persistence->before_provider_apply = [&] {
      provider_node.set_advertised_endpoints({});
      endpoints_changed_during_commit = true;
   };
   provider_persistence->reject_provider_upserts_after_first = true;
   auto provider_registration =
       forge::asio::blocking::run(runtime, provider_node.async_provide(builtins::kad_dht, key));
   BOOST_REQUIRE(provider_registration.active());
   BOOST_TEST(endpoints_changed_during_commit);
   const auto publications_after_initial_provide =
       server_persistence->provider_upsert_attempts.load(std::memory_order_relaxed);
   auto coalesced_registration =
       forge::asio::blocking::run(runtime, provider_node.async_provide(builtins::kad_dht, key));
   BOOST_REQUIRE(coalesced_registration.active());
   BOOST_TEST(provider_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) >= 1U);
   BOOST_TEST(server_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) ==
              publications_after_initial_provide);

   const auto found_peer = forge::asio::blocking::run(runtime, client.async_find_peer(builtins::kad_dht, target));
   BOOST_TEST(std::ranges::any_of(found_peer.closest_peers, [&](const dht::peer& value) {
      return value.id == target && std::ranges::any_of(value.endpoints, [&](const auto& current) {
                return current.to_string() == target_endpoint.to_string();
             });
   }));

   const auto providers = forge::asio::blocking::run(runtime, client.async_find_providers(builtins::kad_dht, key));
   BOOST_TEST(std::ranges::any_of(providers, [&](const dht::peer& value) {
      return value.id == provider && std::ranges::any_of(value.endpoints, [&](const auto& current) {
                return current.to_string() == provider_endpoint.to_string();
             });
   }));
   BOOST_TEST(server.metrics().dht_queries >= 2U);
   BOOST_TEST(server.metrics().dht_responses >= 2U);

   forge::asio::blocking::run(runtime, provider_registration.async_withdraw());
   BOOST_TEST(!provider_registration.active());
   BOOST_TEST(coalesced_registration.active());
   BOOST_TEST(provider_persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 0U);
   forge::asio::blocking::run(runtime, provider_registration.async_withdraw());
   BOOST_TEST(provider_persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 0U);
   forge::asio::blocking::run(runtime, coalesced_registration.async_withdraw());
   BOOST_TEST(!coalesced_registration.active());
   BOOST_TEST(provider_persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);
   forge::asio::blocking::run(runtime, coalesced_registration.async_withdraw());
   BOOST_TEST(provider_persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, provider_node.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   forge::asio::blocking::run(runtime, target_node.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_local_provider_publication_bounds_advertised_endpoints) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.max_peer_endpoints = 1;
   const auto server_profile = custom_test_dht_profile(dht::mode::server, limits);
   const auto provider_profile = custom_test_dht_profile(dht::mode::client, limits);
   const auto server_identity = make_test_certificate_identity("dht-bounded-provider-server");
   const auto provider_identity = make_test_certificate_identity("dht-bounded-provider-client");
   auto server_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto provider_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto server_options = dht_options_for(server_identity, server_profile);
   server_options.dht_record_persistence.emplace(server_profile.protocol, server_persistence);
   auto provider_options = dht_options_for(provider_identity, provider_profile);
   provider_options.dht_record_persistence.emplace(provider_profile.protocol, provider_persistence);
   for (const auto port : {4'211, 4'212}) {
      auto advertised = make_dns_tcp_endpoint(port, "bounded-provider.example.com");
      advertised.peer = provider_identity.peer;
      provider_options.advertised_endpoints.push_back(std::move(advertised));
   }
   auto server = node{runtime, std::move(server_options)};
   auto provider = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   verify_dht_server(runtime, provider, server, server_endpoint, provider_profile.protocol);
   forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());
   const auto key = make_dht_key(std::vector<std::uint8_t>{'/', 'b', 'o', 'u', 'n', 'd'});

   auto registration = forge::asio::blocking::run(runtime, provider.async_provide(provider_profile.protocol, key));
   for (auto attempt = 0U;
        attempt < 20U && server_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) == 0U;
        ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "bounded DHT provider publication");
   }
   BOOST_REQUIRE(server_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) > 0U);
   const auto local =
       forge::asio::blocking::run(runtime, provider_persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers, .limit = 1}));
   const auto remote =
       forge::asio::blocking::run(runtime, server_persistence->async_hydrate(dht::record_store::hydration_request{
                                               .kind = dht::record_store::hydration_kind::providers, .limit = 1}));
   BOOST_REQUIRE_EQUAL(local.providers.size(), 1U);
   BOOST_REQUIRE_EQUAL(remote.providers.size(), 1U);
   BOOST_TEST(local.providers.front().endpoints.size() == 1U);
   BOOST_TEST(remote.providers.front().endpoints.size() == 1U);

   forge::asio::blocking::run(runtime, registration.async_withdraw());
   forge::asio::blocking::run(runtime, provider.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_provide_requires_quorum_and_rolls_back_local_record) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_profile = custom_test_dht_profile(dht::mode::server);
   const auto provider_profile = custom_test_dht_profile(dht::mode::client);
   const auto server_identity = make_test_certificate_identity("dht-quorum-server");
   const auto provider_identity = make_test_certificate_identity("dht-quorum-provider");
   auto server = node{runtime, dht_options_for(server_identity, server_profile)};
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto provider_options = dht_options_for(provider_identity, provider_profile);
   provider_options.dht_record_persistence.emplace(content_swarm_test_dht, persistence);
   auto provider_node = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(listen(provider_node, runtime));
   verify_dht_server(runtime, provider_node, server, server_endpoint, content_swarm_test_dht);
   forge::asio::blocking::run(runtime, provider_node.async_hydrate_peer_state());

   const auto key = make_dht_key(
       std::vector<std::uint8_t>{'q', 'u', 'o', 'r', 'u', 'm', '-', 'r', 'o', 'l', 'l', 'b', 'a', 'c', 'k'});
   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         runtime, provider_node.async_provide(content_swarm_test_dht, key,
                                                              dht::query_options{.requested_count = 2, .quorum = 2}))),
                     exceptions::peer_not_found);
   BOOST_TEST(persistence->provider_upsert_attempts.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);

   forge::asio::blocking::run(runtime, provider_node.async_stop());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_provide_replicates_to_all_closest_peers_after_quorum) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto first_identity = make_test_certificate_identity("dht-fanout-first");
   const auto second_identity = make_test_certificate_identity("dht-fanout-second");
   const auto provider_identity = make_test_certificate_identity("dht-fanout-provider");
   auto first_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto second_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto first_options = dht_options_for(first_identity, custom_test_dht_profile(dht::mode::server));
   auto second_options = dht_options_for(second_identity, custom_test_dht_profile(dht::mode::server));
   first_options.dht_record_persistence.emplace(content_swarm_test_dht, first_persistence);
   second_options.dht_record_persistence.emplace(content_swarm_test_dht, second_persistence);
   auto provider_options = dht_options_for(
       provider_identity, custom_test_dht_profile(dht::mode::client, dht::options{.replication = 2, .alpha = 1}));
   auto advertised = make_dns_tcp_endpoint(4'194, "dht-fanout-provider.example.com");
   advertised.peer = provider_identity.peer;
   provider_options.advertised_endpoints.push_back(advertised);

   auto first = node{runtime, std::move(first_options)};
   auto second = node{runtime, std::move(second_options)};
   auto provider = node{runtime, std::move(provider_options)};
   const auto first_endpoint = listen(first, runtime);
   const auto second_endpoint = listen(second, runtime);
   static_cast<void>(listen(provider, runtime));
   verify_dht_server(runtime, provider, first, first_endpoint, content_swarm_test_dht);
   verify_dht_server(runtime, provider, second, second_endpoint, content_swarm_test_dht);
   for (auto attempt = 0U; attempt < 20U && provider.routing_status(content_swarm_test_dht).active < 2U; ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "DHT fanout peer admission");
   }
   BOOST_REQUIRE(provider.routing_status(content_swarm_test_dht).active == 2U);
   forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());

   const auto key = make_dht_key(std::vector<std::uint8_t>{'f', 'u', 'l', 'l', '-', 'f', 'a', 'n', 'o', 'u', 't'});
   for (auto attempt = 0U;
        attempt < 8U && (first_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) == 0U ||
                         second_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) == 0U);
        ++attempt) {
      auto registration = forge::asio::blocking::run(
          runtime, provider.async_provide(content_swarm_test_dht, key, dht::query_options{.quorum = 1}));
      BOOST_REQUIRE(registration.active());
      forge::asio::blocking::run(runtime, registration.async_withdraw());
   }
   BOOST_TEST(first_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) > 0U);
   BOOST_TEST(second_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) > 0U);

   forge::asio::blocking::run(runtime, provider.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_get_providers_does_not_persist_third_party_claim) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.failure_threshold = 1;
   const auto server_profile = custom_test_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_dht_profile(dht::mode::client, limits);
   const auto server_identity = make_test_certificate_identity("dht-backpressure-server");
   const auto client_identity = make_test_certificate_identity("dht-backpressure-client");
   const auto provider_identity = make_test_certificate_identity("dht-backpressure-provider");
   auto server_options = dht_options_for(server_identity, server_profile);
   auto client_options = dht_options_for(client_identity, client_profile);
   auto client_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   client_persistence->reject_provider_upserts = true;
   client_options.dht_record_persistence.emplace(content_swarm_test_dht, client_persistence);
   auto provider_options = dht_options_for(provider_identity, client_profile);
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   auto provider_node = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(listen(provider_node, runtime));
   verify_dht_server(runtime, client, server, server_endpoint, content_swarm_test_dht);
   verify_dht_server(runtime, provider_node, server, server_endpoint, content_swarm_test_dht);
   forge::asio::blocking::run(runtime, provider_node.async_hydrate_peer_state());

   const auto key = make_dht_key(std::vector<std::uint8_t>{'r', 'e', 'm', 'o', 't', 'e', '-', 'p', 'r', 'o', 'v'});
   auto provider_registration =
       forge::asio::blocking::run(runtime, provider_node.async_provide(content_swarm_test_dht, key));
   BOOST_REQUIRE(provider_registration.active());

   const auto routing_before = client.routing_status(content_swarm_test_dht);
   const auto server_before = client.peers().find(server.local_peer());
   const auto queries_before = server.metrics().dht_queries;
   const auto responses_before = server.metrics().dht_responses;
   BOOST_REQUIRE(server_before.has_value());
   BOOST_REQUIRE(routing_before.active > 0U);

   const auto discovered =
       forge::asio::blocking::run(runtime, client.async_find_providers(content_swarm_test_dht, key));

   const auto server_after = client.peers().find(server.local_peer());
   BOOST_REQUIRE(server_after.has_value());
   BOOST_TEST(
       std::ranges::any_of(discovered, [&](const auto& value) { return value.id == provider_node.local_peer(); }));
   BOOST_TEST(client_persistence->provider_upsert_attempts.load(std::memory_order_relaxed) == 0U);
   BOOST_TEST(server.metrics().dht_queries > queries_before);
   BOOST_TEST(server.metrics().dht_responses > responses_before);
   BOOST_TEST(server_after->failures == server_before->failures);
   BOOST_TEST(client.routing_status(content_swarm_test_dht).active == routing_before.active);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, provider_node.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_iterative_lookup_walks_many_peer_topology) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto seed_identity = make_test_certificate_identity("dht-seed");
   const auto hop_identity = make_test_certificate_identity("dht-hop");
   const auto target_identity = make_test_certificate_identity("dht-target");
   const auto client_identity = make_test_certificate_identity("dht-client");
   auto limits = dht::options{};
   limits.alpha = 1;
   const auto server_profile = custom_test_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_dht_profile(dht::mode::client, limits);
   auto seed_options = dht_options_for(seed_identity, server_profile);
   auto hop_options = dht_options_for(hop_identity, server_profile);
   auto target_options = dht_options_for(target_identity, server_profile);
   auto client_options = dht_options_for(client_identity, client_profile);

   auto seed = node{runtime, std::move(seed_options)};
   auto hop = node{runtime, std::move(hop_options)};
   auto target_node = node{runtime, std::move(target_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto seed_endpoint = listen(seed, runtime);
   const auto hop_endpoint = listen(hop, runtime);
   const auto target_listen_endpoint = listen(target_node, runtime);

   auto target_endpoint = make_dns_tcp_endpoint(4127, "target.example.com");
   target_endpoint.peer = target_node.local_peer();
   verify_dht_server(runtime, hop, target_node, target_listen_endpoint, content_swarm_test_dht, target_endpoint);

   auto hop_discovery_endpoint = make_dns_tcp_endpoint(4126, "hop.example.com");
   hop_discovery_endpoint.peer = hop.local_peer();
   verify_dht_server(runtime, seed, hop, hop_endpoint, content_swarm_test_dht, hop_discovery_endpoint);
   client.peers().learn_endpoint(hop.local_peer(), hop_endpoint, capability_set{});
   verify_dht_server(runtime, client, seed, seed_endpoint, content_swarm_test_dht);

   const auto target = target_node.local_peer();

   const auto found = forge::asio::blocking::run(runtime, client.async_find_peer(content_swarm_test_dht, target));
   BOOST_TEST(found.complete);
   BOOST_TEST(std::ranges::any_of(found.closest_peers, [&](const dht::peer& value) {
      return value.id == target && std::ranges::any_of(value.endpoints, [&](const auto& current) {
                return current.to_string() == target_endpoint.to_string();
             });
   }));
   BOOST_TEST(seed.metrics().dht_queries >= 1U);
   BOOST_TEST(hop.metrics().dht_queries >= 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, target_node.async_stop());
   forge::asio::blocking::run(runtime, hop.async_stop());
   forge::asio::blocking::run(runtime, seed.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_iterative_provider_lookup_and_provide_reach_closest_peers) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto seed_identity = make_test_certificate_identity("dht-provider-seed");
   const auto hop_identity = make_test_certificate_identity("dht-provider-hop");
   const auto client_identity = make_test_certificate_identity("dht-provider-client");
   const auto provider_identity = make_test_certificate_identity("dht-provider-origin");
   auto limits = dht::options{};
   limits.alpha = 1;
   const auto server_profile = custom_test_dht_profile(dht::mode::server, limits);
   const auto client_profile = custom_test_dht_profile(dht::mode::client, limits);
   auto seed_options = dht_options_for(seed_identity, server_profile);
   auto hop_options = dht_options_for(hop_identity, server_profile);
   auto client_options = dht_options_for(client_identity, client_profile);
   auto provider_endpoint = make_dns_tcp_endpoint(4131, "provider.example.com");
   provider_endpoint.peer = provider_identity.peer;
   auto provider_options = dht_options_for(provider_identity, client_profile);
   provider_options.advertised_endpoints.push_back(provider_endpoint);

   auto seed = node{runtime, std::move(seed_options)};
   auto hop = node{runtime, std::move(hop_options)};
   auto client = node{runtime, std::move(client_options)};
   auto provider_node = node{runtime, std::move(provider_options)};
   const auto seed_endpoint = listen(seed, runtime);
   const auto hop_endpoint = listen(hop, runtime);
   const auto client_endpoint = listen(client, runtime);
   static_cast<void>(client_endpoint);
   static_cast<void>(listen(provider_node, runtime));
   auto hop_discovery_endpoint = make_dns_tcp_endpoint(4130, "provider-hop.example.com");
   hop_discovery_endpoint.peer = hop.local_peer();
   verify_dht_server(runtime, seed, hop, hop_endpoint, content_swarm_test_dht, hop_discovery_endpoint);
   client.peers().learn_endpoint(hop.local_peer(), hop_endpoint, capability_set{});
   verify_dht_server(runtime, client, seed, seed_endpoint, content_swarm_test_dht);
   verify_dht_server(runtime, provider_node, hop, hop_endpoint, content_swarm_test_dht);
   forge::asio::blocking::run(runtime, provider_node.async_hydrate_peer_state());
   forge::asio::blocking::run(runtime, client.async_hydrate_peer_state());

   const auto key = make_dht_key(
       std::vector<std::uint8_t>{'f', 'c', 'l', '-', 'd', 'h', 't', '-', 'm', 'u', 'l', 't', 'i', '-', 'h', 'o', 'p'});
   const auto provider = provider_node.local_peer();
   auto provider_registration =
       forge::asio::blocking::run(runtime, provider_node.async_provide(content_swarm_test_dht, key));
   BOOST_REQUIRE(provider_registration.active());

   const auto providers = forge::asio::blocking::run(runtime, client.async_find_providers(content_swarm_test_dht, key));
   BOOST_TEST(std::ranges::any_of(providers, [&](const dht::peer& value) {
      return value.id == provider && std::ranges::any_of(value.endpoints, [&](const auto& current) {
                return current.to_string() == provider_endpoint.to_string();
             });
   }));

   const auto publish_key =
       make_dht_key(std::vector<std::uint8_t>{'f', 'c', 'l', '-', 'd', 'h', 't', '-', 'r', 'e', 'p', 'u', 'b'});
   auto client_registration =
       forge::asio::blocking::run(runtime, client.async_provide(content_swarm_test_dht, publish_key));
   BOOST_REQUIRE(client_registration.active());
   auto stored = std::vector<dht::peer>{};
   for (auto attempt = 0; attempt < 100; ++attempt) {
      stored = forge::asio::blocking::run(runtime, hop.async_find_providers(content_swarm_test_dht, publish_key,
                                                                            dht::query_options{.requested_count = 1}));
      if (std::ranges::any_of(stored, [&](const dht::peer& value) { return value.id == client.local_peer(); })) {
         break;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "DHT provider publication convergence");
   }
   BOOST_TEST(std::ranges::any_of(stored, [&](const dht::peer& value) { return value.id == client.local_peer(); }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, provider_node.async_stop());
   forge::asio::blocking::run(runtime, hop.async_stop());
   forge::asio::blocking::run(runtime, seed.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_node_registers_and_discovers_over_negotiated_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options =
       options_for(peer(122), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   server_options.limits.rendezvous.require_signed_peer_record = false;
   auto client_options =
       options_for(peer(123), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   client_options.limits.rendezvous.require_signed_peer_record = false;
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});

   const auto response = forge::asio::blocking::run(
       runtime, client.async_rendezvous_register(server.local_peer(), rendezvous::register_request{
                                                                          .namespace_name = "forge.discovery",
                                                                          .ttl = std::chrono::seconds{7'200},
                                                                      }));
   BOOST_TEST(static_cast<int>(response.status_value) == static_cast<int>(rendezvous::status::ok));
   BOOST_TEST(response.ttl == std::chrono::seconds{7'200});

   const auto discovered = forge::asio::blocking::run(
       runtime, client.async_rendezvous_discover(server.local_peer(), rendezvous::discover_request{
                                                                          .namespace_name = "forge.discovery",
                                                                          .limit = 10,
                                                                      }));
   BOOST_TEST(static_cast<int>(discovered.status_value) == static_cast<int>(rendezvous::status::ok));
   BOOST_REQUIRE_EQUAL(discovered.registrations.size(), 1U);
   BOOST_TEST(discovered.registrations.front().namespace_name == "forge.discovery");
   BOOST_TEST(discovered.registrations.front().peer.to_string().empty());
   BOOST_TEST(discovered.registrations.front().signed_peer_record.empty());
   BOOST_TEST(rendezvous::codec::read_cookie(discovered.cookie) >= 1U);
   BOOST_TEST(server.metrics().rendezvous_registrations >= 1U);
   BOOST_TEST(server.metrics().rendezvous_discovers >= 1U);

   const auto invalid_cookie = forge::asio::blocking::run(
       runtime, client.async_rendezvous_discover(server.local_peer(),
                                                 rendezvous::discover_request{
                                                     .namespace_name = "forge.discovery",
                                                     .limit = 10,
                                                     .cookie = rendezvous::codec::make_cookie(1, "forge.other"),
                                                 }));
   BOOST_TEST(static_cast<int>(invalid_cookie.status_value) == static_cast<int>(rendezvous::status::invalid_cookie));
   BOOST_TEST(invalid_cookie.registrations.empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_signed_registration_rejects_mixed_endpoints_atomically) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("rendezvous-atomic-filter-server");
   const auto client_identity = make_test_certificate_identity("rendezvous-atomic-filter-client");
   auto server_options =
       options_for(server_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, options_for(client_identity, capability_set{.bits = capabilities::direct_quic})};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});

   auto accepted = make_dns_tcp_endpoint(4'224, "rendezvous-atomic.example.com");
   accepted.peer = client.local_peer();
   auto filtered = make_tcp_endpoint(4'225, "169.254.20.30");
   filtered.peer = client.local_peer();
   const auto signed_record =
       make_signed_rendezvous_peer_record(client_identity, std::vector<endpoint>{accepted, filtered}, 1);

   const auto rejected = forge::asio::blocking::run(
       runtime, client.async_rendezvous_register(server.local_peer(), rendezvous::register_request{
                                                                          .namespace_name = "forge.atomic-filter",
                                                                          .signed_peer_record = signed_record,
                                                                          .ttl = std::chrono::seconds{7'200},
                                                                      }));
   BOOST_TEST(static_cast<int>(rejected.status_value) == static_cast<int>(rendezvous::status::not_authorized));
   BOOST_TEST(server.metrics().rendezvous_registrations == 0U);
   BOOST_TEST(server.peers().discover_rendezvous("forge.atomic-filter", 0, 10).empty());

   auto malformed_payload = rendezvous::codec::encode_peer_record(rendezvous::peer_record{
       .peer = client.local_peer(),
       .endpoints = addresses_for({accepted}),
       .sequence = 2,
   });
   const auto malformed_address_info = std::array<std::uint8_t, 5>{0x1a, 0x03, 0x0a, 0x01, 0xff};
   malformed_payload.insert(malformed_payload.end(), malformed_address_info.begin(), malformed_address_info.end());
   const auto malformed_record =
       signed_envelope::seal(public_key_for(client_identity),
                             forge::crypto::pki::pem::read_private_key(client_identity.private_key_pem),
                             "libp2p-routing-state", rendezvous::codec::peer_record_payload_type(), malformed_payload)
           .encode();
   const auto malformed_rejected = forge::asio::blocking::run(
       runtime, client.async_rendezvous_register(server.local_peer(), rendezvous::register_request{
                                                                          .namespace_name = "forge.atomic-filter",
                                                                          .signed_peer_record = malformed_record,
                                                                          .ttl = std::chrono::seconds{7'200},
                                                                      }));
   BOOST_TEST(static_cast<int>(malformed_rejected.status_value) ==
              static_cast<int>(rendezvous::status::invalid_signed_peer_record));
   BOOST_TEST(server.metrics().rendezvous_registrations == 0U);
   BOOST_TEST(server.peers().discover_rendezvous("forge.atomic-filter", 0, 10).empty());

   const auto discovered = forge::asio::blocking::run(
       runtime, client.async_rendezvous_discover(server.local_peer(), rendezvous::discover_request{
                                                                          .namespace_name = "forge.atomic-filter",
                                                                          .limit = 10,
                                                                      }));
   BOOST_TEST(static_cast<int>(discovered.status_value) == static_cast<int>(rendezvous::status::ok));
   BOOST_TEST(discovered.registrations.empty());
   BOOST_TEST(server.peers().discover_rendezvous("forge.atomic-filter", 0, 10).empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_server_enforces_per_peer_registration_capacity) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options =
       options_for(peer(224), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   server_options.limits.rendezvous.max_registrations_per_peer = 1;
   const auto first_identity = make_test_certificate_identity("rendezvous-capacity-first");
   const auto second_identity = make_test_certificate_identity("rendezvous-capacity-second");
   auto first_options =
       options_for(first_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   auto second_options =
       options_for(second_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   auto server = node{runtime, std::move(server_options)};
   auto first = node{runtime, std::move(first_options)};
   auto second = node{runtime, std::move(second_options)};
   const auto server_endpoint = listen(server, runtime);
   first.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   second.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   auto first_endpoint = make_dns_tcp_endpoint(4'225, "rendezvous-capacity-first.example.com");
   first_endpoint.peer = first.local_peer();
   auto second_endpoint = make_dns_tcp_endpoint(4'226, "rendezvous-capacity-second.example.com");
   second_endpoint.peer = second.local_peer();
   const auto first_record =
       make_signed_rendezvous_peer_record(first_identity, std::vector<endpoint>{std::move(first_endpoint)}, 1);
   const auto second_record =
       make_signed_rendezvous_peer_record(second_identity, std::vector<endpoint>{std::move(second_endpoint)}, 1);

   const auto register_namespace = [&](node& client, std::string namespace_name,
                                       const std::vector<std::uint8_t>& signed_peer_record) {
      return forge::asio::blocking::run(
          runtime,
          client.async_rendezvous_register(server.local_peer(), rendezvous::register_request{
                                                                    .namespace_name = std::move(namespace_name),
                                                                    .signed_peer_record = signed_peer_record,
                                                                    .ttl = std::chrono::seconds{7'200},
                                                                }));
   };
   BOOST_TEST(static_cast<int>(register_namespace(first, "forge.first", first_record).status_value) ==
              static_cast<int>(rendezvous::status::ok));
   BOOST_TEST(static_cast<int>(register_namespace(first, "forge.first", first_record).status_value) ==
              static_cast<int>(rendezvous::status::ok));
   BOOST_TEST(static_cast<int>(register_namespace(first, "forge.second", first_record).status_value) ==
              static_cast<int>(rendezvous::status::unavailable));
   BOOST_TEST(static_cast<int>(register_namespace(second, "forge.second", second_record).status_value) ==
              static_cast<int>(rendezvous::status::ok));

   forge::asio::blocking::run(runtime, second.async_stop());
   forge::asio::blocking::run(runtime, first.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_request_uses_one_deadline_after_protocol_negotiation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_apply = true;
   auto server_options =
       options_for(peer(124), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.peer_state.persistence = persistence;
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   server_options.limits.rendezvous.require_signed_peer_record = false;
   auto client_options =
       options_for(peer(125), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   client_options.limits.topology.query_timeout = std::chrono::milliseconds{75};
   client_options.limits.rendezvous.require_signed_peer_record = false;
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});

   const auto started = std::chrono::steady_clock::now();
   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_rendezvous_register(server.local_peer(), rendezvous::register_request{
                                                                             .namespace_name = "forge.timeout",
                                                                             .ttl = std::chrono::seconds{7'200},
                                                                         }));
      BOOST_FAIL("expected Rendezvous response timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   BOOST_TEST(elapsed.count() < 225);
   BOOST_TEST(persistence->wait_until_apply_blocked());

   persistence->release_apply();
   persistence->block_apply = false;
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_stop_cancels_a_blocked_rendezvous_response) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_apply = true;
   persistence->block_rendezvous_apply_only = true;
   const auto server_identity = make_test_certificate_identity("topology-rendezvous-stop-server");
   const auto seeker_identity = make_test_certificate_identity("topology-rendezvous-stop-seeker");
   auto server_options =
       options_for(server_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.peer_state.persistence = persistence;
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   auto server = node{runtime, std::move(server_options)};
   auto server_endpoint = listen(server, runtime);
   server_endpoint.peer = server.local_peer();

   auto seeker_options = options_for(seeker_identity, capability_set{.bits = capabilities::direct_quic});
   seeker_options.limits.topology.dht_enabled = false;
   seeker_options.limits.topology.peer_exchange_enabled = false;
   seeker_options.limits.topology.query_timeout = std::chrono::seconds{10};
   seeker_options.limits.topology.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = server_endpoint,
           .namespaces = {"forge.topology-stop"},
       },
   };
   auto seeker = node{runtime, std::move(seeker_options)};
   static_cast<void>(listen(seeker, runtime));
   static_cast<void>(forge::asio::blocking::run(runtime, seeker.async_start()));
   auto refresh = boost::asio::co_spawn(runtime.context(), seeker.async_refresh_discovery(), boost::asio::use_future);

   BOOST_REQUIRE(persistence->wait_until_apply_blocked());
   seeker.request_stop();
   auto stopped = boost::asio::co_spawn(runtime.context(), seeker.async_stop(), boost::asio::use_future);
   wait_for_server(stopped, std::chrono::seconds{1}, "topology Rendezvous stop");
   try {
      static_cast<void>(refresh.get());
   } catch (const forge::exceptions::base&) {
   }

   persistence->release_apply();
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_rendezvous_stream_timeout_preserves_shared_authenticated_session) {
   auto server_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("topology-rendezvous-session-server");
   const auto client_identity = make_test_certificate_identity("topology-rendezvous-session-client");
   auto server_options =
       options_for(server_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   auto server = node{server_runtime, std::move(server_options)};
   register_echo(server);
   auto server_endpoint = listen_tcp(server, server_runtime);
   server_endpoint.peer = server.local_peer();

   auto client_options = options_for(client_identity, capability_set{.bits = capabilities::direct_quic});
   client_options.limits.topology.dht_enabled = false;
   client_options.limits.topology.peer_exchange_enabled = false;
   client_options.limits.topology.query_timeout = std::chrono::milliseconds{200};
   client_options.limits.topology.refresh_interval = std::chrono::seconds{60};
   client_options.limits.topology.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = server_endpoint,
           .namespaces = {"forge.session-preservation"},
       },
   };
   auto client = node{client_runtime, std::move(client_options)};
   static_cast<void>(listen(client, client_runtime));
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   static_cast<void>(forge::asio::blocking::run(client_runtime, client.async_start()));
   static_cast<void>(forge::asio::blocking::run(client_runtime, client.async_refresh_discovery()));

   const auto before = client.diagnostics();
   const auto established = std::ranges::find_if(before.sessions, [&](const diagnostics::session& session) {
      return session.remote_peer == server.local_peer() && !session.closed &&
             session.identify_state == identify::state::identified;
   });
   BOOST_REQUIRE(established != before.sessions.end());
   const auto session_id = established->id;
   const auto sessions_opened = client.metrics().sessions_opened;

   auto release_server = block_runtime(server_runtime, "topology Rendezvous stream negotiation barrier");
   auto refresh =
       boost::asio::co_spawn(client_runtime.context(), client.async_refresh_discovery(), boost::asio::use_future);
   const auto refresh_ready = refresh.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
   auto refresh_returned = false;
   auto refresh_code = std::optional<exceptions::code>{};
   if (refresh_ready) {
      try {
         static_cast<void>(refresh.get());
         refresh_returned = true;
      } catch (const forge::exceptions::base& error) {
         refresh_code = exceptions::code_of(error);
      }
   }
   const auto while_blocked = client.diagnostics();
   const auto original_session_alive =
       std::ranges::any_of(while_blocked.sessions, [&](const diagnostics::session& session) {
          return session.id == session_id && session.remote_peer == server.local_peer() && !session.closed;
       });
   release_server->set_value();

   BOOST_REQUIRE(refresh_ready);
   BOOST_TEST(!refresh_returned);
   BOOST_REQUIRE(refresh_code.has_value());
   BOOST_TEST(static_cast<int>(*refresh_code) == static_cast<int>(exceptions::code::timeout));
   BOOST_TEST(original_session_alive);

   auto stream = forge::asio::blocking::run(
       client_runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                         node::open_options{
                                                             .allow_relay = false,
                                                             .timeout = std::chrono::seconds{2},
                                                             .direct_attempt_timeout = std::chrono::seconds{2},
                                                             .max_direct_endpoints = 1,
                                                         }));
   const auto payload = std::vector<std::uint8_t>{'r', 'e', 'n', 'd', 'e', 'z', 'v', 'o', 'u', 's'};
   forge::asio::blocking::run(client_runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(client_runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   forge::asio::blocking::run(client_runtime, stream.async_close());
   BOOST_TEST(client.metrics().sessions_opened == sessions_opened);
   BOOST_TEST(std::ranges::any_of(client.diagnostics().sessions, [&](const diagnostics::session& session) {
      return session.id == session_id && session.remote_peer == server.local_peer() && !session.closed;
   }));

   forge::asio::blocking::run(client_runtime, client.async_stop());
   forge::asio::blocking::run(server_runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_rendezvous_client_and_server_only_readvertises_local_registrations) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto dns_server = forge::tests::dns::local_dns_server{hidden_dns_response};
   const auto upstream_identity = make_test_certificate_identity("rendezvous-readvertise-upstream");
   const auto registrar_identity = make_test_certificate_identity("rendezvous-readvertise-registrar");
   const auto bridge_identity = make_test_certificate_identity("rendezvous-readvertise-bridge");
   const auto observer_identity = make_test_certificate_identity("rendezvous-readvertise-observer");
   auto upstream_options =
       options_for(upstream_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   upstream_options.limits.rendezvous.operating_role = rendezvous::role::server;
   upstream_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   auto upstream = node{runtime, std::move(upstream_options)};
   auto registrar_options = options_for(registrar_identity, capability_set{.bits = capabilities::direct_quic});
   registrar_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   auto registrar = node{runtime, std::move(registrar_options)};
   auto upstream_endpoint = listen(upstream, runtime);
   auto registrar_endpoint = listen_quic_with_advertised_dns4(registrar, runtime);
   upstream_endpoint.peer = upstream.local_peer();

   auto bridge_options =
       options_for(bridge_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   bridge_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   bridge_options.limits.rendezvous.operating_role = rendezvous::role::client_and_server;
   bridge_options.limits.topology.peers = topology::watermarks{.low = 1, .target = 1, .high = 3};
   bridge_options.limits.topology.refresh_interval = std::chrono::seconds{60};
   bridge_options.limits.topology.query_timeout = std::chrono::seconds{2};
   bridge_options.limits.topology.max_parallel_queries = 1;
   bridge_options.limits.topology.max_parallel_dials = 1;
   bridge_options.limits.topology.dht_enabled = false;
   bridge_options.limits.topology.peer_exchange_enabled = false;
   bridge_options.limits.topology.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = upstream_endpoint,
           .namespaces = {"forge.readvertise"},
       },
   };
   auto bridge = node{runtime, std::move(bridge_options)};
   auto bridge_endpoint = listen(bridge, runtime);
   bridge_endpoint.peer = bridge.local_peer();
   auto observer_options = options_for(observer_identity, capability_set{.bits = capabilities::direct_quic});
   observer_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   auto observer = node{runtime, std::move(observer_options)};

   registrar.peers().learn_endpoint(upstream.local_peer(), upstream_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   const auto signed_record =
       make_signed_rendezvous_peer_record(registrar_identity, std::vector<endpoint>{registrar_endpoint}, 1);
   const auto registered_upstream = forge::asio::blocking::run(
       runtime, registrar.async_rendezvous_register(upstream.local_peer(), rendezvous::register_request{
                                                                               .namespace_name = "forge.readvertise",
                                                                               .signed_peer_record = signed_record,
                                                                               .ttl = std::chrono::seconds{7'200},
                                                                           }));
   BOOST_REQUIRE(static_cast<int>(registered_upstream.status_value) == static_cast<int>(rendezvous::status::ok));

   static_cast<void>(forge::asio::blocking::run(runtime, bridge.async_start()));
   const auto raw_results = forge::asio::blocking::run(
       runtime, bridge.async_rendezvous_discover(upstream.local_peer(), rendezvous::discover_request{
                                                                            .namespace_name = "forge.readvertise",
                                                                            .limit = 10,
                                                                        }));
   BOOST_REQUIRE(std::ranges::any_of(raw_results.registrations, [&](const rendezvous::registration& registration) {
      return registration.peer == registrar.local_peer();
   }));
   BOOST_TEST(std::ranges::none_of(
       bridge.peers().discover_rendezvous("forge.readvertise", 0, 10),
       [&](const rendezvous::registration& registration) { return registration.peer == registrar.local_peer(); }));

   const auto topology_results = forge::asio::blocking::run(runtime, bridge.async_refresh_discovery());
   BOOST_REQUIRE(std::ranges::any_of(topology_results, [&](const discovery::result& result) {
      return result.peer == registrar.local_peer() && result.discovered_by == discovery::source::rendezvous;
   }));
   BOOST_TEST(std::ranges::none_of(
       bridge.peers().discover_rendezvous("forge.readvertise", 0, 10),
       [&](const rendezvous::registration& registration) { return registration.peer == registrar.local_peer(); }));

   observer.peers().learn_endpoint(bridge.local_peer(), bridge_endpoint,
                                   capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   const auto before_local_registration = forge::asio::blocking::run(
       runtime, observer.async_rendezvous_discover(bridge.local_peer(), rendezvous::discover_request{
                                                                            .namespace_name = "forge.readvertise",
                                                                            .limit = 10,
                                                                        }));
   BOOST_REQUIRE(static_cast<int>(before_local_registration.status_value) == static_cast<int>(rendezvous::status::ok));
   BOOST_TEST(
       std::ranges::none_of(before_local_registration.registrations, [&](const rendezvous::registration& registration) {
          return registration.peer == registrar.local_peer();
       }));

   registrar.peers().learn_endpoint(bridge.local_peer(), bridge_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   const auto registered_locally = forge::asio::blocking::run(
       runtime, registrar.async_rendezvous_register(bridge.local_peer(), rendezvous::register_request{
                                                                             .namespace_name = "forge.readvertise",
                                                                             .signed_peer_record = signed_record,
                                                                             .ttl = std::chrono::seconds{7'200},
                                                                         }));
   BOOST_REQUIRE(static_cast<int>(registered_locally.status_value) == static_cast<int>(rendezvous::status::ok));

   const auto after_local_registration = forge::asio::blocking::run(
       runtime, observer.async_rendezvous_discover(bridge.local_peer(), rendezvous::discover_request{
                                                                            .namespace_name = "forge.readvertise",
                                                                            .limit = 10,
                                                                        }));
   BOOST_REQUIRE(static_cast<int>(after_local_registration.status_value) == static_cast<int>(rendezvous::status::ok));
   const auto served =
       std::ranges::find_if(after_local_registration.registrations, [&](const rendezvous::registration& registration) {
          return registration.peer == registrar.local_peer();
       });
   BOOST_REQUIRE(served != after_local_registration.registrations.end());
   BOOST_REQUIRE(!served->endpoints.empty());
   BOOST_TEST(std::ranges::any_of(served->endpoints, [&](const auto& current) {
      return current.to_string() == registrar_endpoint.to_string();
   }));

   forge::asio::blocking::run(runtime, observer.async_stop());
   forge::asio::blocking::run(runtime, bridge.async_stop());
   forge::asio::blocking::run(runtime, registrar.async_stop());
   forge::asio::blocking::run(runtime, upstream.async_stop());
   dns_server.close();
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_refresh_replaces_registration_and_cookie_discovers_new_records) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options =
       options_for(peer(132), capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   server_options.limits.rendezvous.operating_role = rendezvous::role::server;
   auto identity = make_test_certificate_identity("rendezvous-refresh-client");
   auto client_options =
       options_for(identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});

   auto first_endpoint = make_dns_tcp_endpoint(4133, "client-first.example.com");
   first_endpoint.peer = client.local_peer();
   auto second_endpoint = make_dns_tcp_endpoint(4134, "client-second.example.com");
   second_endpoint.peer = client.local_peer();
   const auto first_record = make_signed_rendezvous_peer_record(identity, std::vector<endpoint>{first_endpoint}, 1);
   const auto second_record = make_signed_rendezvous_peer_record(identity, std::vector<endpoint>{second_endpoint}, 2);

   auto first = forge::asio::blocking::run(
       runtime, client.async_rendezvous_register(server.local_peer(), rendezvous::register_request{
                                                                          .namespace_name = "forge.discovery",
                                                                          .signed_peer_record = first_record,
                                                                          .ttl = std::chrono::seconds{7'200},
                                                                      }));
   BOOST_TEST(static_cast<int>(first.status_value) == static_cast<int>(rendezvous::status::ok));

   const auto first_discovery_started = std::chrono::system_clock::now();
   const auto after_first = forge::asio::blocking::run(
       runtime, client.async_rendezvous_discover(server.local_peer(), rendezvous::discover_request{
                                                                          .namespace_name = "forge.discovery",
                                                                          .limit = 10,
                                                                      }));
   BOOST_REQUIRE_EQUAL(after_first.registrations.size(), 1U);
   BOOST_TEST(after_first.registrations.front().expires_at >= first_discovery_started + std::chrono::seconds{7'200});
   BOOST_TEST(after_first.registrations.front().expires_at <=
              std::chrono::system_clock::now() + std::chrono::seconds{7'200});
   BOOST_TEST(client.peers().discover_rendezvous("forge.discovery", 0, 10).empty());
   static_cast<void>(forge::asio::blocking::run(runtime, client.peers().async_prune_expired()));
   BOOST_TEST(client.peers().discover_rendezvous("forge.discovery", 0, 10).empty());
   const auto cookie = after_first.cookie;

   auto second = forge::asio::blocking::run(
       runtime, client.async_rendezvous_register(server.local_peer(), rendezvous::register_request{
                                                                          .namespace_name = "forge.discovery",
                                                                          .signed_peer_record = second_record,
                                                                          .ttl = std::chrono::seconds{7'200},
                                                                      }));
   BOOST_TEST(static_cast<int>(second.status_value) == static_cast<int>(rendezvous::status::ok));

   const auto after_cookie = forge::asio::blocking::run(
       runtime, client.async_rendezvous_discover(server.local_peer(), rendezvous::discover_request{
                                                                          .namespace_name = "forge.discovery",
                                                                          .limit = 10,
                                                                          .cookie = cookie,
                                                                      }));
   BOOST_REQUIRE_EQUAL(after_cookie.registrations.size(), 1U);
   BOOST_TEST(after_cookie.registrations.front().peer.to_string() == client.local_peer().to_string());
   BOOST_REQUIRE_EQUAL(after_cookie.registrations.front().endpoints.size(), 1U);
   BOOST_TEST(after_cookie.registrations.front().endpoints.front().to_string() == second_endpoint.to_string());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_discovery_refresh_learns_dht_and_rendezvous_relay_candidates_for_autorelay) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto dht_identity = make_test_certificate_identity("discovery-refresh-dht");
   auto client_identity = make_test_certificate_identity("discovery-refresh-client");
   auto relay_identity = make_test_certificate_identity("discovery-refresh-relay");
   auto dht_options = dht_options_for(dht_identity, amino_v1(dht::mode::server));
   auto rendezvous_identity = make_test_certificate_identity("discovery-refresh-rendezvous");
   auto rendezvous_options =
       options_for(rendezvous_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   rendezvous_options.limits.rendezvous.operating_role = rendezvous::role::server;
   auto relay_options = dht_options_for(
       relay_identity, amino_v1(dht::mode::server),
       capability_set{.bits = capabilities::direct_quic | capabilities::relay | capabilities::relay_reservation});
   auto dht_server = node{runtime, std::move(dht_options)};
   auto rendezvous_server = node{runtime, std::move(rendezvous_options)};
   auto relay = node{runtime, std::move(relay_options)};
   const auto dht_endpoint = listen(dht_server, runtime);
   auto rendezvous_endpoint = listen(rendezvous_server, runtime);
   const auto relay_endpoint = listen(relay, runtime);
   auto client_options = dht_options_for(
       client_identity, amino_v1(),
       capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous | capabilities::relay_reservation});
   client_options.relay_policy.auto_discovery_enabled = true;
   client_options.relay_policy.target_reservations = 1;
   client_options.relay_policy.max_candidates_per_refresh = 4;
   client_options.limits.topology.max_candidates = 4;
   rendezvous_endpoint.peer = rendezvous_server.local_peer();
   client_options.limits.topology.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = rendezvous_endpoint,
           .namespaces = {"forge.discovery"},
       },
   };
   auto client = node{runtime, std::move(client_options)};
   auto advertised_relay_endpoint = make_dns_tcp_endpoint(4140, "relay.example.com");
   advertised_relay_endpoint.peer = relay.local_peer();

   verify_dht_server(runtime, dht_server, relay, relay_endpoint, builtins::kad_dht, advertised_relay_endpoint);
   relay.peers().learn_endpoint(rendezvous_server.local_peer(), rendezvous_endpoint,
                                capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   const auto relay_record =
       make_signed_rendezvous_peer_record(relay_identity, std::vector<endpoint>{advertised_relay_endpoint}, 1);
   auto registered = forge::asio::blocking::run(
       runtime, relay.async_rendezvous_register(rendezvous_server.local_peer(), rendezvous::register_request{
                                                                                    .namespace_name = "forge.discovery",
                                                                                    .signed_peer_record = relay_record,
                                                                                    .ttl = std::chrono::seconds{7'200},
                                                                                }));
   BOOST_TEST(static_cast<int>(registered.status_value) == static_cast<int>(rendezvous::status::ok));

   verify_dht_server(runtime, client, dht_server, dht_endpoint, builtins::kad_dht);
   client.peers().learn_endpoint(rendezvous_server.local_peer(), rendezvous_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   client.peers().learn_endpoint(relay.local_peer(), relay_endpoint, capability_set{});
   static_cast<void>(listen(client, runtime));
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));

   const auto discovered = forge::asio::blocking::run(runtime, client.async_refresh_discovery());
   BOOST_TEST(rendezvous_server.metrics().rendezvous_registrations >= 2U);
   BOOST_TEST(std::ranges::any_of(discovered,
                                  [&](const discovery::result& value) { return value.peer == relay.local_peer(); }));
   const auto learned = client.peers().find(relay.local_peer());
   BOOST_REQUIRE(learned.has_value());
   BOOST_TEST(learned->capabilities.has(capabilities::relay));
   BOOST_TEST(learned->capabilities.has(capabilities::relay_reservation));

   const auto reservations = forge::asio::blocking::run(runtime, client.async_refresh_relay_candidates());
   BOOST_REQUIRE_EQUAL(reservations.size(), 1U);
   BOOST_TEST(reservations.front().relay_peer.to_string() == relay.local_peer().to_string());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, relay.async_stop());
   forge::asio::blocking::run(runtime, rendezvous_server.async_stop());
   forge::asio::blocking::run(runtime, dht_server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_discovers_and_identifies_hidden_dht_peer_from_routing_seed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto routing_identity = make_test_certificate_identity("topology-hidden-dht-routing");
   const auto hidden_identity = make_test_certificate_identity("topology-hidden-dht-target");
   const auto seeker_identity = make_test_certificate_identity("topology-hidden-dht-seeker");
   auto dht_limits = dht::options{};
   dht_limits.alpha = 1;
   // Echoing the seeker would consume one of the seed-and-hidden shortlist slots and can displace the hidden candidate.
   dht_limits.replication = 2;
   const auto routing_profile = custom_test_dht_profile(dht::mode::server, dht_limits);
   const auto seeker_profile = custom_test_dht_profile(dht::mode::client, dht_limits);

   auto routing = node{runtime, dht_options_for(routing_identity, routing_profile)};
   auto hidden = node{runtime, dht_options_for(hidden_identity, routing_profile)};
   auto seeker_options = dht_options_for(seeker_identity, seeker_profile);
   seeker_options.limits.topology.peers = topology::watermarks{.low = 2, .target = 2, .high = 3};
   seeker_options.limits.topology.refresh_interval = std::chrono::milliseconds{60'000};
   seeker_options.limits.topology.query_timeout = std::chrono::milliseconds{5'000};
   seeker_options.limits.topology.max_candidates = 4;
   seeker_options.limits.topology.max_parallel_queries = 1;
   seeker_options.limits.topology.max_parallel_dials = 1;
   seeker_options.limits.topology.peer_exchange_enabled = false;
   auto seeker = node{runtime, std::move(seeker_options)};

   const auto routing_endpoint = listen(routing, runtime);
   const auto hidden_endpoint = listen(hidden, runtime);
   static_cast<void>(listen(seeker, runtime));
   BOOST_TEST(static_cast<int>(hidden_endpoint.transport.host_type) == static_cast<int>(endpoint::host_kind::ip4));
   BOOST_TEST(hidden_endpoint.transport.host == "127.0.0.1");
   verify_dht_server(runtime, routing, hidden, hidden_endpoint, content_swarm_test_dht);
   verify_dht_server(runtime, seeker, routing, routing_endpoint, content_swarm_test_dht);

   BOOST_REQUIRE(routing.peers().find(hidden.local_peer()).has_value());
   BOOST_TEST(!seeker.peers().find(hidden.local_peer()).has_value());

   static_cast<void>(forge::asio::blocking::run(runtime, seeker.async_start()));
   const auto discovered = forge::asio::blocking::run(runtime, seeker.async_refresh_discovery());

   BOOST_TEST(routing.metrics().dht_queries >= 1U);
   BOOST_TEST(std::ranges::any_of(discovered, [&](const discovery::result& value) {
      return value.peer == hidden.local_peer() && value.discovered_by == discovery::source::dht;
   }));
   const auto learned = seeker.peers().find(hidden.local_peer());
   BOOST_REQUIRE(learned.has_value());
   BOOST_TEST(static_cast<int>(learned->discovered_by) == static_cast<int>(discovery::source::dht));
   BOOST_TEST(std::ranges::any_of(learned->protocols,
                                  [](const protocol_id& protocol) { return protocol == content_swarm_test_dht; }));
   for (auto attempt = 0U; attempt < 20U && !has_identified_session(seeker, hidden.local_peer()); ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{50}, "hidden DHT topology Identify");
   }
   BOOST_REQUIRE(has_identified_session(seeker, hidden.local_peer()));

   forge::asio::blocking::run(runtime, seeker.async_stop());
   forge::asio::blocking::run(runtime, hidden.async_stop());
   forge::asio::blocking::run(runtime, routing.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_discovers_and_identifies_hidden_rendezvous_registration) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto dns_server = forge::tests::dns::local_dns_server{hidden_dns_response};
   const auto rendezvous_identity = make_test_certificate_identity("topology-hidden-rendezvous-point");
   const auto hidden_identity = make_test_certificate_identity("topology-hidden-rendezvous-target");
   const auto seeker_identity = make_test_certificate_identity("topology-hidden-rendezvous-seeker");
   auto rendezvous_options =
       options_for(rendezvous_identity, capability_set{.bits = capabilities::direct_quic | capabilities::rendezvous});
   auto rendezvous_persistence = std::make_shared<tracking_peer_store_persistence>();
   rendezvous_options.peer_state.persistence = rendezvous_persistence;
   rendezvous_options.limits.rendezvous.operating_role = rendezvous::role::server;
   rendezvous_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   auto hidden_options = options_for(hidden_identity, capability_set{.bits = capabilities::direct_quic});
   hidden_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   auto hidden = node{runtime, std::move(hidden_options)};
   auto rendezvous_point = node{runtime, std::move(rendezvous_options)};
   auto rendezvous_endpoint = listen(rendezvous_point, runtime);
   auto hidden_endpoint = listen_quic_with_advertised_dns4(hidden, runtime);
   rendezvous_endpoint.peer = rendezvous_point.local_peer();

   hidden.peers().learn_endpoint(rendezvous_point.local_peer(), rendezvous_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   const auto hidden_record =
       make_signed_rendezvous_peer_record(hidden_identity, std::vector<endpoint>{hidden_endpoint}, 1);
   const auto registered = forge::asio::blocking::run(
       runtime,
       hidden.async_rendezvous_register(rendezvous_point.local_peer(), rendezvous::register_request{
                                                                           .namespace_name = "forge.hidden-rendezvous",
                                                                           .signed_peer_record = hidden_record,
                                                                           .ttl = std::chrono::seconds{7'200},
                                                                       }));
   BOOST_REQUIRE(registered.status_value == rendezvous::status::ok);

   auto seeker_options = options_for(seeker_identity, capability_set{.bits = capabilities::direct_quic});
   seeker_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   seeker_options.limits.topology.peers = topology::watermarks{.low = 2, .target = 2, .high = 3};
   seeker_options.limits.topology.refresh_interval = std::chrono::milliseconds{60'000};
   seeker_options.limits.topology.query_timeout = std::chrono::milliseconds{5'000};
   seeker_options.limits.topology.max_candidates = 4;
   seeker_options.limits.topology.max_parallel_queries = 1;
   seeker_options.limits.topology.max_parallel_dials = 1;
   seeker_options.limits.topology.dht_enabled = false;
   seeker_options.limits.topology.peer_exchange_enabled = false;
   seeker_options.limits.topology.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = rendezvous_endpoint,
           .namespaces = {"forge.hidden-rendezvous"},
       },
   };
   auto seeker = node{runtime, std::move(seeker_options)};
   static_cast<void>(listen(seeker, runtime));

   BOOST_TEST(!seeker.peers().find(hidden.local_peer()).has_value());

   static_cast<void>(forge::asio::blocking::run(runtime, seeker.async_start()));
   const auto discovered = forge::asio::blocking::run(runtime, seeker.async_refresh_discovery());

   const auto registrations = rendezvous_point.peers().discover_rendezvous("forge.hidden-rendezvous", 0, 10);
   const auto local_registration = std::ranges::find_if(
       registrations, [&](const rendezvous::registration& value) { return value.peer == seeker.local_peer(); });
   BOOST_REQUIRE(local_registration != registrations.end());
   BOOST_REQUIRE(!local_registration->signed_peer_record.empty());
   const auto local_envelope = signed_envelope::decode(local_registration->signed_peer_record);
   BOOST_TEST(local_envelope.payload_type == rendezvous::codec::peer_record_payload_type(),
              boost::test_tools::per_element());
   const auto local_record = rendezvous::codec::open_peer_record(local_envelope, seeker.local_peer());
   BOOST_TEST(local_record.peer.to_string() == seeker.local_peer().to_string());
   BOOST_TEST(local_record.sequence > 0U);
   BOOST_TEST(rendezvous_point.metrics().rendezvous_registrations >= 2U);
   BOOST_TEST(rendezvous_point.metrics().rendezvous_discovers >= 1U);
   BOOST_TEST(std::ranges::any_of(discovered, [&](const discovery::result& value) {
      return value.peer == hidden.local_peer() && value.discovered_by == discovery::source::rendezvous;
   }));
   const auto learned = seeker.peers().find(hidden.local_peer());
   BOOST_REQUIRE(learned.has_value());
   BOOST_TEST(static_cast<int>(learned->discovered_by) == static_cast<int>(discovery::source::rendezvous));
   BOOST_TEST(std::ranges::any_of(learned->endpoints, [&](const auto& value) {
      return value.address.to_string() == hidden_endpoint.to_string();
   }));
   BOOST_TEST(has_identified_session(seeker, hidden.local_peer()));

   seeker.request_stop();
   forge::asio::blocking::run(runtime, seeker.async_stop());
   BOOST_REQUIRE(rendezvous_persistence->wait_until_rendezvous_removal());
   const auto after_stop = rendezvous_point.peers().discover_rendezvous("forge.hidden-rendezvous", 0, 10);
   BOOST_TEST(std::ranges::none_of(
       after_stop, [&](const rendezvous::registration& value) { return value.peer == seeker.local_peer(); }));
   forge::asio::blocking::run(runtime, hidden.async_stop());
   forge::asio::blocking::run(runtime, rendezvous_point.async_stop());
   dns_server.close();
}

BOOST_AUTO_TEST_CASE(p2p_topology_discovers_pex_hint_then_identifies_hidden_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto dns_server = forge::tests::dns::local_dns_server{hidden_dns_response};
   const auto source_identity = make_test_certificate_identity("topology-hidden-pex-source");
   const auto hidden_identity = make_test_certificate_identity("topology-hidden-pex-target");
   const auto seeker_identity = make_test_certificate_identity("topology-hidden-pex-seeker");
   auto source_options = options_for(source_identity);
   source_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   auto source = node{runtime, std::move(source_options)};
   auto hidden_options = options_for(hidden_identity, capability_set{.bits = capabilities::direct_quic});
   hidden_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   auto hidden = node{runtime, std::move(hidden_options)};
   const auto source_endpoint = listen(source, runtime);
   const auto hidden_endpoint = listen_quic_with_advertised_dns4(hidden, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, source.async_connect(hidden_endpoint, node::connect_options{.expected_peer = hidden.local_peer()})));

   auto poisoned_hint = source.peers().find(hidden.local_peer());
   BOOST_REQUIRE(poisoned_hint.has_value());
   poisoned_hint->capabilities.add(capabilities::pubsub);
   source.peers().upsert(*poisoned_hint);
   const auto source_hint = source.peers().find(hidden.local_peer());
   BOOST_REQUIRE(source_hint.has_value());
   BOOST_TEST(source_hint->capabilities.has(capabilities::pubsub));

   auto seeker_options = options_for(seeker_identity);
   seeker_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns_server.port()}};
   seeker_options.limits.topology.peers = topology::watermarks{.low = 2, .target = 2, .high = 3};
   seeker_options.limits.topology.refresh_interval = std::chrono::milliseconds{60'000};
   seeker_options.limits.topology.query_timeout = std::chrono::milliseconds{5'000};
   seeker_options.limits.topology.max_candidates = 4;
   seeker_options.limits.topology.max_parallel_queries = 1;
   seeker_options.limits.topology.max_parallel_dials = 1;
   seeker_options.limits.topology.max_peer_exchange_peers = 1;
   seeker_options.limits.topology.dht_enabled = false;
   auto seeker = node{runtime, std::move(seeker_options)};
   static_cast<void>(listen(seeker, runtime));
   static_cast<void>(forge::asio::blocking::run(
       runtime, seeker.async_connect(source_endpoint, node::connect_options{.expected_peer = source.local_peer()})));

   const auto source_record = seeker.peers().find(source.local_peer());
   BOOST_REQUIRE(source_record.has_value());
   BOOST_TEST(std::ranges::any_of(source_record->protocols,
                                  [](const protocol_id& protocol) { return protocol == builtins::peer_exchange; }));
   BOOST_TEST(has_identified_session(seeker, source.local_peer()));
   BOOST_TEST(!seeker.peers().find(hidden.local_peer()).has_value());

   static_cast<void>(forge::asio::blocking::run(runtime, seeker.async_start()));
   const auto discovered = forge::asio::blocking::run(runtime, seeker.async_refresh_discovery());

   BOOST_TEST(source.metrics().peer_exchange_messages >= 1U);
   BOOST_TEST(std::ranges::any_of(discovered, [&](const discovery::result& value) {
      return value.peer == hidden.local_peer() && value.discovered_by == discovery::source::peer_exchange;
   }));
   const auto learned = seeker.peers().find(hidden.local_peer());
   BOOST_REQUIRE(learned.has_value());
   BOOST_TEST(static_cast<int>(learned->discovered_by) == static_cast<int>(discovery::source::peer_exchange));
   BOOST_TEST(std::ranges::any_of(learned->endpoints, [&](const auto& value) {
      return value.address.to_string() == hidden_endpoint.to_string();
   }));
   BOOST_TEST(!learned->capabilities.has(capabilities::pubsub));
   BOOST_TEST(
       std::ranges::any_of(learned->endpoints, [](const auto& value) {
          return parse_endpoint(value.address.to_string()).is_direct_quic();
       }));
   BOOST_TEST(has_identified_session(seeker, hidden.local_peer()));

   forge::asio::blocking::run(runtime, seeker.async_stop());
   forge::asio::blocking::run(runtime, hidden.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   dns_server.close();
}

BOOST_AUTO_TEST_CASE(p2p_topology_mixed_circuit_and_direct_candidate_reaches_direct_listener) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto hidden = node{runtime, options_for(make_test_identity())};
   auto direct = listen_tcp(hidden, runtime);
   direct.peer = hidden.local_peer();
   const auto circuit = forge::multiformats::multiaddr::parse(
       "/ip4/127.0.0.1/tcp/1/p2p/" + peer(122).to_string() +
       "/p2p-circuit/p2p/" + hidden.local_peer().to_string());
   auto seeker_options = options_for(make_test_identity());
   seeker_options.limits.topology.peers = topology::watermarks{.low = 1, .target = 1, .high = 2};
   seeker_options.limits.topology.refresh_interval = std::chrono::seconds{60};
   seeker_options.limits.topology.query_timeout = std::chrono::seconds{3};
   seeker_options.limits.topology.max_parallel_queries = 1;
   seeker_options.limits.topology.max_parallel_dials = 1;
   seeker_options.limits.topology.max_peer_exchange_peers = 1;
   seeker_options.limits.topology.dht_enabled = false;
   auto seeker = node{runtime, std::move(seeker_options)};
   // Seed the existing PEX candidate cache, preserving the optional relay peer
   // alongside the same target's direct address for the topology dial callback.
   seeker.peers().upsert(peer_store::record{
       .peer = hidden.local_peer(),
       .capabilities = capability_set{.bits = capabilities::direct_quic},
       .discovered_by = discovery::source::peer_exchange,
       .endpoints = {{.address = circuit}, {.address = direct.to_multiaddr()}},
       .discovery_expires_at = std::chrono::system_clock::now() + std::chrono::minutes{1},
   });
   const auto candidates = seeker.peers().scored_candidates(discovery::source::peer_exchange, 1);
   BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
   BOOST_REQUIRE_EQUAL(candidates.front().endpoints.size(), 2U);
   BOOST_TEST(std::ranges::any_of(candidates.front().endpoints, [&](const auto& item) {
      return item.address.to_string() == circuit.to_string();
   }));
   BOOST_TEST(std::ranges::any_of(candidates.front().endpoints, [&](const auto& item) {
      return item.address.to_string() == direct.to_string();
   }));

   static_cast<void>(listen_tcp(seeker, runtime));
   static_cast<void>(forge::asio::blocking::run(runtime, seeker.async_start()));
   static_cast<void>(forge::asio::blocking::run(runtime, seeker.async_refresh_discovery()));
   BOOST_TEST(has_identified_session(seeker, hidden.local_peer()));
   BOOST_TEST(seeker.metrics().path_direct_attempts == 1U);
   BOOST_TEST(seeker.metrics().active_sessions == 1U);
   BOOST_TEST(seeker.diagnostics().resources.active_dials == 0U);

   forge::asio::blocking::run(runtime, seeker.async_stop());
   forge::asio::blocking::run(runtime, hidden.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_rendezvous_requires_exact_identify_protocol) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = options_for(peer(135), capability_set{.bits = capabilities::direct_quic});
   auto server = node{runtime, std::move(server_options)};
   auto server_endpoint = listen(server, runtime);

   auto client_identity = make_test_certificate_identity("topology-rendezvous-protocol-gate");
   auto client_options = options_for(client_identity, capability_set{.bits = capabilities::direct_quic});
   server_endpoint.peer = server.local_peer();
   client_options.limits.topology.dht_enabled = false;
   client_options.limits.topology.peer_exchange_enabled = false;
   client_options.limits.topology.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = server_endpoint,
           .namespaces = {"forge.protocol-gate"},
       },
   };
   auto client = node{runtime, std::move(client_options)};
   static_cast<void>(listen(client, runtime));
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, client.async_refresh_discovery()),
                     exceptions::unsupported_protocol);
   BOOST_TEST(server.metrics().rendezvous_registrations == 0U);
   BOOST_TEST(server.peers().discover_rendezvous("forge.protocol-gate", 0, 10).empty());
   const auto identified = client.peers().find(server.local_peer());
   BOOST_REQUIRE(identified.has_value());
   BOOST_TEST(!std::ranges::any_of(identified->protocols,
                                   [](const auto& protocol) { return protocol == builtins::rendezvous; }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_nodes_deliver_signed_publish_over_negotiated_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for();
   auto subscriber_options = pubsub_options_for();
   subscriber_options.explicit_peer_id = peer(150);
   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   auto received = std::make_shared<std::promise<std::vector<std::uint8_t>>>();
   auto future = received->get_future();
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    pubsub::topic{.value = "forge.pubsub"},
                    [received](pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                       received->set_value(event.value.data);
                       co_return pubsub::validation_result::accept;
                    }));

   const auto published = forge::asio::blocking::run(
       runtime, publisher.async_publish(pubsub::topic{.value = "forge.pubsub"},
                                        std::vector<std::uint8_t>{'p', 'u', 'b', 's', 'u', 'b'}));
   BOOST_TEST(!published.signature.empty());

   if (future.wait_for(std::chrono::milliseconds{5'000}) != std::future_status::ready) {
      const auto metrics = subscriber.metrics();
      BOOST_FAIL("pubsub delivery did not finish; received="
                 << metrics.pubsub_messages_received << " delivered=" << metrics.pubsub_messages_delivered
                 << " invalid=" << metrics.pubsub_invalid_messages << " duplicates=" << metrics.pubsub_duplicates
                 << " rejected=" << metrics.protocol_rejections);
   }
   BOOST_TEST(future.get() == std::vector<std::uint8_t>({'p', 'u', 'b', 's', 'u', 'b'}),
              boost::test_tools::per_element());
   BOOST_TEST(publisher.pubsub_snapshot().messages_published >= 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered >= 1U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_singleflights_connection_and_serializes_first_concurrent_publishes) {
   constexpr auto publish_count = std::size_t{24};
   constexpr auto payload_size = std::size_t{64 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 8}};
   const auto publisher_identity = make_test_identity();
   const auto subscriber_identity = make_test_identity();
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   auto publisher = node{runtime, options_for(publisher_identity, pubsub_capabilities)};
   auto subscriber = node{runtime, options_for(subscriber_identity, pubsub_capabilities)};
   const auto subscriber_endpoint = listen_tcp(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   struct delivery_state {
      std::mutex mutex;
      std::condition_variable ready;
      std::set<std::vector<std::uint8_t>> values;
   };
   auto delivered = std::make_shared<delivery_state>();
   const auto subject = pubsub::topic{.value = "forge.pubsub.concurrent"};
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [delivered](pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              if (!event.value.data.empty()) {
                 {
                    auto lock = std::scoped_lock{delivered->mutex};
                    delivered->values.insert(event.value.data);
                 }
                 delivered->ready.notify_all();
              }
              co_return pubsub::validation_result::accept;
           }));
   auto publishes = std::vector<std::future<pubsub::message>>{};
   publishes.reserve(publish_count);
   auto ready = std::make_shared<std::atomic_size_t>();
   auto start = std::make_shared<std::atomic_bool>();
   for (auto index = std::size_t{}; index < publish_count; ++index) {
      auto payload = std::vector<std::uint8_t>(payload_size, static_cast<std::uint8_t>(index + 1U));
      publishes.push_back(boost::asio::co_spawn(
          runtime.context(),
          [&publisher, subject, payload = std::move(payload), ready,
           start]() mutable -> boost::asio::awaitable<pubsub::message> {
             ready->fetch_add(1U, std::memory_order_release);
             while (!start->load(std::memory_order_acquire)) {
                co_await boost::asio::post(boost::asio::use_awaitable);
             }
             co_return co_await publisher.async_publish(subject, std::move(payload));
          },
          boost::asio::use_future));
   }
   const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (ready->load(std::memory_order_acquire) != publish_count &&
          std::chrono::steady_clock::now() < start_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "concurrent GossipSub start barrier");
   }
   BOOST_REQUIRE_EQUAL(ready->load(std::memory_order_acquire), publish_count);
   start->store(true, std::memory_order_release);
   for (auto& publish : publishes) {
      BOOST_REQUIRE_MESSAGE(publish.wait_for(std::chrono::seconds{10}) == std::future_status::ready,
                            "concurrent GossipSub publish did not finish");
      try {
         static_cast<void>(publish.get());
      } catch (const forge::exceptions::base& error) {
         const auto publisher_metrics = publisher.metrics();
         const auto subscriber_metrics = subscriber.metrics();
         BOOST_FAIL("concurrent GossipSub publish failed: "
                    << error.what() << "; publisher sessions=" << publisher_metrics.active_sessions << " closed="
                    << publisher_metrics.sessions_closed << " pruned=" << publisher_metrics.sessions_pruned
                    << " connection_rejections=" << publisher_metrics.connection_rejections << " direct_failures="
                    << publisher_metrics.direct_failures << " invalid=" << publisher_metrics.pubsub_invalid_messages
                    << " protocol_rejections=" << publisher_metrics.protocol_rejections
                    << " handshakes_failed=" << publisher_metrics.handshakes_failed << "; subscriber sessions="
                    << subscriber_metrics.active_sessions << " closed=" << subscriber_metrics.sessions_closed
                    << " invalid=" << subscriber_metrics.pubsub_invalid_messages
                    << " protocol_rejections=" << subscriber_metrics.protocol_rejections);
      }
   }
   {
      auto lock = std::unique_lock{delivered->mutex};
      BOOST_REQUIRE(delivered->ready.wait_for(lock, std::chrono::seconds{10},
                                              [&] { return delivered->values.size() == publish_count; }));
      for (auto index = std::size_t{}; index < publish_count; ++index) {
         const auto expected = std::vector<std::uint8_t>(payload_size, static_cast<std::uint8_t>(index + 1U));
         BOOST_TEST(delivered->values.contains(expected));
      }
   }
   BOOST_TEST(publisher.metrics().active_sessions == 1U);
   BOOST_TEST(publisher.metrics().sessions_opened == 1U);
   BOOST_TEST(subscriber.metrics().active_sessions == 1U);
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_connection_singleflight_reclaims_transient_peer_gates_after_waiters) {
   constexpr auto peer_count = std::size_t{200};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto registry = detail::connection_singleflight_registry{};
   auto starters = std::vector<detail::connection_singleflight_registry::lease>{};
   auto waiters = std::vector<detail::connection_singleflight_registry::lease>{};
   starters.reserve(peer_count);
   waiters.reserve(peer_count);

   for (auto index = std::size_t{}; index < peer_count; ++index) {
      auto starter = registry.join(peer(static_cast<std::uint8_t>(index + 1U)), runtime.context().get_executor());
      auto waiter = registry.join(peer(static_cast<std::uint8_t>(index + 1U)), runtime.context().get_executor());
      BOOST_REQUIRE(starter.start.has_value());
      BOOST_TEST(!waiter.start.has_value());
      registry.succeed(*starter.start);
      starters.push_back(std::move(starter.participant));
      waiters.push_back(std::move(waiter.participant));
   }
   BOOST_TEST(registry.size() == peer_count);

   for (auto& starter : starters) {
      registry.leave(starter);
   }
   BOOST_TEST(registry.size() == peer_count);

   for (auto& waiter : waiters) {
      const auto result = forge::asio::blocking::run(runtime, waiter.wait());
      BOOST_TEST(result.succeeded);
      registry.leave(waiter);
   }
   BOOST_TEST(registry.size() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_connection_singleflight_shares_one_typed_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = detail::connection_singleflight_registry{};
   const auto remote = peer(217);
   auto starter = registry.join(remote, runtime.context().get_executor());
   auto first_waiter = registry.join(remote, runtime.context().get_executor());
   auto second_waiter = registry.join(remote, runtime.context().get_executor());
   BOOST_REQUIRE(starter.start.has_value());
   BOOST_TEST(!first_waiter.start.has_value());
   BOOST_TEST(!second_waiter.start.has_value());

   registry.fail(*starter.start, exceptions::code::timeout, "shared connection timeout");
   const auto first = forge::asio::blocking::run(runtime, first_waiter.participant.wait());
   const auto second = forge::asio::blocking::run(runtime, second_waiter.participant.wait());
   BOOST_TEST(!first.succeeded);
   BOOST_TEST(!second.succeeded);
   BOOST_REQUIRE(first.error.has_value());
   BOOST_REQUIRE(second.error.has_value());
   BOOST_CHECK(*first.error == exceptions::code::timeout);
   BOOST_CHECK(*second.error == exceptions::code::timeout);
   BOOST_TEST(first.message == "shared connection timeout");
   BOOST_TEST(second.message == first.message);

   registry.leave(starter.participant);
   registry.leave(first_waiter.participant);
   registry.leave(second_waiter.participant);
   BOOST_TEST(registry.size() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_connection_singleflight_survives_starter_cancellation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto registry = detail::connection_singleflight_registry{};
   const auto remote = peer(216);
   auto starter = registry.join(remote, runtime.context().get_executor());
   auto waiter = registry.join(remote, runtime.context().get_executor());
   BOOST_REQUIRE(starter.start.has_value());
   BOOST_TEST(!waiter.start.has_value());

   registry.leave(starter.participant);
   BOOST_TEST(registry.size() == 1U);
   registry.succeed(*starter.start);
   const auto result = forge::asio::blocking::run(runtime, waiter.participant.wait());
   BOOST_TEST(result.succeeded);
   registry.leave(waiter.participant);
   BOOST_TEST(registry.size() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_singleflight_waiter_cancellation_does_not_poison_owner) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto registry = detail::connection_singleflight_registry{};
   const auto remote = peer(220);
   auto owner = registry.join(remote, runtime.context().get_executor());
   auto canceled = registry.join(remote, runtime.context().get_executor());
   BOOST_REQUIRE(owner.start.has_value());
   BOOST_TEST(!canceled.start.has_value());

   auto canceled_result =
       boost::asio::co_spawn(runtime.context(), canceled.participant.wait(), boost::asio::use_future);
   canceled.participant.request_stop();
   BOOST_REQUIRE(canceled_result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   try {
      static_cast<void>(canceled_result.get());
      BOOST_FAIL("canceled singleflight waiter unexpectedly completed");
   } catch (const boost::system::system_error& error) {
      BOOST_TEST(error.code() == boost::asio::error::operation_aborted);
   }
   registry.leave(canceled.participant);

   auto late = registry.join(remote, runtime.context().get_executor());
   BOOST_TEST(!late.start.has_value());
   registry.succeed(*owner.start);
   const auto late_result = forge::asio::blocking::run(runtime, late.participant.wait());
   BOOST_TEST(late_result.succeeded);

   registry.leave(owner.participant);
   registry.leave(late.participant);
   BOOST_TEST(registry.size() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_singleflight_new_entry_failure_does_not_publish) {
   struct failure_state {
      bool fail_publish = true;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto failure = failure_state{};
   auto registry = detail::connection_singleflight_registry{detail::connection_singleflight_registry::test_hooks{
       .context = &failure, .reach = [](void* context, detail::connection_singleflight_registry::test_stage stage) {
          auto& value = *static_cast<failure_state*>(context);
          if (value.fail_publish &&
              stage == detail::connection_singleflight_registry::test_stage::before_new_entry_publish) {
             throw std::bad_alloc{};
          }
       }}};
   const auto remote = peer(214);

   BOOST_CHECK_THROW(static_cast<void>(registry.join(remote, runtime.context().get_executor())), std::bad_alloc);
   BOOST_TEST(registry.size() == 0U);

   failure.fail_publish = false;
   auto owner = registry.join(remote, runtime.context().get_executor());
   BOOST_REQUIRE(owner.start.has_value());
   registry.succeed(*owner.start);
   registry.leave(owner.participant);
   BOOST_TEST(registry.size() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_singleflight_existing_entry_failure_preserves_waiter_budget) {
   struct failure_state {
      bool fail_existing = false;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto failure = failure_state{};
   auto registry = detail::connection_singleflight_registry{detail::connection_singleflight_registry::test_hooks{
       .context = &failure, .reach = [](void* context, detail::connection_singleflight_registry::test_stage stage) {
          auto& value = *static_cast<failure_state*>(context);
          if (value.fail_existing &&
              stage == detail::connection_singleflight_registry::test_stage::before_existing_entry_commit) {
             throw std::bad_alloc{};
          }
       }}};
   const auto remote = peer(215);
   auto owner = registry.join(remote, runtime.context().get_executor(), 1);
   BOOST_REQUIRE(owner.start.has_value());

   failure.fail_existing = true;
   BOOST_CHECK_THROW(static_cast<void>(registry.join(remote, runtime.context().get_executor(), 1)), std::bad_alloc);
   BOOST_TEST(registry.size() == 1U);

   failure.fail_existing = false;
   auto waiter = registry.join(remote, runtime.context().get_executor(), 1);
   BOOST_TEST(!waiter.start.has_value());
   registry.succeed(*owner.start);
   const auto result = forge::asio::blocking::run(runtime, waiter.participant.wait());
   BOOST_TEST(result.succeeded);
   registry.leave(owner.participant);
   registry.leave(waiter.participant);
   BOOST_TEST(registry.size() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_singleflight_completion_failpoint_keeps_concurrent_and_late_waiters_terminal) {
   struct failure_state {
      bool fail_delivery = true;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto failure = failure_state{};
   auto registry = detail::connection_singleflight_registry{detail::connection_singleflight_registry::test_hooks{
       .context = &failure, .reach = [](void* context, detail::connection_singleflight_registry::test_stage stage) {
          auto& value = *static_cast<failure_state*>(context);
          if (value.fail_delivery &&
              stage == detail::connection_singleflight_registry::test_stage::before_completion_delivery) {
             throw std::bad_alloc{};
          }
       }}};
   const auto remote = peer(219);
   auto owner = registry.join(remote, runtime.context().get_executor());
   auto waiter = registry.join(remote, runtime.context().get_executor());
   BOOST_REQUIRE(owner.start.has_value());
   auto completed = boost::asio::co_spawn(runtime.context(), waiter.participant.wait(), boost::asio::use_future);

   BOOST_CHECK_NO_THROW(registry.fail(*owner.start, exceptions::code::timeout, "completion delivery failure"));
   auto late = registry.join(remote, runtime.context().get_executor());
   BOOST_TEST(!late.start.has_value());
   auto late_completed = boost::asio::co_spawn(runtime.context(), late.participant.wait(), boost::asio::use_future);
   BOOST_REQUIRE(static_cast<bool>(completed.wait_for(std::chrono::seconds{1}) == std::future_status::ready));
   BOOST_REQUIRE(static_cast<bool>(late_completed.wait_for(std::chrono::seconds{1}) == std::future_status::ready));
   const auto outcome = completed.get();
   const auto late_outcome = late_completed.get();
   BOOST_TEST(!outcome.succeeded);
   BOOST_REQUIRE(outcome.error);
   BOOST_TEST(static_cast<int>(*outcome.error) == static_cast<int>(exceptions::code::timeout));
   BOOST_TEST(outcome.message == "completion delivery failure");
   BOOST_TEST(!late_outcome.succeeded);
   BOOST_REQUIRE(late_outcome.error);
   BOOST_TEST(static_cast<int>(*late_outcome.error) == static_cast<int>(exceptions::code::timeout));
   BOOST_TEST(late_outcome.message == "completion delivery failure");
   registry.leave(owner.participant);
   registry.leave(waiter.participant);
   registry.leave(late.participant);
   BOOST_TEST(registry.size() == 0U);

   failure.fail_delivery = false;
   auto retry = registry.join(remote, runtime.context().get_executor());
   BOOST_REQUIRE(retry.start.has_value());
   registry.succeed(*retry.start);
   registry.leave(retry.participant);
   BOOST_TEST(registry.size() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_non_owner_session_close_keeps_cached_stream_generation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_test_certificate_identity("pubsub-generation-publisher");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-generation-subscriber");
   const auto pressure_identity = make_test_certificate_identity("pubsub-generation-pressure");
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   auto publisher_options = options_for(publisher_identity, pubsub_capabilities);
   set_resource_session_limits(publisher_options, 2, (std::numeric_limits<std::size_t>::max)(), 2, 2,
                               session_admission_headroom::one_session);
   publisher_options.limits.session_low_watermark = 1;
   publisher_options.limits.session_grace_period = std::chrono::milliseconds{0};
   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, options_for(subscriber_identity, pubsub_capabilities)};
   auto pressure = node{runtime, options_for(pressure_identity)};
   const auto publisher_endpoint = listen(publisher, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);
   const auto pressure_endpoint = listen(pressure, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint, pubsub_capabilities);

   struct delivery_state {
      std::mutex mutex;
      std::condition_variable ready;
      std::size_t count = 0;
   };
   auto delivered = std::make_shared<delivery_state>();
   const auto subject = pubsub::topic{.value = "forge.pubsub.session-generation"};
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    subject, [delivered](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
                       {
                          auto lock = std::scoped_lock{delivered->mutex};
                          ++delivered->count;
                       }
                       delivered->ready.notify_all();
                       co_return pubsub::validation_result::accept;
                    }));
   forge::asio::blocking::run(
       runtime,
       subscriber.async_connect(publisher_endpoint, node::connect_options{.expected_peer = publisher.local_peer()}));
   forge::asio::blocking::run(
       runtime,
       publisher.async_connect(subscriber_endpoint, node::connect_options{.expected_peer = subscriber.local_peer()}));
   const auto session_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (publisher.diagnostics().sessions.size() != 2U && std::chrono::steady_clock::now() < session_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "publisher inbound session barrier");
   }
   const auto before = publisher.diagnostics();
   BOOST_REQUIRE_EQUAL(before.sessions.size(), 2U);
   BOOST_TEST(before.sessions[0].remote_peer.to_string() == subscriber.local_peer().to_string());
   BOOST_TEST(before.sessions[1].remote_peer.to_string() == subscriber.local_peer().to_string());
   const auto non_owner_session_id = before.sessions[0].id;
   const auto owner_session_id = before.sessions[1].id;

   forge::asio::blocking::run(runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{0x41U}));
   {
      auto lock = std::unique_lock{delivered->mutex};
      BOOST_REQUIRE(delivered->ready.wait_for(lock, std::chrono::seconds{5}, [&] { return delivered->count == 1U; }));
   }
   const auto opened_streams = publisher.metrics().protocol_streams_opened;
   BOOST_REQUIRE(opened_streams >= 1U);

   forge::asio::blocking::run(
       runtime,
       publisher.async_connect(pressure_endpoint, node::connect_options{.expected_peer = pressure.local_peer()}));
   const auto after_prune = publisher.diagnostics();
   BOOST_REQUIRE_EQUAL(after_prune.sessions.size(), 2U);
   BOOST_TEST(publisher.metrics().sessions_pruned >= 1U);
   BOOST_TEST(std::ranges::none_of(after_prune.sessions,
                                   [&](const auto& session) { return session.id == non_owner_session_id; }));
   BOOST_TEST(
       std::ranges::any_of(after_prune.sessions, [&](const auto& session) { return session.id == owner_session_id; }));
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);

   forge::asio::blocking::run(runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{0x42U}));
   {
      auto lock = std::unique_lock{delivered->mutex};
      BOOST_REQUIRE(delivered->ready.wait_for(lock, std::chrono::seconds{5}, [&] { return delivered->count == 2U; }));
   }
   BOOST_TEST(publisher.metrics().protocol_streams_opened == opened_streams);
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
   forge::asio::blocking::run(runtime, pressure.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_dead_outbound_generation_detaches_mesh_and_reannounces_before_graft) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_test_certificate_identity("pubsub-outbound-generation-publisher");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-outbound-generation-subscriber");
   auto publisher_options = pubsub_options_for(publisher_identity);
   publisher_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   auto subscriber_options = pubsub_options_for(subscriber_identity);
   subscriber_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   subscriber_options.limits.pubsub.limits.max_data_size = 4;

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.outbound-generation"};
   const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      co_return pubsub::validation_result::accept;
   };
   forge::asio::blocking::run(runtime, publisher.async_subscribe(subject, accept));
   forge::asio::blocking::run(runtime, subscriber.async_subscribe(subject, accept));
   forge::asio::blocking::run(
       runtime,
       publisher.async_connect(subscriber_endpoint, node::connect_options{.expected_peer = subscriber.local_peer()}));

   auto inbound = forge::asio::blocking::run(
       runtime, subscriber.async_open_protocol_stream(publisher.local_peer(), builtins::meshsub_v11));
   const auto subscribe_and_graft = pubsub::codec::encode(pubsub::rpc{
       .subscriptions = {pubsub::subscription{.subscribe = true, .subject = subject}},
       .control_value = pubsub::control{.grafts = {pubsub::control::graft{.subject = subject}}},
   });
   forge::asio::blocking::run(runtime, inbound.async_write(subscribe_and_graft));
   for (auto poll = 0;
        poll < 200 && (publisher.pubsub_snapshot().peers != 1U || publisher.pubsub_snapshot().mesh_edges != 1U);
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub explicit inbound generation");
   }
   BOOST_REQUIRE(publisher.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(publisher.pubsub_snapshot().mesh_edges == 1U);
   const auto active_sessions = publisher.metrics().active_sessions;
   BOOST_REQUIRE(active_sessions > 0U);

   forge::asio::blocking::run(runtime,
                              publisher.async_publish(subject, std::vector<std::uint8_t>(8, std::uint8_t{0x41U})));
   for (auto poll = 0; poll < 200 && subscriber.metrics().protocol_rejections < 1U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub G1 remote reset");
   }
   BOOST_REQUIRE(subscriber.metrics().protocol_rejections >= 1U);
   const auto g1_streams = publisher.metrics().protocol_streams_opened;

   for (auto attempt = 0; attempt < 50 && publisher.metrics().protocol_streams_opened <= g1_streams; ++attempt) {
      try {
         static_cast<void>(forge::asio::blocking::run(
             runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{static_cast<std::uint8_t>(attempt)})));
      } catch (const forge::exceptions::base&) {
      }
      if (publisher.metrics().protocol_streams_opened <= g1_streams) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub G1 reset propagation");
      }
   }
   BOOST_REQUIRE(publisher.metrics().protocol_streams_opened > g1_streams);
   BOOST_REQUIRE(publisher.pubsub_snapshot().mesh_edges == 0U);
   BOOST_REQUIRE(publisher.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(subscriber.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(publisher.metrics().active_sessions == active_sessions);
   const auto g2_streams = publisher.metrics().protocol_streams_opened;

   forge::asio::blocking::run(runtime,
                              publisher.async_publish(subject, std::vector<std::uint8_t>(8, std::uint8_t{0x42U})));
   for (auto poll = 0; poll < 400 && subscriber.metrics().protocol_rejections < 2U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub G2 remote reset");
   }
   BOOST_REQUIRE(subscriber.metrics().protocol_rejections >= 2U);
   for (auto attempt = 0; attempt < 50 && publisher.metrics().protocol_streams_opened <= g2_streams; ++attempt) {
      try {
         static_cast<void>(forge::asio::blocking::run(
             runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{static_cast<std::uint8_t>(attempt)})));
      } catch (const forge::exceptions::base&) {
      }
      if (publisher.metrics().protocol_streams_opened <= g2_streams) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub failed G2 cleanup");
      }
   }
   BOOST_REQUIRE(publisher.metrics().protocol_streams_opened > g2_streams);
   BOOST_REQUIRE(publisher.pubsub_snapshot().mesh_edges == 0U);
   BOOST_REQUIRE(publisher.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(publisher.metrics().active_sessions == active_sessions);

   forge::asio::blocking::run(runtime, inbound.async_close());
   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_rejects_publish_after_cached_stream_shutdown) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_test_identity();
   const auto subscriber_identity = make_test_identity();
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   auto publisher = node{runtime, options_for(publisher_identity, pubsub_capabilities)};
   auto subscriber = node{runtime, options_for(subscriber_identity, pubsub_capabilities)};
   const auto subscriber_endpoint = listen_tcp(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint, pubsub_capabilities);

   const auto subject = pubsub::topic{.value = "forge.pubsub.shutdown"};
   auto delivered = std::make_shared<std::promise<void>>();
   auto delivery = delivered->get_future();
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    subject, [delivered](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                       delivered->set_value();
                       co_return pubsub::validation_result::accept;
                    }));
   forge::asio::blocking::run(
       runtime,
       publisher.async_connect(subscriber_endpoint, node::connect_options{.expected_peer = subscriber.local_peer()}));
   forge::asio::blocking::run(runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{0x42U}));
   BOOST_REQUIRE(delivery.wait_for(std::chrono::seconds{5}) == std::future_status::ready);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   BOOST_TEST(publisher.metrics().stopped);
   try {
      static_cast<void>(
          forge::asio::blocking::run(runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{0x43U})));
      BOOST_FAIL("GossipSub publish should reject after clean shutdown");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::closed));
   }

   const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (subscriber.metrics().active_sessions != 0U) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < close_deadline);
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "orderly GossipSub session close");
   }
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);

   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_separates_immediate_source_from_signed_author) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto author_identity = make_test_certificate_identity("pubsub-signed-author");
   const auto relay_identity = make_test_certificate_identity("pubsub-immediate-relay");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-source-subscriber");
   auto author = node{runtime, pubsub_options_for(author_identity)};
   auto relay = node{runtime, pubsub_options_for(relay_identity)};
   auto subscriber = node{runtime, pubsub_options_for(subscriber_identity)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   relay.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.source-author"};
   auto delivered = std::make_shared<std::promise<std::pair<peer_id, std::optional<peer_id>>>>();
   auto future = delivered->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [delivered](pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              delivered->set_value({event.source, event.value.from});
              co_return pubsub::validation_result::accept;
           }));

   const auto published =
       forge::asio::blocking::run(runtime, author.async_publish(subject, std::vector<std::uint8_t>{1, 2, 3}));
   BOOST_REQUIRE(published.from.has_value());
   BOOST_TEST(published.from->to_string() == author.local_peer().to_string());
   auto relay_stream = forge::asio::blocking::run(
       runtime, relay.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(runtime,
                              relay_stream.async_write(pubsub::codec::encode(pubsub::rpc{.messages = {published}})));

   BOOST_REQUIRE(future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   const auto [source, signed_author] = future.get();
   BOOST_TEST(source.to_string() == relay.local_peer().to_string());
   BOOST_REQUIRE(signed_author.has_value());
   BOOST_TEST(signed_author->to_string() == author.local_peer().to_string());

   forge::asio::blocking::run(runtime, relay_stream.async_close());
   forge::asio::blocking::run(runtime, author.async_stop());
   forge::asio::blocking::run(runtime, relay.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_is_redelivered_after_bounded_cooldown) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for();
   auto subscriber_options = pubsub_options_for();
   publisher_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.explicit_peer_id = peer(153);
   for (auto* options : {&publisher_options, &subscriber_options}) {
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
      options->limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
      options->limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{40};
      options->limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{80};
      options->limits.pubsub.limits.history_gossip = 1;
   }

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   (void)listen(publisher, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.retry"};
   forge::asio::blocking::run(
       runtime,
       publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto first_attempt = std::make_shared<std::promise<void>>();
   auto first_attempt_future = first_attempt->get_future();
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    subject,
                    [attempts, first_attempt,
                     accepted](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                       const auto attempt = attempts->fetch_add(1, std::memory_order_relaxed);
                       if (attempt == 0) {
                          first_attempt->set_value();
                          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
                          timer.expires_after(std::chrono::milliseconds{25});
                          boost::system::error_code ec;
                          co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                          co_return pubsub::validation_result::retry;
                       }
                       if (attempt == 1) {
                          accepted->set_value();
                       }
                       co_return pubsub::validation_result::accept;
                    }));

   (void)forge::asio::blocking::run(runtime,
                                    publisher.async_publish(subject, std::vector<std::uint8_t>{'r', 'e', 't', 'r', 'y'},
                                                            pubsub::publish_options{.sign = false}));
   BOOST_REQUIRE(first_attempt_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   first_attempt_future.get();
   const auto noise_subject = pubsub::topic{.value = "forge.pubsub.retry.noise"};
   for (auto value = std::uint8_t{}; value < 8; ++value) {
      (void)forge::asio::blocking::run(runtime, publisher.async_publish(noise_subject, std::vector<std::uint8_t>{value},
                                                                        pubsub::publish_options{.sign = false}));
   }

   if (accepted_future.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
      const auto snapshot = subscriber.pubsub_snapshot();
      BOOST_FAIL("retryable PubSub message was not redelivered; attempts="
                 << attempts->load(std::memory_order_relaxed) << " received=" << snapshot.messages_received
                 << " delivered=" << snapshot.messages_delivered << " duplicates=" << snapshot.duplicates
                 << " controls=" << snapshot.control_messages);
   }
   accepted_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{50}, "post-redelivery accounting");

   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 1U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_ignore_remains_terminal_during_history_window) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for();
   auto subscriber_options = pubsub_options_for();
   publisher_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   for (auto* options : {&publisher_options, &subscriber_options}) {
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
      options->limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
      options->limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{20};
      options->limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{40};
   }

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   (void)listen(publisher, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.ignore"};
   forge::asio::blocking::run(
       runtime,
       publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));
   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto ignored = std::make_shared<std::promise<void>>();
   auto ignored_future = ignored->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [attempts, ignored](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              if (attempts->fetch_add(1, std::memory_order_relaxed) == 0) {
                 ignored->set_value();
              }
              co_return pubsub::validation_result::ignore;
           }));

   (void)forge::asio::blocking::run(
       runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{'i', 'g', 'n', 'o', 'r', 'e'},
                                        pubsub::publish_options{.sign = false}));
   BOOST_REQUIRE(ignored_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   ignored_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{250}, "terminal ignore window");

   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 0U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_rejects_signed_equivocation_for_same_message_id) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_test_certificate_identity("pubsub-retry-equivocation-publisher");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-retry-equivocation-subscriber");
   auto subscriber_options = pubsub_options_for(subscriber_identity);
   subscriber_options.limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{40};
   subscriber_options.limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{40};
   auto publisher_options = pubsub_options_for(publisher_identity);

   auto subscriber = node{runtime, std::move(subscriber_options)};
   auto publisher = node{runtime, std::move(publisher_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.retry.equivocation"};
   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto ignored = std::make_shared<std::promise<void>>();
   auto ignored_future = ignored->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [attempts, ignored](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              if (attempts->fetch_add(1, std::memory_order_relaxed) == 0) {
                 ignored->set_value();
              }
              co_return pubsub::validation_result::retry;
           }));

   auto first = pubsub::message{
       .data = std::vector<std::uint8_t>{'f', 'i', 'r', 's', 't'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 17},
       .subject = subject,
   };
   const auto private_key = forge::crypto::pki::pem::read_private_key(publisher_identity.private_key_pem);
   pubsub::codec::sign_message(first, private_key);
   auto second = first;
   second.data = std::vector<std::uint8_t>{'s', 'e', 'c', 'o', 'n', 'd'};
   pubsub::codec::sign_message(second, private_key);
   BOOST_REQUIRE(pubsub::codec::verify_message(first));
   BOOST_REQUIRE(pubsub::codec::verify_message(second));
   BOOST_TEST(pubsub::codec::message_id(first) == pubsub::codec::message_id(second), boost::test_tools::per_element());

   auto stream = forge::asio::blocking::run(
       runtime, publisher.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(runtime, stream.async_write(pubsub::codec::encode(
                                           pubsub::rpc{.messages = std::vector<pubsub::message>{first}})));
   BOOST_REQUIRE(ignored_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   ignored_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{60}, "signed equivocation retry cooldown");

   forge::asio::blocking::run(runtime, stream.async_write(pubsub::codec::encode(
                                           pubsub::rpc{.messages = std::vector<pubsub::message>{second}})));
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "signed equivocation accounting");

   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages >= 1U);
   BOOST_TEST(subscriber.metrics().protocol_rejections >= 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 0U);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_accepts_equivalent_signed_envelope_without_inline_key) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_test_certificate_identity("pubsub-retry-envelope-publisher");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-retry-envelope-subscriber");
   auto publisher_options = pubsub_options_for(publisher_identity);
   auto subscriber_options = pubsub_options_for(subscriber_identity);
   subscriber_options.limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{40};
   subscriber_options.limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{40};

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.retry.envelope"};
   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto retryable = std::make_shared<std::promise<void>>();
   auto retryable_future = retryable->get_future();
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject,
           [attempts, retryable, accepted](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              const auto attempt = attempts->fetch_add(1, std::memory_order_relaxed);
              if (attempt == 0) {
                 retryable->set_value();
                 co_return pubsub::validation_result::retry;
              }
              accepted->set_value();
              co_return pubsub::validation_result::accept;
           }));

   const auto author = make_test_identity();
   auto with_key = pubsub::message{
       .data = std::vector<std::uint8_t>{'e', 'n', 'v', 'e', 'l', 'o', 'p', 'e'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 18},
       .subject = subject,
   };
   pubsub::codec::sign_message(with_key, author.private_key);
   auto without_key = with_key;
   without_key.key.clear();
   BOOST_REQUIRE(pubsub::codec::verify_message(with_key));
   BOOST_REQUIRE(pubsub::codec::verify_message(without_key));

   auto stream = forge::asio::blocking::run(
       runtime, publisher.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(runtime, stream.async_write(pubsub::codec::encode(
                                           pubsub::rpc{.messages = std::vector<pubsub::message>{with_key}})));
   BOOST_REQUIRE(retryable_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   retryable_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{60}, "equivalent envelope retry cooldown");
   forge::asio::blocking::run(runtime, stream.async_write(pubsub::codec::encode(
                                           pubsub::rpc{.messages = std::vector<pubsub::message>{without_key}})));

   BOOST_REQUIRE(accepted_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   accepted_future.get();
   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 1U);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_in_progress_validation_is_not_replaced_after_cache_pressure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   const auto first_transport_identity = make_test_certificate_identity("pubsub-validation-first-transport");
   const auto competing_transport_identity = make_test_certificate_identity("pubsub-validation-competing-transport");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-validation-pressure-subscriber");
   auto first_options = pubsub_options_for(first_transport_identity);
   auto competing_options = pubsub_options_for(competing_transport_identity);
   auto subscriber_options = pubsub_options_for(subscriber_identity);
   subscriber_options.limits.pubsub.limits.history_length = 1;
   subscriber_options.limits.pubsub.limits.max_messages = 1;

   auto first_transport = node{runtime, std::move(first_options)};
   auto competing_transport = node{runtime, std::move(competing_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   first_transport.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                          capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   competing_transport.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                              capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.validation.pressure"};
   auto first_entered = std::make_shared<std::promise<void>>();
   auto first_entered_future = first_entered->get_future();
   auto second_payloads = std::make_shared<std::atomic_uint64_t>(0);
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    subject,
                    [first_entered, second_payloads](
                        pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                       if (event.value.data == std::vector<std::uint8_t>{'f', 'i', 'r', 's', 't'}) {
                          first_entered->set_value();
                          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
                          timer.expires_after(std::chrono::milliseconds{300});
                          co_await timer.async_wait(boost::asio::use_awaitable);
                       } else if (event.value.data == std::vector<std::uint8_t>{'s', 'e', 'c', 'o', 'n', 'd'}) {
                          second_payloads->fetch_add(1, std::memory_order_relaxed);
                       }
                       co_return pubsub::validation_result::accept;
                    }));

   const auto author = make_test_identity();
   auto first = pubsub::message{
       .data = std::vector<std::uint8_t>{'f', 'i', 'r', 's', 't'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 19},
       .subject = subject,
   };
   pubsub::codec::sign_message(first, author.private_key);
   auto second = first;
   second.data = std::vector<std::uint8_t>{'s', 'e', 'c', 'o', 'n', 'd'};
   pubsub::codec::sign_message(second, author.private_key);
   auto filler = pubsub::message{
       .data = std::vector<std::uint8_t>{'f', 'i', 'l', 'l', 'e', 'r'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 20},
       .subject = subject,
   };
   pubsub::codec::sign_message(filler, author.private_key);

   auto first_stream = forge::asio::blocking::run(
       runtime, first_transport.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   auto competing_stream = forge::asio::blocking::run(
       runtime, competing_transport.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(runtime, first_stream.async_write(pubsub::codec::encode(
                                           pubsub::rpc{.messages = std::vector<pubsub::message>{first}})));
   BOOST_REQUIRE(first_entered_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   first_entered_future.get();
   forge::asio::blocking::run(runtime, competing_stream.async_write(pubsub::codec::encode(
                                           pubsub::rpc{.messages = std::vector<pubsub::message>{filler}})));
   forge::asio::blocking::run(runtime, competing_stream.async_write(pubsub::codec::encode(
                                           pubsub::rpc{.messages = std::vector<pubsub::message>{second}})));
   wait_on_runtime(runtime, std::chrono::milliseconds{500}, "in-progress validation completion");

   BOOST_TEST(second_payloads->load(std::memory_order_relaxed) == 0U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages >= 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 2U);

   forge::asio::blocking::run(runtime, first_stream.async_close());
   forge::asio::blocking::run(runtime, competing_stream.async_close());
   forge::asio::blocking::run(runtime, first_transport.async_stop());
   forge::asio::blocking::run(runtime, competing_transport.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_proactive_redelivery_stops_after_backpressure_budget) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   const auto occupying_identity = make_test_certificate_identity("pubsub-redelivery-occupying");
   const auto retrying_identity = make_test_certificate_identity("pubsub-redelivery-retrying");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-redelivery-subscriber");
   auto occupying_options = pubsub_options_for(occupying_identity);
   auto retrying_options = pubsub_options_for(retrying_identity);
   auto subscriber_options = pubsub_options_for(subscriber_identity);
   subscriber_options.limits.pubsub.limits.max_validation_queue = 1;
   subscriber_options.limits.pubsub.limits.max_validation_redeliveries = 2;
   subscriber_options.limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{20};
   subscriber_options.limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{20};

   auto occupying = node{runtime, std::move(occupying_options)};
   auto retrying = node{runtime, std::move(retrying_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   occupying.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   retrying.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                   capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.redelivery.budget"};
   auto occupying_entered = std::make_shared<std::promise<void>>();
   auto occupying_entered_future = occupying_entered->get_future();
   auto retrying_callbacks = std::make_shared<std::atomic_uint64_t>(0);
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    subject,
                    [occupying_entered, retrying_callbacks](
                        pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                       if (event.value.data == std::vector<std::uint8_t>{'o', 'c', 'c', 'u', 'p', 'y'}) {
                          occupying_entered->set_value();
                          auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
                          timer.expires_after(std::chrono::milliseconds{400});
                          co_await timer.async_wait(boost::asio::use_awaitable);
                       } else {
                          retrying_callbacks->fetch_add(1, std::memory_order_relaxed);
                       }
                       co_return pubsub::validation_result::accept;
                    }));

   const auto author = make_test_identity();
   auto occupying_message = pubsub::message{
       .data = std::vector<std::uint8_t>{'o', 'c', 'c', 'u', 'p', 'y'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 21},
       .subject = subject,
   };
   pubsub::codec::sign_message(occupying_message, author.private_key);
   auto retrying_message = pubsub::message{
       .data = std::vector<std::uint8_t>{'r', 'e', 't', 'r', 'y'},
       .seqno = std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 22},
       .subject = subject,
   };
   pubsub::codec::sign_message(retrying_message, author.private_key);

   auto occupying_stream = forge::asio::blocking::run(
       runtime, occupying.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   auto retrying_stream = forge::asio::blocking::run(
       runtime, retrying.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(
       runtime, occupying_stream.async_write(pubsub::codec::encode(pubsub::rpc{.messages = {occupying_message}})));
   BOOST_REQUIRE(occupying_entered_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   occupying_entered_future.get();

   for (auto delivery = 0; delivery < 4; ++delivery) {
      forge::asio::blocking::run(
          runtime, retrying_stream.async_write(pubsub::codec::encode(pubsub::rpc{.messages = {retrying_message}})));
      wait_on_runtime(runtime, std::chrono::milliseconds{40}, "proactive redelivery");
   }
   wait_on_runtime(runtime, std::chrono::milliseconds{450}, "validation queue release");
   forge::asio::blocking::run(
       runtime, retrying_stream.async_write(pubsub::codec::encode(pubsub::rpc{.messages = {retrying_message}})));
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "post-redelivery-budget delivery");

   BOOST_TEST(retrying_callbacks->load(std::memory_order_relaxed) == 0U);
   BOOST_TEST(subscriber.metrics().backpressure_rejections == 3U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);

   forge::asio::blocking::run(runtime, occupying_stream.async_close());
   forge::asio::blocking::run(runtime, retrying_stream.async_close());
   forge::asio::blocking::run(runtime, occupying.async_stop());
   forge::asio::blocking::run(runtime, retrying.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_source_is_not_replaced_by_foreign_ihave) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   const auto publisher_identity = make_test_certificate_identity("pubsub-retry-source-publisher");
   const auto subscriber_identity = make_test_certificate_identity("pubsub-retry-source-subscriber");
   const auto attacker_identity = make_test_certificate_identity("pubsub-retry-source-attacker");
   auto publisher_options = pubsub_options_for(publisher_identity);
   auto subscriber_options = pubsub_options_for(subscriber_identity);
   auto attacker_options = pubsub_options_for(attacker_identity);
   publisher_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   attacker_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{500};
   subscriber_options.limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{100};
   subscriber_options.limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{100};
   subscriber_options.limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{100};
   subscriber_options.limits.pubsub.limits.max_validation_requests = 2;

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   auto attacker = node{runtime, std::move(attacker_options)};
   (void)listen(publisher, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);
   (void)listen(attacker, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   attacker.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                   capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.retry.source"};
   forge::asio::blocking::run(
       runtime,
       publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto ignored = std::make_shared<std::promise<void>>();
   auto ignored_future = ignored->get_future();
   auto accepted = std::make_shared<std::promise<peer_id>>();
   auto accepted_future = accepted->get_future();
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(subject,
                                           [attempts, ignored, accepted](pubsub::event event) mutable
                                               -> boost::asio::awaitable<pubsub::validation_result> {
                                              const auto attempt = attempts->fetch_add(1, std::memory_order_relaxed);
                                              if (attempt == 0) {
                                                 ignored->set_value();
                                                 co_return pubsub::validation_result::retry;
                                              }
                                              if (attempt == 1) {
                                                 accepted->set_value(event.source);
                                              }
                                              co_return pubsub::validation_result::accept;
                                           }));

   const auto published = forge::asio::blocking::run(
       runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{'s', 'o', 'u', 'r', 'c', 'e'},
                                        pubsub::publish_options{.sign = false}));
   BOOST_REQUIRE(ignored_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   ignored_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{120}, "foreign IHAVE injection window");

   auto attacker_stream = forge::asio::blocking::run(
       runtime, attacker.async_open_protocol_stream(subscriber.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(
       runtime,
       attacker_stream.async_write(pubsub::codec::encode(pubsub::rpc{
           .control_value =
               pubsub::control{
                   .have = std::vector<pubsub::control::ihave>{pubsub::control::ihave{
                       .subject = subject,
                       .message_ids = std::vector<std::vector<std::uint8_t>>{pubsub::codec::message_id(published)}}},
               },
       })));

   BOOST_REQUIRE(accepted_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   BOOST_TEST(accepted_future.get().to_string() == publisher.local_peer().to_string());
   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 1U);

   forge::asio::blocking::run(runtime, attacker_stream.async_close());
   forge::asio::blocking::run(runtime, attacker.async_stop());
   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_stops_after_max_attempts_without_duplicate_history) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for();
   auto subscriber_options = pubsub_options_for();
   publisher_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.explicit_peer_id = peer(155);
   for (auto* options : {&publisher_options, &subscriber_options}) {
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
      options->limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
      options->limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{20};
      options->limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{40};
      options->limits.pubsub.limits.max_validation_attempts = 3;
   }
   subscriber_options.limits.pubsub.limits.history_length = 1;
   subscriber_options.limits.pubsub.limits.max_messages = 1;

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   (void)listen(publisher, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.retry.limit"};
   forge::asio::blocking::run(
       runtime,
       publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto exhausted = std::make_shared<std::promise<void>>();
   auto exhausted_future = exhausted->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [attempts, exhausted](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              if (attempts->fetch_add(1, std::memory_order_relaxed) == 2) {
                 exhausted->set_value();
              }
              co_return pubsub::validation_result::retry;
           }));

   (void)forge::asio::blocking::run(runtime,
                                    publisher.async_publish(subject, std::vector<std::uint8_t>{'l', 'i', 'm', 'i', 't'},
                                                            pubsub::publish_options{.sign = false}));
   BOOST_REQUIRE(exhausted_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   exhausted_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{250}, "terminal retry limit window");

   const auto snapshot = subscriber.pubsub_snapshot();
   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 3U);
   BOOST_TEST(snapshot.messages_delivered == 0U);
   BOOST_TEST(snapshot.cached_messages == 1U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_unavailable_source_stops_retry_requests_after_limit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for();
   auto subscriber_options = pubsub_options_for();
   publisher_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.explicit_peer_id = peer(156);
   for (auto* options : {&publisher_options, &subscriber_options}) {
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
      options->limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
      options->limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{100};
      options->limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{100};
   }
   publisher_options.limits.pubsub.limits.history_length = 1;
   publisher_options.limits.pubsub.limits.max_messages = 1;
   subscriber_options.limits.pubsub.limits.max_validation_requests = 2;

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   (void)listen(publisher, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.retry.unavailable"};
   forge::asio::blocking::run(
       runtime,
       publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto ignored = std::make_shared<std::promise<void>>();
   auto ignored_future = ignored->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [attempts, ignored](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              if (attempts->fetch_add(1, std::memory_order_relaxed) == 0) {
                 ignored->set_value();
              }
              co_return pubsub::validation_result::retry;
           }));

   (void)forge::asio::blocking::run(
       runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{'u', 'n', 'a', 'v', 'a', 'i', 'l'},
                                        pubsub::publish_options{.sign = false}));
   BOOST_REQUIRE(ignored_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   ignored_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{25}, "initial ignore completion");

   const auto noise_subject = pubsub::topic{.value = "forge.pubsub.retry.eviction"};
   (void)forge::asio::blocking::run(runtime, publisher.async_publish(noise_subject, std::vector<std::uint8_t>{0xff},
                                                                     pubsub::publish_options{.sign = false}));
   const auto control_before = publisher.pubsub_snapshot().control_messages;
   for (auto poll = 0; poll < 100 && publisher.pubsub_snapshot().control_messages < control_before + 2; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{20}, "bounded unavailable-source retry");
   }
   const auto control_at_limit = publisher.pubsub_snapshot().control_messages;
   BOOST_REQUIRE(control_at_limit >= control_before + 2);

   wait_on_runtime(runtime, std::chrono::milliseconds{350}, "post-request-limit window");
   BOOST_TEST(publisher.pubsub_snapshot().control_messages == control_at_limit);
   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 1U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_reject_remains_terminal_during_history_window) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for();
   auto subscriber_options = pubsub_options_for();
   publisher_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   subscriber_options.explicit_peer_id = peer(154);
   for (auto* options : {&publisher_options, &subscriber_options}) {
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
      options->limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
      options->limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{30};
      options->limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{60};
   }

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   (void)listen(publisher, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.pubsub.reject"};
   forge::asio::blocking::run(
       runtime,
       publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto rejected = std::make_shared<std::promise<void>>();
   auto rejected_future = rejected->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [attempts, rejected](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              const auto attempt = attempts->fetch_add(1, std::memory_order_relaxed);
              if (attempt == 0) {
                 rejected->set_value();
              }
              co_return pubsub::validation_result::reject;
           }));

   (void)forge::asio::blocking::run(
       runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{'r', 'e', 'j', 'e', 'c', 't'},
                                        pubsub::publish_options{.sign = false}));
   BOOST_REQUIRE(rejected_future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   rejected_future.get();
   wait_on_runtime(runtime, std::chrono::milliseconds{250}, "terminal rejection gossip window");

   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 0U);
   BOOST_TEST(subscriber.pubsub_snapshot().invalid_messages >= 1U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_forwards_between_subscribed_peers) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   auto publisher_options = pubsub_options_for();
   auto hub_options = pubsub_options_for();
   auto subscriber_options = pubsub_options_for();
   hub_options.explicit_peer_id = peer(151);
   subscriber_options.explicit_peer_id = peer(152);

   auto publisher = node{runtime, std::move(publisher_options)};
   auto hub = node{runtime, std::move(hub_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   const auto hub_endpoint = listen(hub, runtime);
   const auto subscriber_endpoint = listen(subscriber, runtime);

   publisher.peers().learn_endpoint(hub.local_peer(), hub_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   subscriber.peers().learn_endpoint(hub.local_peer(), hub_endpoint,
                                     capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   hub.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                              capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.mesh"};
   forge::asio::blocking::run(
       runtime, hub.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));
   auto received = std::make_shared<std::promise<std::vector<std::uint8_t>>>();
   auto future = received->get_future();
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [received](pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              received->set_value(event.value.data);
              co_return pubsub::validation_result::accept;
           }));
   for (auto poll = 0; poll < 200 && hub.pubsub_snapshot().mesh_edges == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub heartbeat GRAFT");
   }
   BOOST_REQUIRE(hub.pubsub_snapshot().mesh_edges >= 1U);

   forge::asio::blocking::run(runtime, publisher.async_publish(subject, std::vector<std::uint8_t>{'m', 'e', 's', 'h'}));

   if (future.wait_for(std::chrono::milliseconds{5'000}) != std::future_status::ready) {
      const auto hub_metrics = hub.metrics();
      const auto subscriber_metrics = subscriber.metrics();
      BOOST_FAIL("pubsub forwarding did not finish; hub_received="
                 << hub_metrics.pubsub_messages_received << " hub_delivered=" << hub_metrics.pubsub_messages_delivered
                 << " subscriber_received=" << subscriber_metrics.pubsub_messages_received
                 << " subscriber_delivered=" << subscriber_metrics.pubsub_messages_delivered
                 << " subscriber_invalid=" << subscriber_metrics.pubsub_invalid_messages);
   }
   BOOST_TEST(future.get() == std::vector<std::uint8_t>({'m', 'e', 's', 'h'}), boost::test_tools::per_element());
   for (auto poll = 0; poll < 200 && subscriber.pubsub_snapshot().messages_delivered == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub delivery accounting");
   }
   BOOST_TEST(hub.pubsub_snapshot().mesh_edges >= 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered >= 1U);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, hub.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_subscription_is_idempotent_and_does_not_create_mesh_edge) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for();
   server_options.limits.resources.protocol.max_streams = 1;
   server_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   auto client_options = pubsub_options_for();
   client_options.explicit_peer_id = peer(219);

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto subject = pubsub::topic{.value = "forge.subscription.idempotent"};
   forge::asio::blocking::run(
       runtime, server.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));
   const auto denied_before = server.diagnostics().resources.denied_streams;

   const auto subscribe = pubsub::codec::encode(pubsub::rpc{
       .subscriptions =
           std::vector<pubsub::subscription>{
               pubsub::subscription{.subscribe = true, .subject = subject},
           },
   });
   forge::asio::blocking::run(runtime, stream.async_write(subscribe));
   forge::asio::blocking::run(runtime, stream.async_write(subscribe));
   for (auto poll = 0; poll < 100 && server.pubsub_snapshot().peers == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub subscription processing");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(server.diagnostics().resources.denied_streams == denied_before);

   const auto graft = pubsub::codec::encode(pubsub::rpc{
       .control_value =
           pubsub::control{
               .grafts =
                   std::vector<pubsub::control::graft>{
                       pubsub::control::graft{.subject = subject},
                   },
           },
   });
   forge::asio::blocking::run(runtime, stream.async_write(graft));
   for (auto poll = 0; poll < 100 && server.pubsub_snapshot().mesh_edges == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub GRAFT processing");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);

   forge::asio::blocking::run(runtime, stream.async_write(graft));
   wait_on_runtime(runtime, std::chrono::milliseconds{25}, "duplicate GossipSub GRAFT processing");
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 1U);

   const auto unsubscribe = pubsub::codec::encode(pubsub::rpc{
       .subscriptions =
           std::vector<pubsub::subscription>{
               pubsub::subscription{.subscribe = false, .subject = subject},
           },
   });
   forge::asio::blocking::run(runtime, stream.async_write(unsubscribe));
   for (auto poll = 0; poll < 100 && server.pubsub_snapshot().mesh_edges != 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub unsubscribe processing");
   }
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   forge::asio::blocking::run(runtime, stream.async_write(unsubscribe));
   wait_on_runtime(runtime, std::chrono::milliseconds{25}, "duplicate GossipSub unsubscribe processing");
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_graft_at_topic_subscription_cap_sends_prune_without_state_or_subscribe_loop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   auto server_options = pubsub_options_for(make_test_certificate_identity("graft-capacity-server"));
   server_options.limits.pubsub.limits.max_peers_per_topic = 1;
   server_options.limits.pubsub.limits.prune_backoff = std::chrono::seconds{1};
   server_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   server_options.limits.resources.max_malformed_messages_per_peer = 1;
   auto occupying_options = pubsub_options_for(make_test_certificate_identity("graft-capacity-occupying"));
   occupying_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   auto rejected_options = pubsub_options_for(make_test_certificate_identity("graft-capacity-rejected"));
   rejected_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};

   auto server = node{runtime, std::move(server_options)};
   auto occupying = node{runtime, std::move(occupying_options)};
   auto rejected = node{runtime, std::move(rejected_options)};
   register_echo(server);
   const auto server_endpoint = listen(server, runtime);
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   occupying.peers().learn_endpoint(server.local_peer(), server_endpoint, pubsub_capabilities);
   rejected.peers().learn_endpoint(server.local_peer(), server_endpoint, pubsub_capabilities);

   const auto subject = pubsub::topic{.value = "forge.graft.capacity"};
   const auto warmup_subject = pubsub::topic{.value = "forge.graft.capacity.warmup"};
   const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      co_return pubsub::validation_result::accept;
   };
   forge::asio::blocking::run(runtime, server.async_subscribe(subject, accept));

   auto occupying_stream = forge::asio::blocking::run(
       runtime, occupying.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   auto rejected_stream = forge::asio::blocking::run(
       runtime, rejected.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(runtime,
                              rejected_stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                  .subscriptions = {pubsub::subscription{.subscribe = true, .subject = warmup_subject}},
                              })));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().peers != 1U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub PRUNE outbound warmup subscription");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);

   const auto warmup_messages_before = rejected.pubsub_snapshot().messages_received;
   static_cast<void>(
       forge::asio::blocking::run(runtime, server.async_publish(warmup_subject, std::vector<std::uint8_t>{0x37U})));
   for (auto poll = 0; poll < 200 && rejected.pubsub_snapshot().messages_received == warmup_messages_before; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub PRUNE outbound warmup delivery");
   }
   BOOST_REQUIRE(rejected.pubsub_snapshot().messages_received == warmup_messages_before + 1U);

   forge::asio::blocking::run(runtime,
                              occupying_stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                  .subscriptions = {pubsub::subscription{.subscribe = true, .subject = subject}},
                              })));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().peers != 2U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub topic capacity fill");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 2U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 0U);

   const auto before_capacity_subscribe = server.metrics();
   const auto peers_before_capacity_subscribe = server.pubsub_snapshot().peers;
   forge::asio::blocking::run(runtime,
                              rejected_stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                  .subscriptions = {pubsub::subscription{.subscribe = true, .subject = subject}},
                              })));
   for (auto poll = 0;
        poll < 200 && server.metrics().backpressure_rejections == before_capacity_subscribe.backpressure_rejections;
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub capacity SUBSCRIBE rejection");
   }
   BOOST_REQUIRE(server.metrics().backpressure_rejections == before_capacity_subscribe.backpressure_rejections + 1U);
   BOOST_TEST(server.metrics().protocol_rejections == before_capacity_subscribe.protocol_rejections + 1U);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == before_capacity_subscribe.pubsub_invalid_messages);
   BOOST_TEST(server.metrics().active_sessions == before_capacity_subscribe.active_sessions);
   BOOST_TEST(server.pubsub_snapshot().peers == peers_before_capacity_subscribe);

   const auto server_before = server.pubsub_snapshot();
   const auto rejected_before = rejected.pubsub_snapshot();
   const auto server_metrics_before = server.metrics();
   const auto server_streams_before = server.metrics().protocol_streams_opened;
   BOOST_REQUIRE(rejected_before.mesh_edges == 0U);
   forge::asio::blocking::run(runtime, rejected_stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                           .control_value =
                                               pubsub::control{
                                                   .grafts = {pubsub::control::graft{.subject = subject}},
                                               },
                                       })));
   for (auto poll = 0; poll < 200 && rejected.pubsub_snapshot().control_messages == rejected_before.control_messages;
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub capacity PRUNE delivery");
   }
   wait_on_runtime(runtime, std::chrono::milliseconds{25}, "GossipSub capacity PRUNE stability");

   const auto server_after = server.pubsub_snapshot();
   const auto rejected_after = rejected.pubsub_snapshot();
   BOOST_REQUIRE(rejected_after.control_messages == rejected_before.control_messages + 1U);
   BOOST_TEST(server_after.control_messages == server_before.control_messages + 1U);
   BOOST_TEST(server_after.peers == server_before.peers);
   BOOST_TEST(server_after.mesh_edges == 0U);
   BOOST_TEST(rejected_after.peers == rejected_before.peers);
   BOOST_TEST(rejected_after.mesh_edges == 0U);
   BOOST_TEST(server.metrics().protocol_streams_opened == server_streams_before);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == server_metrics_before.pubsub_invalid_messages);
   BOOST_TEST(server.metrics().backpressure_rejections == server_metrics_before.backpressure_rejections + 1U);
   BOOST_TEST(server.metrics().active_sessions == server_metrics_before.active_sessions);

   forge::asio::blocking::run(runtime, rejected_stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                           .control_value =
                                               pubsub::control{
                                                   .grafts = {pubsub::control::graft{.subject = subject}},
                                               },
                                       })));
   for (auto poll = 0; poll < 200 && rejected.pubsub_snapshot().control_messages == rejected_after.control_messages;
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub backoff violation PRUNE delivery");
   }
   wait_on_runtime(runtime, std::chrono::milliseconds{25}, "GossipSub backoff violation PRUNE stability");
   BOOST_REQUIRE(rejected.pubsub_snapshot().control_messages == rejected_after.control_messages + 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == server_metrics_before.pubsub_invalid_messages + 1U);
   BOOST_TEST(server.metrics().active_sessions == server_metrics_before.active_sessions);

   auto echo = forge::asio::blocking::run(
       runtime, rejected.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                    node::open_options{.allow_relay = false}));
   const auto echo_payload = std::vector<std::uint8_t>{'l', 'i', 'v', 'e'};
   forge::asio::blocking::run(runtime, echo.async_write_frame(echo_payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, echo.async_read_frame()) == echo_payload,
              boost::test_tools::per_element());
   forge::asio::blocking::run(runtime, echo.async_close());

   forge::asio::blocking::run(runtime,
                              occupying_stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                  .subscriptions = {pubsub::subscription{.subscribe = false, .subject = subject}},
                              })));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().peers != 1U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub topic capacity release");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   wait_on_runtime(runtime, std::chrono::milliseconds{1'100}, "GossipSub local PRUNE backoff expiry");
   const auto control_after_violation = rejected.pubsub_snapshot().control_messages;
   forge::asio::blocking::run(runtime, rejected_stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                           .control_value =
                                               pubsub::control{
                                                   .grafts = {pubsub::control::graft{.subject = subject}},
                                               },
                                       })));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().mesh_edges == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub post-backoff GRAFT admission");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(rejected.pubsub_snapshot().control_messages == control_after_violation);

   forge::asio::blocking::run(runtime, occupying_stream.async_close());
   forge::asio::blocking::run(runtime, rejected_stream.async_close());
   forge::asio::blocking::run(runtime, occupying.async_stop());
   forge::asio::blocking::run(runtime, rejected.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_received_prune_blocks_immediate_graft_until_backoff_expiry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for(make_test_certificate_identity("remote-prune-graft-server"));
   auto client_options = pubsub_options_for(make_test_certificate_identity("remote-prune-graft-client"));
   for (auto* options : {&server_options, &client_options}) {
      options->limits.pubsub.limits.prune_backoff = std::chrono::seconds{1};
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   }

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.graft.remote-prune"};
   const auto warmup_subject = pubsub::topic{.value = "forge.graft.remote-prune.warmup"};
   forge::asio::blocking::run(
       runtime, server.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   forge::asio::blocking::run(runtime,
                              stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                  .subscriptions = {pubsub::subscription{.subscribe = true, .subject = warmup_subject}},
                              })));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().peers != 1U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub remote PRUNE stream warmup");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   const auto client_messages_before = client.pubsub_snapshot().messages_received;
   static_cast<void>(
       forge::asio::blocking::run(runtime, server.async_publish(warmup_subject, std::vector<std::uint8_t>{0x42U})));
   for (auto poll = 0; poll < 200 && client.pubsub_snapshot().messages_received == client_messages_before; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub remote PRUNE reverse stream warmup");
   }
   BOOST_REQUIRE(client.pubsub_snapshot().messages_received == client_messages_before + 1U);

   const auto server_control_before_prune = server.pubsub_snapshot().control_messages;
   forge::asio::blocking::run(runtime, stream.async_write(pubsub::codec::encode(pubsub::rpc{
                                           .control_value =
                                               pubsub::control{
                                                   .prunes = {pubsub::control::prune{
                                                       .subject = subject,
                                                       .backoff = std::chrono::seconds{1},
                                                   }},
                                               },
                                       })));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().control_messages == server_control_before_prune; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub remote PRUNE receipt");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().control_messages == server_control_before_prune + 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 0U);

   const auto server_before_graft = server.pubsub_snapshot();
   const auto client_before_graft = client.pubsub_snapshot();
   const auto metrics_before_graft = server.metrics();
   const auto streams_before_graft = server.metrics().protocol_streams_opened;
   const auto graft = pubsub::codec::encode(pubsub::rpc{
       .control_value = pubsub::control{.grafts = {pubsub::control::graft{.subject = subject}}},
   });
   forge::asio::blocking::run(runtime, stream.async_write(graft));
   for (auto poll = 0; poll < 200 && client.pubsub_snapshot().control_messages == client_before_graft.control_messages;
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub remote-backoff PRUNE delivery");
   }

   const auto server_after_rejection = server.pubsub_snapshot();
   const auto client_after_rejection = client.pubsub_snapshot();
   BOOST_REQUIRE(client_after_rejection.control_messages == client_before_graft.control_messages + 1U);
   BOOST_TEST(server_after_rejection.control_messages == server_before_graft.control_messages + 1U);
   BOOST_TEST(server_after_rejection.peers == server_before_graft.peers);
   BOOST_TEST(server_after_rejection.mesh_edges == 0U);
   BOOST_TEST(client_after_rejection.peers == client_before_graft.peers);
   BOOST_TEST(client_after_rejection.mesh_edges == client_before_graft.mesh_edges);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == metrics_before_graft.pubsub_invalid_messages + 1U);
   BOOST_TEST(server.metrics().protocol_rejections == metrics_before_graft.protocol_rejections + 1U);
   BOOST_TEST(server.metrics().protocol_streams_opened == streams_before_graft);

   wait_on_runtime(runtime, std::chrono::milliseconds{1'100}, "GossipSub unified PRUNE backoff expiry");
   const auto server_control_after_expiry = server.pubsub_snapshot().control_messages;
   const auto client_control_after_expiry = client.pubsub_snapshot().control_messages;
   forge::asio::blocking::run(runtime, stream.async_write(graft));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().mesh_edges == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub post-remote-backoff GRAFT admission");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(server.pubsub_snapshot().control_messages == server_control_after_expiry + 1U);
   BOOST_TEST(client.pubsub_snapshot().control_messages == client_control_after_expiry);
   BOOST_TEST(server.metrics().protocol_streams_opened == streams_before_graft);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_unsubscribe_sends_leave_and_enforces_backoff_until_expiry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for(make_test_certificate_identity("unsubscribe-leave-server"));
   auto client_options = pubsub_options_for(make_test_certificate_identity("unsubscribe-leave-client"));
   for (auto* options : {&server_options, &client_options}) {
      options->limits.pubsub.limits.unsubscribe_backoff = std::chrono::seconds{1};
      options->limits.pubsub.limits.prune_backoff = std::chrono::seconds{1};
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   }

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   const auto subject = pubsub::topic{.value = "forge.unsubscribe.leave"};
   const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      co_return pubsub::validation_result::accept;
   };
   forge::asio::blocking::run(runtime, server.async_subscribe(subject, accept));
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto subscribe_and_graft = pubsub::codec::encode(pubsub::rpc{
       .subscriptions = {pubsub::subscription{.subscribe = true, .subject = subject}},
       .control_value = pubsub::control{.grafts = {pubsub::control::graft{.subject = subject}}},
   });
   forge::asio::blocking::run(runtime, stream.async_write(subscribe_and_graft));
   for (auto poll = 0;
        poll < 200 && (server.pubsub_snapshot().peers != 1U || server.pubsub_snapshot().mesh_edges != 1U); ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub unsubscribe mesh setup");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);

   const auto client_messages_before = client.pubsub_snapshot().messages_received;
   static_cast<void>(
       forge::asio::blocking::run(runtime, server.async_publish(subject, std::vector<std::uint8_t>{0x4cU})));
   for (auto poll = 0; poll < 200 && (client.pubsub_snapshot().messages_received == client_messages_before ||
                                      client.pubsub_snapshot().peers != 1U);
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub unsubscribe reverse stream warmup");
   }
   BOOST_REQUIRE(client.pubsub_snapshot().messages_received == client_messages_before + 1U);
   BOOST_REQUIRE(client.pubsub_snapshot().peers == 1U);

   const auto client_before_leave = client.pubsub_snapshot();
   const auto server_streams_before_leave = server.metrics().protocol_streams_opened;
   const auto client_streams_before_leave = client.metrics().protocol_streams_opened;
   forge::asio::blocking::run(runtime, server.async_unsubscribe(subject));
   for (auto poll = 0;
        poll < 200 && (client.pubsub_snapshot().peers != 0U ||
                       client.pubsub_snapshot().control_messages == client_before_leave.control_messages);
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub unsubscribe LEAVE delivery");
   }
   const auto client_after_leave = client.pubsub_snapshot();
   BOOST_REQUIRE(client_after_leave.peers == 0U);
   BOOST_REQUIRE(client_after_leave.control_messages == client_before_leave.control_messages + 1U);
   BOOST_TEST(client_after_leave.mesh_edges == 0U);
   BOOST_TEST(server.pubsub_snapshot().topics == 0U);
   BOOST_TEST(server.pubsub_snapshot().peers == 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(server.metrics().protocol_streams_opened == server_streams_before_leave);
   BOOST_TEST(client.metrics().protocol_streams_opened == client_streams_before_leave);

   forge::asio::blocking::run(runtime, server.async_subscribe(subject, accept));
   for (auto poll = 0; poll < 200 && client.pubsub_snapshot().peers != 1U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub post-unsubscribe resubscribe delivery");
   }
   BOOST_REQUIRE(client.pubsub_snapshot().peers == 1U);
   const auto server_before_reentry = server.pubsub_snapshot();
   const auto client_before_reentry = client.pubsub_snapshot();
   const auto server_metrics_before_reentry = server.metrics();
   const auto graft = pubsub::codec::encode(pubsub::rpc{
       .control_value = pubsub::control{.grafts = {pubsub::control::graft{.subject = subject}}},
   });
   forge::asio::blocking::run(runtime, stream.async_write(graft));
   for (auto poll = 0;
        poll < 200 && client.pubsub_snapshot().control_messages == client_before_reentry.control_messages; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub unsubscribe-backoff PRUNE delivery");
   }
   BOOST_REQUIRE(client.pubsub_snapshot().control_messages == client_before_reentry.control_messages + 1U);
   BOOST_TEST(server.pubsub_snapshot().control_messages == server_before_reentry.control_messages + 1U);
   BOOST_TEST(server.pubsub_snapshot().peers == 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(server.metrics().pubsub_invalid_messages == server_metrics_before_reentry.pubsub_invalid_messages + 1U);
   BOOST_TEST(server.metrics().protocol_streams_opened == server_streams_before_leave);
   BOOST_TEST(client.metrics().protocol_streams_opened == client_streams_before_leave);

   wait_on_runtime(runtime, std::chrono::milliseconds{1'100}, "GossipSub unsubscribe backoff expiry");
   const auto server_control_after_expiry = server.pubsub_snapshot().control_messages;
   const auto client_control_after_expiry = client.pubsub_snapshot().control_messages;
   forge::asio::blocking::run(runtime, stream.async_write(graft));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().mesh_edges == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub post-unsubscribe-backoff GRAFT admission");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(server.pubsub_snapshot().control_messages == server_control_after_expiry + 1U);
   BOOST_TEST(client.pubsub_snapshot().control_messages == client_control_after_expiry);
   BOOST_TEST(server.metrics().protocol_streams_opened == server_streams_before_leave);
   BOOST_TEST(client.metrics().protocol_streams_opened == client_streams_before_leave);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_heartbeat_obeys_sent_and_received_prune_backoff_until_expiry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   auto server_options = pubsub_options_for(make_test_certificate_identity("heartbeat-backoff-server"));
   server_options.limits.pubsub.limits.mesh_n = 1;
   server_options.limits.pubsub.limits.mesh_n_low = 1;
   server_options.limits.pubsub.limits.mesh_n_high = 1;
   server_options.limits.pubsub.limits.prune_backoff = std::chrono::seconds{1};
   server_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{2'000};
   server_options.limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{400};
   auto first_options = pubsub_options_for(make_test_certificate_identity("heartbeat-backoff-first"));
   auto second_options = pubsub_options_for(make_test_certificate_identity("heartbeat-backoff-second"));
   for (auto* options : {&first_options, &second_options}) {
      options->limits.pubsub.limits.prune_backoff = std::chrono::seconds{1};
      options->limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{2'050};
      options->limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{50};
   }

   auto server = node{runtime, std::move(server_options)};
   auto first = node{runtime, std::move(first_options)};
   auto second = node{runtime, std::move(second_options)};
   const auto server_endpoint = listen(server, runtime);
   (void)listen(first, runtime);
   (void)listen(second, runtime);
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   first.peers().learn_endpoint(server.local_peer(), server_endpoint, pubsub_capabilities);
   second.peers().learn_endpoint(server.local_peer(), server_endpoint, pubsub_capabilities);

   const auto subject = pubsub::topic{.value = "forge.graft.heartbeat-backoff"};
   const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      co_return pubsub::validation_result::accept;
   };
   forge::asio::blocking::run(runtime, server.async_subscribe(subject, accept));
   forge::asio::blocking::run(runtime, first.async_subscribe(subject, accept));
   forge::asio::blocking::run(runtime, second.async_subscribe(subject, accept));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().peers != 2U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub heartbeat backoff subscriptions");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 2U);

   auto first_stream = forge::asio::blocking::run(
       runtime, first.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   auto second_stream = forge::asio::blocking::run(
       runtime, second.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto graft = pubsub::codec::encode(pubsub::rpc{
       .control_value = pubsub::control{.grafts = {pubsub::control::graft{.subject = subject}}},
   });
   forge::asio::blocking::run(runtime, first_stream.async_write(graft));
   forge::asio::blocking::run(runtime, second_stream.async_write(graft));
   for (auto poll = 0; poll < 200 && server.pubsub_snapshot().mesh_edges != 2U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub heartbeat oversubscription setup");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 2U);

   const auto first_is_pruned = first.local_peer() > second.local_peer();
   auto* pruned = first_is_pruned ? &first : &second;
   auto* retained = first_is_pruned ? &second : &first;
   auto* pruned_stream = first_is_pruned ? &first_stream : &second_stream;
   auto* retained_stream = first_is_pruned ? &second_stream : &first_stream;
   const auto pruned_control_before = pruned->pubsub_snapshot().control_messages;
   for (auto poll = 0; poll < 600 && pruned->pubsub_snapshot().control_messages == pruned_control_before; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub heartbeat PRUNE delivery");
   }
   BOOST_REQUIRE(pruned->pubsub_snapshot().control_messages == pruned_control_before + 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_REQUIRE(pruned->pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(pruned->pubsub_snapshot().mesh_edges == 0U);

   const auto sessions_before_cleanup = server.metrics().active_sessions;
   BOOST_REQUIRE(sessions_before_cleanup >= 2U);
   forge::asio::blocking::run(runtime, retained_stream->async_close());
   forge::asio::blocking::run(runtime, retained->async_stop());
   for (auto poll = 0;
        poll < 400 && (server.pubsub_snapshot().peers != 1U || server.pubsub_snapshot().mesh_edges != 0U ||
                       server.metrics().active_sessions >= sessions_before_cleanup);
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub retained peer cleanup");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 0U);
   BOOST_REQUIRE(server.metrics().active_sessions + 1U == sessions_before_cleanup);

   const auto server_control_during_backoff = server.pubsub_snapshot().control_messages;
   const auto pruned_control_during_backoff = pruned->pubsub_snapshot().control_messages;
   const auto invalid_before_backoff = server.pubsub_snapshot().invalid_messages;

   wait_on_runtime(runtime, std::chrono::milliseconds{300}, "GossipSub remote PRUNE backoff heartbeat ticks");
   BOOST_TEST(server.pubsub_snapshot().control_messages == server_control_during_backoff);
   BOOST_TEST(pruned->pubsub_snapshot().control_messages == pruned_control_during_backoff);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(pruned->pubsub_snapshot().mesh_edges == 0U);
   BOOST_TEST(server.pubsub_snapshot().invalid_messages == invalid_before_backoff);

   for (auto poll = 0; poll < 400 && server.pubsub_snapshot().control_messages == server_control_during_backoff;
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub remote PRUNE backoff expiry");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().control_messages == server_control_during_backoff + 1U);
   BOOST_REQUIRE(pruned->pubsub_snapshot().control_messages == pruned_control_during_backoff);
   BOOST_TEST(server.pubsub_snapshot().peers == 1U);
   BOOST_TEST(pruned->pubsub_snapshot().peers == 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(pruned->pubsub_snapshot().mesh_edges == 1U);
   BOOST_TEST(server.pubsub_snapshot().invalid_messages == invalid_before_backoff);

   forge::asio::blocking::run(runtime, pruned_stream->async_close());
   forge::asio::blocking::run(runtime, pruned->async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_heartbeat_does_not_graft_unsubscribed_sessions) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for();
   server_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
   server_options.limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
   auto client_options = pubsub_options_for();
   client_options.explicit_peer_id = peer(220);

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto subject = pubsub::topic{.value = "forge.subscription.absent"};
   forge::asio::blocking::run(
       runtime, server.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   wait_on_runtime(runtime, std::chrono::milliseconds{150}, "GossipSub unsubscribed heartbeat stability");
   BOOST_TEST(server.pubsub_snapshot().peers == 0U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_graft_records_subscription_through_heartbeat) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for();
   server_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
   server_options.limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
   auto client_options = pubsub_options_for();
   client_options.explicit_peer_id = peer(225);

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto subject = pubsub::topic{.value = "forge.graft.subscription"};
   forge::asio::blocking::run(
       runtime, server.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   const auto graft = pubsub::codec::encode(pubsub::rpc{
       .control_value =
           pubsub::control{.grafts = std::vector<pubsub::control::graft>{pubsub::control::graft{.subject = subject}}},
   });
   forge::asio::blocking::run(runtime, stream.async_write(graft));
   for (auto poll = 0; poll < 100 && server.pubsub_snapshot().mesh_edges == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub GRAFT-only admission");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);

   forge::asio::blocking::run(runtime, stream.async_write(graft));
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "GossipSub GRAFT-only heartbeat stability");
   BOOST_TEST(server.pubsub_snapshot().peers == 1U);
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 1U);

   const auto unsubscribe = pubsub::codec::encode(pubsub::rpc{
       .subscriptions =
           std::vector<pubsub::subscription>{
               pubsub::subscription{.subscribe = false, .subject = subject},
           },
   });
   forge::asio::blocking::run(runtime, stream.async_write(unsubscribe));
   for (auto poll = 0; poll < 100 && server.pubsub_snapshot().mesh_edges != 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub GRAFT-only unsubscribe");
   }
   BOOST_TEST(server.pubsub_snapshot().mesh_edges == 0U);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_protocol_stream_close_preserves_peer_state_until_session_disconnect) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for();
   server_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
   server_options.limits.pubsub.limits.heartbeat_interval = std::chrono::milliseconds{20};
   auto client_options = pubsub_options_for();
   client_options.explicit_peer_id = peer(221);

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   auto first_stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto subject = pubsub::topic{.value = "forge.subscription.disconnect"};
   forge::asio::blocking::run(
       runtime, server.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));

   const auto subscribe_and_graft = pubsub::codec::encode(pubsub::rpc{
       .subscriptions =
           std::vector<pubsub::subscription>{
               pubsub::subscription{.subscribe = true, .subject = subject},
           },
       .control_value =
           pubsub::control{
               .grafts =
                   std::vector<pubsub::control::graft>{
                       pubsub::control::graft{.subject = subject},
                   },
           },
   });
   forge::asio::blocking::run(runtime, first_stream.async_write(subscribe_and_graft));
   for (auto poll = 0; poll < 100 && server.pubsub_snapshot().mesh_edges == 0U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub initial subscription and GRAFT");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);

   forge::asio::blocking::run(runtime, first_stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "GossipSub transient protocol stream close");
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_REQUIRE(server.metrics().active_sessions > 0U);

   auto second_stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto graft = pubsub::codec::encode(pubsub::rpc{
       .control_value =
           pubsub::control{
               .grafts =
                   std::vector<pubsub::control::graft>{
                       pubsub::control::graft{.subject = subject},
                   },
           },
   });
   forge::asio::blocking::run(runtime, second_stream.async_write(graft));
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "GossipSub reopened stream heartbeat stability");
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 1U);
   BOOST_REQUIRE(server.metrics().active_sessions > 0U);

   forge::asio::blocking::run(runtime, client.async_stop());
   for (auto poll = 0;
        poll < 200 && (server.pubsub_snapshot().peers != 0U || server.pubsub_snapshot().mesh_edges != 0U); ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub final session disconnect cleanup");
   }
   BOOST_REQUIRE(server.pubsub_snapshot().peers == 0U);
   BOOST_REQUIRE(server.pubsub_snapshot().mesh_edges == 0U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_control_spam_is_penalized_without_stopping_node) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for();
   server_options.limits.pubsub.limits.max_ihave_per_peer = 1;
   server_options.limits.pubsub.limits.max_iwant_per_peer = 1;
   server_options.limits.pubsub.limits.max_graft_per_peer = 1;
   auto client_options = pubsub_options_for();
   client_options.explicit_peer_id = peer(160);

   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   forge::asio::blocking::run(
       runtime, server.async_subscribe(pubsub::topic{.value = "forge.spam"},
                                       [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
                                          co_return pubsub::validation_result::accept;
                                       }));

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
   const auto id = std::vector<std::uint8_t>{'i', 'd'};
   auto spam =
       pubsub::rpc{
           .control_value =
               pubsub::control{
                   .have =
                       std::vector<pubsub::control::ihave>{
                           pubsub::control::ihave{.subject = pubsub::topic{.value = "forge.spam"},
                                                  .message_ids = std::vector<std::vector<std::uint8_t>>{id}},
                           pubsub::control::ihave{.subject = pubsub::topic{.value = "forge.spam"},
                                                  .message_ids = std::vector<std::vector<std::uint8_t>>{id}}},
                   .want =
                       std::vector<pubsub::control::iwant>{
                           pubsub::control::iwant{.message_ids = std::vector<std::vector<std::uint8_t>>{id}},
                           pubsub::control::iwant{.message_ids = std::vector<std::vector<std::uint8_t>>{id}}},
                   .grafts =
                       std::vector<pubsub::control::graft>{
                           pubsub::control::graft{.subject = pubsub::topic{.value = "forge.spam"}},
                           pubsub::control::graft{.subject = pubsub::topic{.value = "forge.spam"}}},
               },
       };
   forge::asio::blocking::run(runtime, stream.async_write(pubsub::codec::encode(spam)));
   wait_on_runtime(runtime, std::chrono::milliseconds{250}, "gossipsub spam accounting");

   BOOST_TEST(server.metrics().pubsub_invalid_messages >= 1U);
   BOOST_TEST(server.metrics().protocol_rejections >= 1U);
   BOOST_TEST(!server.metrics().stopped);

   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_abusive_peer_crossing_malformed_threshold_closes_only_offender_session) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = pubsub_options_for();
   server_options.limits.resources.max_malformed_messages_per_peer = 1;
   set_resource_session_limits(server_options, 2, 2, (std::numeric_limits<std::size_t>::max)(),
                               (std::numeric_limits<std::size_t>::max)());
   server_options.limits.session_low_watermark = 2;
   server_options.limits.pubsub.limits.max_ihave_per_peer = 1;
   auto bad_options = pubsub_options_for();
   bad_options.explicit_peer_id = peer(248);
   auto good_options = pubsub_options_for();
   good_options.explicit_peer_id = peer(249);

   auto server = node{runtime, std::move(server_options)};
   auto bad = node{runtime, std::move(bad_options)};
   auto good = node{runtime, std::move(good_options)};
   register_echo(server);

   const auto server_endpoint = listen(server, runtime);
   bad.peers().learn_endpoint(server.local_peer(), server_endpoint,
                              capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   good.peers().learn_endpoint(server.local_peer(), server_endpoint,
                               capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   (void)forge::asio::blocking::run(
       runtime, bad.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   (void)forge::asio::blocking::run(
       runtime, good.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   const auto id = std::vector<std::uint8_t>{'i', 'd'};
   auto spam = pubsub::rpc{
       .control_value =
           pubsub::control{
               .have =
                   std::vector<pubsub::control::ihave>{
                       pubsub::control::ihave{.subject = pubsub::topic{.value = "forge.abuse"},
                                              .message_ids = std::vector<std::vector<std::uint8_t>>{id}},
                       pubsub::control::ihave{.subject = pubsub::topic{.value = "forge.abuse"},
                                              .message_ids = std::vector<std::vector<std::uint8_t>>{id}}},
           },
   };
   for (auto index = 0; index != 2; ++index) {
      auto stream = forge::asio::blocking::run(
          runtime, bad.async_open_protocol_stream(server.local_peer(), builtins::meshsub_v11));
      forge::asio::blocking::run(runtime, stream.async_write(pubsub::codec::encode(spam)));
      wait_on_runtime(runtime, std::chrono::milliseconds{150}, "gossipsub abuse accounting");
   }

   BOOST_TEST(server.metrics().pubsub_invalid_messages >= 2U);
   BOOST_TEST(server.metrics().sessions_closed >= 1U);
   BOOST_TEST(server.metrics().connection_rejections >= 1U);
   const auto after_eviction = server.diagnostics();
   BOOST_TEST(after_eviction.connections.retained_identify_attempts <= after_eviction.connections.active_sessions);

   auto stream =
       forge::asio::blocking::run(runtime, good.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                                           node::open_options{.allow_relay = false}));
   const auto payload = std::vector<std::uint8_t>{'g', 'o', 'o', 'd'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, good.async_stop());
   forge::asio::blocking::run(runtime, bad.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_outbound_byte_limit_rejects_publish_without_stopping_node) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for();
   publisher_options.limits.pubsub.limits.max_outbound_queue_bytes = 8;
   auto subscriber_options = pubsub_options_for();
   subscriber_options.explicit_peer_id = peer(161);

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(pubsub::topic{.value = "forge.limit"},
                                           [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
                                              co_return pubsub::validation_result::accept;
                                           }));

   BOOST_CHECK_THROW((void)forge::asio::blocking::run(
                         runtime, publisher.async_publish(pubsub::topic{.value = "forge.limit"},
                                                          std::vector<std::uint8_t>{'o', 'v', 'e', 'r'})),
                     forge::exceptions::base);
   BOOST_TEST(publisher.metrics().backpressure_rejections >= 1U);
   BOOST_TEST(!publisher.metrics().stopped);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   forge::asio::blocking::run(runtime, subscriber.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_outbound_byte_limit_counts_blocked_active_publication_per_peer) {
   constexpr auto queue_limit = std::size_t{128 * 1024};
   constexpr auto payload_size = std::size_t{96 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_test_identity();
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   auto publisher_options = options_for(publisher_identity, pubsub_capabilities);
   publisher_options.limits.pubsub.limits.max_outbound_queue_bytes = queue_limit;
   auto publisher = node{runtime, std::move(publisher_options)};

   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   const auto stalled_endpoint = start_stalling_tcp_peer(runtime, std::chrono::seconds{5}, accepted);
   const auto stalled_peer = peer(162);
   publisher.peers().learn_endpoint(stalled_peer, stalled_endpoint, pubsub_capabilities);
   const auto failures_before = publisher.peers().find(stalled_peer)->failures;
   const auto subject = pubsub::topic{.value = "forge.limit.aggregate"};
   auto first = boost::asio::co_spawn(runtime.context(),
                                      publisher.async_publish(subject, std::vector<std::uint8_t>(payload_size, 0x31U)),
                                      boost::asio::use_future);
   BOOST_REQUIRE(accepted_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

   try {
      static_cast<void>(forge::asio::blocking::run(
          runtime, publisher.async_publish(subject, std::vector<std::uint8_t>(payload_size, 0x32U))));
      BOOST_FAIL("aggregate GossipSub outbound byte limit should reject the queued publication");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(exceptions::code::backpressure_rejected));
   }
   BOOST_TEST(publisher.metrics().backpressure_rejections >= 1U);
   BOOST_REQUIRE(publisher.peers().find(stalled_peer).has_value());
   BOOST_TEST(publisher.peers().find(stalled_peer)->failures == failures_before);
   const auto metrics_before_stop = publisher.metrics();
   const auto peer_before_stop = publisher.peers().find(stalled_peer);
   BOOST_REQUIRE(peer_before_stop.has_value());
   BOOST_REQUIRE(!peer_before_stop->endpoints.empty());
   const auto endpoint_failures_before_stop = peer_before_stop->endpoints.front().failures;
   const auto endpoint_backoff_before_stop = peer_before_stop->endpoints.front().backoff_until;

   forge::asio::blocking::run(runtime, publisher.async_stop());
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(first.get()), forge::exceptions::base);
   const auto metrics_after_stop = publisher.metrics();
   const auto peer_after_stop = publisher.peers().find(stalled_peer);
   BOOST_REQUIRE(peer_after_stop.has_value());
   BOOST_REQUIRE(!peer_after_stop->endpoints.empty());
   BOOST_TEST(peer_after_stop->failures == failures_before);
   BOOST_TEST(peer_after_stop->endpoints.front().failures == endpoint_failures_before_stop);
   const auto backoff_unchanged_during_stop =
       peer_after_stop->endpoints.front().backoff_until == endpoint_backoff_before_stop;
   BOOST_TEST(backoff_unchanged_during_stop);
   BOOST_TEST(metrics_after_stop.direct_failures == metrics_before_stop.direct_failures);
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "post-stop GossipSub dial drain");
   const auto peer_after_drain = publisher.peers().find(stalled_peer);
   BOOST_REQUIRE(peer_after_drain.has_value());
   BOOST_REQUIRE(!peer_after_drain->endpoints.empty());
   BOOST_TEST(peer_after_drain->failures == failures_before);
   BOOST_TEST(peer_after_drain->endpoints.front().failures == endpoint_failures_before_stop);
   const auto backoff_unchanged_after_drain =
       peer_after_drain->endpoints.front().backoff_until == endpoint_backoff_before_stop;
   BOOST_TEST(backoff_unchanged_after_drain);
   BOOST_TEST(publisher.metrics().direct_failures == metrics_after_stop.direct_failures);
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_outbound_byte_limit_is_global_across_peers) {
   constexpr auto queue_limit = std::size_t{128 * 1024};
   constexpr auto payload_size = std::size_t{96 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_test_identity();
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   auto publisher_options = options_for(publisher_identity, pubsub_capabilities);
   publisher_options.limits.pubsub.limits.max_outbound_queue_bytes = queue_limit;
   auto publisher = node{runtime, std::move(publisher_options)};

   auto accepted_a = std::make_shared<std::promise<void>>();
   auto accepted_b = std::make_shared<std::promise<void>>();
   auto accepted_a_future = accepted_a->get_future();
   auto accepted_b_future = accepted_b->get_future();
   const auto endpoint_a = start_stalling_tcp_peer(runtime, std::chrono::seconds{5}, accepted_a);
   const auto endpoint_b = start_stalling_tcp_peer(runtime, std::chrono::seconds{5}, accepted_b);
   const auto peer_a = peer(163);
   const auto peer_b = peer(164);
   publisher.peers().learn_endpoint(peer_a, endpoint_a, pubsub_capabilities);

   const auto subject = pubsub::topic{.value = "forge.limit.global"};
   auto first = boost::asio::co_spawn(runtime.context(),
                                      publisher.async_publish(subject, std::vector<std::uint8_t>(payload_size, 0x41U)),
                                      boost::asio::use_future);
   BOOST_REQUIRE(accepted_a_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

   publisher.peers().upsert(peer_store::record{.peer = peer_a});
   publisher.peers().learn_endpoint(peer_b, endpoint_b, pubsub_capabilities);
   try {
      static_cast<void>(forge::asio::blocking::run(
          runtime, publisher.async_publish(subject, std::vector<std::uint8_t>(payload_size, 0x42U))));
      BOOST_FAIL("global GossipSub outbound byte limit should reject a different peer");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_CHECK(*exceptions::code_of(error) == exceptions::code::backpressure_rejected);
   }
   const auto second_accept_is_pending =
       accepted_b_future.wait_for(std::chrono::milliseconds{50}) != std::future_status::ready;
   BOOST_TEST(second_accept_is_pending);

   forge::asio::blocking::run(runtime, publisher.async_stop());
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(first.get()), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_rejected_outbound_reservations_do_not_retain_peer_entries) {
   auto budget = detail::pubsub_outbound_budget{};
   constexpr auto limit = std::size_t{128};
   constexpr auto reservation = std::size_t{96};
   const auto active = peer(250);
   BOOST_REQUIRE(budget.reserve(active, reservation, limit));

   for (auto seed = std::uint16_t{1}; seed <= 200; ++seed) {
      BOOST_TEST(!budget.reserve(peer(static_cast<std::uint8_t>(seed)), reservation, limit));
   }
   BOOST_TEST(budget.peers() == 1U);
   BOOST_TEST(budget.total() == reservation);

   budget.release(active, reservation);
   BOOST_TEST(budget.peers() == 0U);
   BOOST_TEST(budget.total() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_remote_peer_failure_attribution_excludes_local_runtime_failures) {
   BOOST_TEST(!detail::remote_peer_attributable_failure(exceptions::code::backpressure_rejected, false));
   BOOST_TEST(!detail::remote_peer_attributable_failure(exceptions::code::connection_rejected, false));
   BOOST_TEST(!detail::remote_peer_attributable_failure(exceptions::code::canceled, false));
   BOOST_TEST(!detail::remote_peer_attributable_failure(exceptions::code::internal, false));
   BOOST_TEST(!detail::remote_peer_attributable_failure(exceptions::code::sequence_exhausted, false));
   BOOST_TEST(!detail::remote_peer_attributable_failure(exceptions::code::durability_uncertain, false));
   BOOST_TEST(!detail::remote_peer_attributable_failure(exceptions::code::closed, true));
   BOOST_TEST(detail::remote_peer_attributable_failure(exceptions::code::timeout, true));
   BOOST_TEST(detail::remote_peer_attributable_failure(exceptions::code::closed, false));
   BOOST_TEST(detail::remote_peer_attributable_failure(exceptions::code::protocol_error, false));
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_validation_queue_limit_retries_excess_without_penalizing_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   auto subscriber_options = pubsub_options_for(make_test_certificate_identity("pubsub-validation-subscriber"));
   subscriber_options.limits.pubsub.limits.max_validation_queue = 1;
   subscriber_options.limits.pubsub.limits.max_validation_attempts = 1;
   subscriber_options.limits.pubsub.limits.validation_retry_initial_delay = std::chrono::milliseconds{600};
   subscriber_options.limits.pubsub.limits.validation_retry_max_delay = std::chrono::milliseconds{600};
   subscriber_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   auto publisher_a_options = pubsub_options_for(make_test_certificate_identity("pubsub-validation-a"));
   publisher_a_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
   auto publisher_b_options = pubsub_options_for(make_test_certificate_identity("pubsub-validation-b"));
   publisher_b_options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;

   auto subscriber = node{runtime, std::move(subscriber_options)};
   auto publisher_a = node{runtime, std::move(publisher_a_options)};
   auto publisher_b = node{runtime, std::move(publisher_b_options)};
   const auto subscriber_endpoint = listen(subscriber, runtime);
   publisher_a.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                      capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   publisher_b.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                      capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   auto entered = std::make_shared<std::atomic_uint64_t>(0);
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    pubsub::topic{.value = "forge.validation"},
                    [entered](pubsub::event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                       entered->fetch_add(1, std::memory_order_relaxed);
                       auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
                       timer.expires_after(std::chrono::milliseconds{500});
                       boost::system::error_code ec;
                       co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
                       co_return pubsub::validation_result::accept;
                    }));

   auto publish_a = boost::asio::co_spawn(
       runtime.context(),
       [&publisher_a]() -> boost::asio::awaitable<void> {
          (void)co_await publisher_a.async_publish(pubsub::topic{.value = "forge.validation"},
                                                   std::vector<std::uint8_t>{'a'},
                                                   pubsub::publish_options{.sign = false});
       },
       boost::asio::use_future);
   auto publish_b = boost::asio::co_spawn(
       runtime.context(),
       [&publisher_b]() -> boost::asio::awaitable<void> {
          (void)co_await publisher_b.async_publish(pubsub::topic{.value = "forge.validation"},
                                                   std::vector<std::uint8_t>{'b'},
                                                   pubsub::publish_options{.sign = false});
       },
       boost::asio::use_future);
   wait_for_server(publish_a, std::chrono::seconds{5}, "first validation publish");
   wait_for_server(publish_b, std::chrono::seconds{5}, "second validation publish");
   for (auto poll = 0; poll < 100 && entered->load(std::memory_order_relaxed) < 2U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{20}, "validation queue retry");
   }
   for (auto poll = 0; poll < 100 && subscriber.pubsub_snapshot().messages_delivered < 2U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{20}, "validation completion");
   }

   BOOST_TEST(entered->load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(subscriber.metrics().backpressure_rejections >= 1U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 2U);

   forge::asio::blocking::run(runtime, subscriber.async_stop());
   BOOST_TEST(subscriber.metrics().stopped);
   forge::asio::blocking::run(runtime, publisher_a.async_stop());
   forge::asio::blocking::run(runtime, publisher_b.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_ten_node_mesh_delivers_multiple_publishes_once) {
   constexpr auto node_count = std::size_t{10};
   constexpr auto publish_count = std::size_t{3};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 12}};
   auto identities = std::vector<test_certificate_identity>{};
   auto nodes = std::vector<std::unique_ptr<node>>{};
   auto endpoints = std::vector<endpoint>{};
   identities.reserve(node_count);
   nodes.reserve(node_count);
   endpoints.reserve(node_count);
   for (auto index = std::size_t{}; index < node_count; ++index) {
      identities.push_back(make_test_certificate_identity("pubsub-mesh-" + std::to_string(index)));
      auto options = pubsub_options_for(identities.back());
      options.limits.pubsub.signatures = pubsub::signature_policy::lax_no_sign;
      nodes.push_back(std::make_unique<node>(runtime, std::move(options)));
      endpoints.push_back(listen(*nodes.back(), runtime));
   }
   for (auto index = std::size_t{}; index < node_count; ++index) {
      for (auto peer_index = std::size_t{}; peer_index < node_count; ++peer_index) {
         if (index == peer_index) {
            continue;
         }
         nodes[index]->peers().learn_endpoint(nodes[peer_index]->local_peer(), endpoints[peer_index],
                                              capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
      }
   }

   struct delivery_state {
      std::mutex mutex;
      std::condition_variable cv;
      std::map<std::string, std::set<std::size_t>> delivered;
      std::uint64_t duplicates = 0;
   };
   auto state = std::make_shared<delivery_state>();
   for (auto index = std::size_t{}; index < node_count; ++index) {
      forge::asio::blocking::run(
          runtime,
          nodes[index]->async_subscribe(
              pubsub::topic{.value = "forge.mesh.stress"},
              [state, index](pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                 const auto payload = std::string{event.value.data.begin(), event.value.data.end()};
                 {
                    auto lock = std::unique_lock{state->mutex};
                    if (!state->delivered[payload].insert(index).second) {
                       ++state->duplicates;
                    }
                 }
                 state->cv.notify_all();
                 co_return pubsub::validation_result::accept;
              }));
   }

   for (auto index = std::size_t{}; index < publish_count; ++index) {
      const auto payload = std::string{"stress-" + std::to_string(index)};
      forge::asio::blocking::run(runtime,
                                 nodes[index]->async_publish(pubsub::topic{.value = "forge.mesh.stress"},
                                                             std::vector<std::uint8_t>{payload.begin(), payload.end()},
                                                             pubsub::publish_options{.sign = false}));
   }

   {
      auto lock = std::unique_lock{state->mutex};
      const auto completed = state->cv.wait_for(lock, std::chrono::seconds{15}, [&] {
         for (auto index = std::size_t{}; index < publish_count; ++index) {
            const auto payload = std::string{"stress-" + std::to_string(index)};
            if (state->delivered[payload].size() < node_count - 1) {
               return false;
            }
         }
         return true;
      });
      BOOST_REQUIRE(completed);
      BOOST_TEST(state->duplicates == 0U);
   }

   for (auto index = std::size_t{}; index < publish_count; ++index) {
      const auto payload = std::string{"stress-" + std::to_string(index)};
      BOOST_TEST(!state->delivered[payload].contains(index));
   }
   for (auto& value : nodes) {
      forge::asio::blocking::run(runtime, value->async_stop());
   }
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_updates_peer_store) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-push-update-server");
   const auto client_identity = make_test_certificate_identity("identify-push-update-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   const auto learned_endpoint =
       parse_endpoint("/ip4/127.0.0.1/udp/4099/quic-v1/p2p/" + client.local_peer().to_string());
   server.peers().learn_endpoint(client.local_peer(), learned_endpoint);

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   auto pushed = identify::document{
       .protocol_version = "/forge/push-test/1",
       .agent_version = "forge-push-test/1",
       .listen_endpoints = addresses_for(
           {parse_endpoint("/ip4/127.0.0.1/udp/4101/quic-v1/p2p/" + client.local_peer().to_string())}),
       .protocols = std::vector<protocol_id>{builtins::ping},
   };
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "identify push propagation");

   const auto found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/push-test/1");
   BOOST_TEST(
       std::ranges::any_of(found->protocols, [](const protocol_id& protocol) { return protocol == builtins::ping; }));
   BOOST_REQUIRE_EQUAL(found->endpoints.size(), 2U);
   const auto identified = std::ranges::find_if(found->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4101U;
   });
   BOOST_REQUIRE(identified != found->endpoints.end());
   BOOST_TEST(peer_for(identified->address).to_bytes() == client.local_peer().to_bytes(),
              boost::test_tools::per_element());
   BOOST_TEST(!identified->sources.learned);
   BOOST_TEST(identified->sources.identify_unsigned);
   BOOST_TEST(!identified->sources.identify_signed);

   const auto replacement = parse_endpoint("/ip4/127.0.0.1/udp/4102/quic-v1/p2p/" + client.local_peer().to_string());
   stream = forge::asio::blocking::run(runtime,
                                       client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   pushed.listen_endpoints = addresses_for({replacement});
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "Identify unsigned snapshot replacement");

   const auto replaced = server.peers().find(client.local_peer());
   BOOST_REQUIRE(replaced);
   BOOST_REQUIRE_EQUAL(replaced->endpoints.size(), 2U);
   BOOST_TEST(std::ranges::none_of(replaced->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4101U;
   }));
   BOOST_TEST(std::ranges::any_of(replaced->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4099U && value.sources.learned;
   }));
   BOOST_TEST(std::ranges::any_of(replaced->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4102U && value.sources.identify_unsigned;
   }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_merges_multiple_length_delimited_messages) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-multipart-server");
   const auto client_identity = make_test_certificate_identity("identify-multipart-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   const auto initial = wait_for_identified_peer(server, client.local_peer(), runtime, "multipart inbound Identify");
   auto advertised_endpoints = std::vector<endpoint>{};
   advertised_endpoints.reserve(48);
   for (auto port = std::uint16_t{4103}; port < 4151; ++port) {
      const auto host =
          "node-" + std::to_string(port) + "." + std::string(40, 'a') + "." + std::string(40, 'b') + ".example.test";
      advertised_endpoints.push_back(parse_endpoint("/dns4/" + host + "/udp/" + std::to_string(port) + "/quic-v1/p2p/" +
                                                    client.local_peer().to_string()));
   }
   const auto signed_record = make_signed_identify_peer_record(
       client_identity, advertised_endpoints, identify_peer_record_sequence(initial.signed_peer_record) + 1);

   auto first_protocols = std::vector<protocol_id>{builtins::ping};
   for (auto index = 0U; index < 9U; ++index) {
      first_protocols.push_back(protocol_id{
          .value = "/forge/identify-padding/" + std::to_string(index) + "/" + std::string(775, 'x'),
      });
   }

   const auto first = identify::document{
       .public_key = encode_public_key(public_key_for(client_identity)),
       .protocols = std::vector<protocol_id>{builtins::ping},
       .signed_peer_record = signed_record,
   };
   const auto second = identify::document{
       .protocol_version = "/forge/multipart/1",
       .agent_version = "forge-multipart/1",
       .listen_endpoints = addresses_for({advertised_endpoints.front()}),
       .protocols = std::move(first_protocols),
   };
   const auto first_bytes = identify::encode(first);
   const auto second_bytes = identify::encode(second);
   BOOST_REQUIRE(first_bytes.size() <= identify::limits{}.max_message_size);
   BOOST_REQUIRE(second_bytes.size() <= identify::limits{}.max_message_size);
   BOOST_REQUIRE(first_bytes.size() + second_bytes.size() > identify::limits{}.max_message_size);
   BOOST_REQUIRE(signed_record.size() > 4U * 1024U);
   auto encoded = wrap_length_delimited(first_bytes);
   const auto tail = wrap_length_delimited(second_bytes);
   encoded.insert(encoded.end(), tail.begin(), tail.end());

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   forge::asio::blocking::run(runtime, stream.async_write(encoded));
   forge::asio::blocking::run(runtime, stream.async_close());
   const auto found =
       wait_for_peer_record(server, client.local_peer(), runtime, "multipart Identify Push", [&](const auto& record) {
          return record.protocol_version == "/forge/multipart/1" && record.signed_peer_record == signed_record &&
                 std::ranges::any_of(record.protocols, [](const auto& value) {
                    return value.value.starts_with("/forge/identify-padding/");
                 });
       });

   BOOST_TEST(found.protocol_version == "/forge/multipart/1");
   BOOST_TEST(found.signed_peer_record == signed_record, boost::test_tools::per_element());
   BOOST_TEST(std::ranges::any_of(found.protocols, [](const auto& value) { return value == builtins::ping; }));
   BOOST_TEST(std::ranges::any_of(
       found.protocols, [](const auto& value) { return value.value.starts_with("/forge/identify-padding/"); }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_reset_does_not_publish_partial_document) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-reset-server");
   const auto client_identity = make_test_certificate_identity("identify-reset-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   const auto before = wait_for_identified_peer(server, client.local_peer(), runtime, "reset Push inbound Identify");
   const auto partial_protocol = protocol_id{.value = "/forge/identify-reset-must-not-publish/1"};
   const auto partial = identify::document{
       .protocol_version = "/forge/identify-reset/1",
       .protocols = std::vector<protocol_id>{builtins::ping, partial_protocol},
   };

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(partial))));
   wait_on_runtime(runtime, std::chrono::milliseconds{50}, "partial Identify Push delivery");
   stream.cancel();
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "partial Identify Push reset");

   const auto after = server.peers().find(client.local_peer());
   BOOST_REQUIRE(after.has_value());
   BOOST_TEST(after->protocol_version == before.protocol_version);
   BOOST_TEST(!std::ranges::contains(after->protocols, partial_protocol));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_preserves_fields_omitted_by_rust_partial_update) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-partial-server");
   const auto client_identity = make_test_certificate_identity("identify-partial-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(listen(client, runtime));
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   const auto before = wait_for_identified_peer(server, client.local_peer(), runtime, "partial Push inbound Identify");
   BOOST_REQUIRE(!before.protocol_version.empty());
   BOOST_REQUIRE(!before.agent_version.empty());
   BOOST_REQUIRE(!before.public_key.empty());
   BOOST_REQUIRE(!before.endpoints.empty());

   const auto product_protocol = protocol_id{.value = "/forge/rust-partial-push/1"};
   const auto partial = identify::document{
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify_push, product_protocol},
   };
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(partial))));
   forge::asio::blocking::run(runtime, stream.async_close());

   auto updated = std::optional<peer_store::record>{};
   for (auto attempt = 0U; attempt < 200U; ++attempt) {
      updated = server.peers().find(client.local_peer());
      if (updated && std::ranges::contains(updated->protocols, product_protocol)) {
         break;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "Rust partial Identify Push");
   }
   BOOST_REQUIRE(updated);
   BOOST_REQUIRE(std::ranges::contains(updated->protocols, product_protocol));
   BOOST_TEST(updated->protocol_version == before.protocol_version);
   BOOST_TEST(updated->agent_version == before.agent_version);
   BOOST_TEST(updated->public_key == before.public_key, boost::test_tools::per_element());
   BOOST_TEST(updated->signed_peer_record == before.signed_peer_record, boost::test_tools::per_element());
   BOOST_TEST(updated->endpoints.size() == before.endpoints.size());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_rejects_mismatched_endpoint_peer_suffix) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-push-mismatch-server");
   const auto client_identity = make_test_certificate_identity("identify-push-mismatch-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   const auto bad_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4107/quic-v1/p2p/" + peer(215).to_string());
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   auto pushed = identify::document{
       .protocol_version = "/forge/push-bad-peer/1",
       .agent_version = "forge-push-bad-peer/1",
       .listen_endpoints = addresses_for({bad_endpoint}),
       .protocols = std::vector<protocol_id>{builtins::ping},
   };
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "identify push mismatch propagation");

   const auto found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/push-bad-peer/1");
   BOOST_TEST(std::ranges::none_of(found->endpoints, [&](const peer_store::endpoint_record& record) {
      return record.address.to_string() == bad_endpoint.to_string();
   }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_flushes_memory_peer_record_for_hydration) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-push-memory-server");
   const auto client_identity = make_test_certificate_identity("identify-push-memory-client");
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto server_options = options_for(server_identity);
   server_options.peer_state.persistence = persistence;
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   auto advertised = parse_endpoint("/ip4/127.0.0.1/udp/4201/quic-v1/p2p/" + client.local_peer().to_string());
   const auto encoded_public_key = encode_public_key(public_key_for(client_identity));
   const auto initial =
       wait_for_identified_peer(server, client.local_peer(), runtime, "persisted Push inbound Identify");
   const auto signed_peer_record =
       make_signed_identify_peer_record(client_identity, std::vector<endpoint>{advertised},
                                        identify_peer_record_sequence(initial.signed_peer_record) + 1);
   auto pushed = identify::document{
       .protocol_version = "/forge/push-persist/1",
       .agent_version = "forge-push-persist/1",
       .public_key = encoded_public_key,
       .listen_endpoints = addresses_for({advertised}),
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify},
       .signed_peer_record = signed_peer_record,
   };
   const auto decoded = identify::decode(identify::encode(pushed));
   BOOST_REQUIRE_EQUAL(decoded.listen_endpoints.size(), 1U);
   BOOST_TEST(peer_for(decoded.listen_endpoints.front()).to_bytes() == client.local_peer().to_bytes(),
              boost::test_tools::per_element());
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "identify push persistence");
   forge::asio::blocking::run(runtime, server.peers().async_flush());

   auto hydrated = peer_store{peer_store::options{.persistence = persistence}};
   forge::asio::blocking::run(runtime, hydrated.async_hydrate());
   const auto snapshot = hydrated.snapshot(128);
   const auto found = std::ranges::find_if(
       snapshot, [](const peer_store::record& value) { return value.protocol_version == "/forge/push-persist/1"; });
   BOOST_REQUIRE(found != snapshot.end());
   BOOST_TEST(found->peer.to_bytes() == client_identity.peer.to_bytes(), boost::test_tools::per_element());
   BOOST_TEST(found->agent_version == "forge-push-persist/1");
   BOOST_TEST(found->public_key == encoded_public_key, boost::test_tools::per_element());
   BOOST_TEST(found->signed_peer_record == signed_peer_record, boost::test_tools::per_element());
   BOOST_TEST(
       std::ranges::any_of(found->protocols, [](const protocol_id& value) { return value == builtins::identify; }));
   BOOST_REQUIRE_EQUAL(found->endpoints.size(), 1U);
   BOOST_TEST(transport_port_for(found->endpoints.front().address) == 4201U);
   BOOST_TEST(!found->endpoints.front().sources.learned);
   BOOST_TEST(found->endpoints.front().sources.identify_signed);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_accepts_rust_legacy_signed_peer_record) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-legacy-record-server");
   const auto client_identity = make_test_certificate_identity("identify-legacy-record-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   const auto advertised = parse_endpoint("/ip4/127.0.0.1/udp/4207/quic-v1/p2p/" + client.local_peer().to_string());
   auto initial = std::optional<peer_store::record>{};
   for (auto attempt = 0U; attempt < 200U; ++attempt) {
      initial = server.peers().find(client.local_peer());
      if (initial && !initial->signed_peer_record.empty()) {
         break;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "inbound Identify before legacy Push");
   }
   BOOST_REQUIRE(initial);
   BOOST_REQUIRE(!initial->signed_peer_record.empty());
   const auto signed_peer_record =
       make_signed_rendezvous_peer_record(client_identity, std::vector<endpoint>{advertised},
                                          identify_peer_record_sequence(initial->signed_peer_record) + 1);
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   auto pushed = identify::document{
       .protocol_version = "/forge/rust-identify/1",
       .agent_version = "rust-libp2p",
       .public_key = encode_public_key(public_key_for(client_identity)),
       .listen_endpoints = addresses_for({advertised}),
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify},
       .signed_peer_record = signed_peer_record,
   };
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "Rust legacy Identify signed record");

   const auto found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/rust-identify/1");
   BOOST_TEST(found->signed_peer_record == signed_peer_record, boost::test_tools::per_element());
   BOOST_REQUIRE_EQUAL(found->endpoints.size(), 1U);
   BOOST_TEST(transport_port_for(found->endpoints.front().address) == 4207U);
   BOOST_TEST(found->endpoints.front().sources.identify_signed);
   BOOST_TEST(
       std::ranges::any_of(found->protocols, [](const protocol_id& value) { return value == builtins::identify; }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_preserves_certified_record_across_unsigned_updates) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-certified-server");
   const auto client_identity = make_test_certificate_identity("identify-certified-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   const auto advertised = parse_endpoint("/ip4/127.0.0.1/udp/4202/quic-v1/p2p/" + client.local_peer().to_string());
   const auto initial =
       wait_for_identified_peer(server, client.local_peer(), runtime, "certified Push inbound Identify");
   const auto signed_sequence = identify_peer_record_sequence(initial.signed_peer_record) + 1;
   const auto signed_record =
       make_signed_identify_peer_record(client_identity, std::vector<endpoint>{advertised}, signed_sequence);
   auto push = [&](identify::document document) {
      auto stream = forge::asio::blocking::run(
          runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
      forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(document))));
      forge::asio::blocking::run(runtime, stream.async_close());
      wait_on_runtime(runtime, std::chrono::milliseconds{100}, "Identify certified-record update");
   };

   push(identify::document{
       .protocol_version = "/forge/certified/1",
       .listen_endpoints = addresses_for({advertised}),
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify_push},
       .signed_peer_record = signed_record,
   });
   auto found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->signed_peer_record == signed_record, boost::test_tools::per_element());
   BOOST_REQUIRE_EQUAL(found->endpoints.size(), 1U);
   BOOST_TEST(found->endpoints.front().sources.identify_signed);

   const auto unsigned_advertised =
       parse_endpoint("/ip4/127.0.0.1/udp/4203/quic-v1/p2p/" + client.local_peer().to_string());
   push(identify::document{
       .protocol_version = "/forge/unsigned-update/1",
       .listen_endpoints = addresses_for({unsigned_advertised}),
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify_push},
   });
   found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/unsigned-update/1");
   BOOST_TEST(found->signed_peer_record == signed_record, boost::test_tools::per_element());
   BOOST_REQUIRE_EQUAL(found->endpoints.size(), 2U);

   const auto equal_sequence_endpoint =
       parse_endpoint("/ip4/127.0.0.1/udp/4206/quic-v1/p2p/" + client.local_peer().to_string());
   const auto equal_sequence_record = make_signed_identify_peer_record(
       client_identity, std::vector<endpoint>{equal_sequence_endpoint}, signed_sequence);
   push(identify::document{
       .protocol_version = "/forge/equal-sequence-refresh/1",
       .listen_endpoints = addresses_for({equal_sequence_endpoint}),
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify_push},
       .signed_peer_record = equal_sequence_record,
   });
   found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/equal-sequence-refresh/1");
   BOOST_TEST(found->signed_peer_record == equal_sequence_record, boost::test_tools::per_element());
   BOOST_TEST(std::ranges::any_of(found->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4206U && value.sources.identify_signed;
   }));

   const auto replacement = parse_endpoint("/ip4/127.0.0.1/udp/4204/quic-v1/p2p/" + client.local_peer().to_string());
   const auto ignored_unsigned =
       parse_endpoint("/ip4/127.0.0.1/udp/4205/quic-v1/p2p/" + client.local_peer().to_string());
   const auto replacement_record =
       make_signed_identify_peer_record(client_identity, std::vector<endpoint>{replacement}, signed_sequence + 1);
   push(identify::document{
       .protocol_version = "/forge/certified/2",
       .listen_endpoints = addresses_for({ignored_unsigned}),
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify_push},
       .signed_peer_record = replacement_record,
   });
   found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->signed_peer_record == replacement_record, boost::test_tools::per_element());
   BOOST_TEST(std::ranges::none_of(found->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4202U || transport_port_for(value.address) == 4205U ||
             transport_port_for(value.address) == 4206U;
   }));
   BOOST_TEST(std::ranges::any_of(found->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4204U && value.sources.identify_signed;
   }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_ignores_invalid_signed_record_without_dropping_authenticated_facts) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-invalid-record-server");
   const auto client_identity = make_test_certificate_identity("identify-invalid-record-client");
   const auto foreign_identity = make_test_certificate_identity("identify-invalid-record-foreign");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   const auto before = wait_for_identified_peer(server, client.local_peer(), runtime, "invalid Push inbound Identify");
   auto invalid_record = make_signed_identify_peer_record(foreign_identity);
   auto pushed = identify::document{
       .protocol_version = "/forge/invalid-record/1",
       .protocols = std::vector<protocol_id>{builtins::ping},
       .signed_peer_record = std::move(invalid_record),
   };
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "Identify invalid signed record rejection");

   const auto found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/invalid-record/1");
   BOOST_TEST(found->signed_peer_record == before.signed_peer_record, boost::test_tools::per_element());
   BOOST_TEST(std::ranges::any_of(found->protocols, [](const auto& value) { return value == builtins::ping; }));
   BOOST_TEST(forge::asio::blocking::run(runtime, client.async_ping(server.local_peer())).count() >= 0);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_rejects_interface_scoped_observation_without_dropping_authenticated_facts) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-scoped-record-server");
   const auto client_identity = make_test_certificate_identity("identify-scoped-record-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   static_cast<void>(wait_for_identified_peer(server, client.local_peer(), runtime, "scoped Push inbound Identify"));
   const auto scoped = parse_endpoint("/ip6zone/receiver0/ip6/2001:4860::1/udp/4210/quic-v1/p2p/" +
                                      client.local_peer().to_string());
   const auto observed = parse_endpoint("/ip6zone/receiver0/ip6/2001:4860::2/udp/4211/quic-v1");
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   const auto pushed = identify::document{
       .protocol_version = "/forge/scoped-record/1",
       .listen_endpoints = addresses_for({scoped}),
       .protocols = std::vector<protocol_id>{builtins::ping},
       .observed_endpoint = observed,
   };
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   static_cast<void>(wait_for_peer_record(server, client.local_peer(), runtime,
       "Identify interface-scoped observation rejection", [](const auto& record) {
          return record.protocol_version == "/forge/scoped-record/1";
       }));

   const auto found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/scoped-record/1");
   BOOST_TEST(!found->observed_endpoint.has_value());
   BOOST_TEST(std::ranges::none_of(found->endpoints, [](const peer_store::endpoint_record& value) {
      return std::ranges::any_of(value.address.components(), [](const auto& component) {
         return component.code == forge::multiformats::protocol_code::ip6zone;
      });
   }));
   BOOST_TEST(std::ranges::any_of(found->protocols, [](const protocol_id& value) { return value == builtins::ping; }));
   BOOST_TEST(forge::asio::blocking::run(runtime, client.async_ping(server.local_peer())).count() >= 0);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_rejects_canonical_record_with_mismatched_inner_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-inner-peer-server");
   const auto client_identity = make_test_certificate_identity("identify-inner-peer-client");
   auto server = node{runtime, options_for(server_identity)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   const auto before =
       wait_for_identified_peer(server, client.local_peer(), runtime, "inner-peer Push inbound Identify");
   const auto advertised = parse_endpoint("/ip4/127.0.0.1/udp/4208/quic-v1/p2p/" + client.local_peer().to_string());
   // Encode hostile protobuf directly: the validating peer-record encoder
   // correctly refuses this disagreement before it can reach the wire.
   auto payload = std::vector<std::uint8_t>{0x0a};
   const auto inner_peer = wrap_length_delimited(peer(214).to_bytes());
   payload.insert(payload.end(), inner_peer.begin(), inner_peer.end());
   payload.push_back(0x10);
   const auto sequence = forge::multiformats::varint_encode(
       identify_peer_record_sequence(before.signed_peer_record) + 1);
   payload.insert(payload.end(), sequence.begin(), sequence.end());
   auto address_info = std::vector<std::uint8_t>{0x0a};
   const auto address = wrap_length_delimited(advertised.to_multiaddr().to_bytes());
   address_info.insert(address_info.end(), address.begin(), address.end());
   payload.push_back(0x1a);
   const auto encoded_address_info = wrap_length_delimited(address_info);
   payload.insert(payload.end(), encoded_address_info.begin(), encoded_address_info.end());
   const auto invalid_record =
       signed_envelope::seal(public_key_for(client_identity),
                             forge::crypto::pki::pem::read_private_key(client_identity.private_key_pem),
                             "libp2p-peer-record", identify_peer_record_payload_type(), payload)
           .encode();
   BOOST_CHECK_NO_THROW(signed_envelope::decode(invalid_record).verify("libp2p-peer-record", client.local_peer()));
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));
   const auto pushed = identify::document{
       .protocol_version = "/forge/inner-peer-mismatch/1",
       .listen_endpoints = addresses_for({advertised}),
       .protocols = std::vector<protocol_id>{builtins::ping},
       .signed_peer_record = invalid_record,
   };
   forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(identify::encode(pushed))));
   forge::asio::blocking::run(runtime, stream.async_close());
   wait_on_runtime(runtime, std::chrono::milliseconds{100}, "Identify inner peer mismatch");

   const auto found = server.peers().find(client.local_peer());
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/inner-peer-mismatch/1");
   BOOST_TEST(found->signed_peer_record == before.signed_peer_record, boost::test_tools::per_element());
   BOOST_TEST(std::ranges::any_of(found->endpoints, [](const auto& value) {
      return transport_port_for(value.address) == 4208U && value.sources.identify_unsigned;
   }));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_read_timeout_releases_inbound_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("identify-push-timeout-server");
   const auto client_identity = make_test_certificate_identity("identify-push-timeout-client");
   auto server_options = options_for(server_identity);
   server_options.identify.timeout = std::chrono::milliseconds{50};
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, options_for(client_identity)};

   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify_push));

   auto released = false;
   for (auto attempt = 0U; attempt < 100U && !released; ++attempt) {
      released = active_stream_count(server.diagnostics().resources) == 0U;
      if (!released) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "Identify Push read timeout");
      }
   }
   BOOST_TEST(released);
   BOOST_TEST(forge::asio::blocking::run(runtime, client.async_ping(server.local_peer())).count() >= 0);
   stream.cancel();

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_unsupported_protocol_rejection_keeps_session_usable) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(79))};
   auto client = node{runtime, options_for(peer(80))};

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));

   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_open_protocol_stream(server.local_peer(), protocol_id{.value = "/product/missing/1"}));
      BOOST_FAIL("expected unsupported protocol rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::unsupported_protocol));
   }

   auto negotiation_streams_closed = false;
   for (auto attempt = 0U; attempt < 100U && !negotiation_streams_closed; ++attempt) {
      negotiation_streams_closed = active_stream_count(client.diagnostics().resources) == 0U &&
                                   active_stream_count(server.diagnostics().resources) == 0U;
      if (!negotiation_streams_closed) {
         wait_on_runtime(runtime, std::chrono::milliseconds{5}, "unsupported protocol stream reset");
      }
   }
   BOOST_REQUIRE(negotiation_streams_closed);

   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::ping));
   const auto payload = std::vector<std::uint8_t>(32, 0x24);
   forge::asio::blocking::run(runtime, stream.async_write(payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, stream.async_read()) == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_cached_protocol_open_shutdown_does_not_penalize_peer) {
   auto server_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{server_runtime, options_for(peer(207))};
   auto client = node{client_runtime, options_for(peer(208))};
   register_echo(server);

   const auto server_endpoint = listen(server, server_runtime);
   (void)forge::asio::blocking::run(
       client_runtime,
       client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()}));
   const auto before = client.peers().find(server.local_peer());
   BOOST_REQUIRE(before.has_value());
   const auto endpoint_before = std::ranges::find_if(before->endpoints, [&](const auto& current) {
      return current.address.to_string() == server_endpoint.to_string();
   });
   BOOST_REQUIRE(endpoint_before != before->endpoints.end());
   const auto peer_failures_before = before->failures;
   const auto endpoint_failures_before = endpoint_before->failures;
   const auto endpoint_backoff_before = endpoint_before->backoff_until;
   const auto direct_failures_before = client.metrics().direct_failures;

   auto release_server = block_runtime(server_runtime, "cached protocol shutdown barrier");
   const auto attempts_before = client.metrics().path_direct_attempts;
   auto opened = boost::asio::co_spawn(
       client_runtime.context(),
       client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                         node::open_options{.allow_relay = false,
                                                            .timeout = std::chrono::seconds{5},
                                                            .direct_attempt_timeout = std::chrono::seconds{5},
                                                            .max_direct_endpoints = 1}),
       boost::asio::use_future);
   const auto attempt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (client.metrics().path_direct_attempts == attempts_before) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < attempt_deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   auto stopped = boost::asio::co_spawn(client_runtime.context(), client.async_stop(), boost::asio::use_future);
   const auto stopped_without_remote_progress = stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
   release_server->set_value();
   BOOST_REQUIRE(stopped_without_remote_progress);
   stopped.get();
   BOOST_REQUIRE(opened.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   try {
      static_cast<void>(opened.get());
      BOOST_FAIL("expected cached protocol open shutdown");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      const auto code = *exceptions::code_of(error);
      BOOST_CHECK(code == exceptions::code::canceled || code == exceptions::code::closed);
   }

   const auto after = client.peers().find(server.local_peer());
   BOOST_REQUIRE(after.has_value());
   const auto endpoint_after = std::ranges::find_if(after->endpoints, [&](const auto& current) {
      return current.address.to_string() == server_endpoint.to_string();
   });
   BOOST_REQUIRE(endpoint_after != after->endpoints.end());
   BOOST_TEST(after->failures == peer_failures_before);
   BOOST_TEST(endpoint_after->failures == endpoint_failures_before);
   const auto backoff_unchanged = endpoint_after->backoff_until == endpoint_backoff_before;
   BOOST_TEST(backoff_unchanged);
   BOOST_TEST(client.metrics().direct_failures == direct_failures_before);

   forge::asio::blocking::run(server_runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_cached_protocol_timeout_penalizes_peer_without_advancing_dht_failure) {
   auto server_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto limits = dht::options{};
   limits.failure_threshold = 1;
   const auto server_identity = make_test_certificate_identity("cached-protocol-server");
   const auto client_identity = make_test_certificate_identity("cached-protocol-client");
   auto server_options = dht_options_for(server_identity, custom_test_dht_profile(dht::mode::server, limits));
   auto client_options = dht_options_for(client_identity, custom_test_dht_profile(dht::mode::client, limits));
   auto server = node{server_runtime, std::move(server_options)};
   auto client = node{client_runtime, std::move(client_options)};
   register_echo(server);

   const auto server_endpoint = listen(server, server_runtime);
   verify_dht_server(client_runtime, client, server, server_endpoint, content_swarm_test_dht);
   const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (true) {
      const auto state = client.diagnostics();
      const auto profile = std::ranges::find_if(
          state.dht_profiles, [](const auto& value) { return value.protocol == content_swarm_test_dht; });
      BOOST_REQUIRE(profile != state.dht_profiles.end());
      if (profile->maintenance_enabled && !profile->maintenance_startup_pending && !profile->maintenance_in_flight) {
         break;
      }
      BOOST_REQUIRE(std::chrono::steady_clock::now() < refresh_deadline);
      wait_on_runtime(client_runtime, std::chrono::milliseconds{10}, "DHT startup routing refresh");
   }
   const auto routing_before = client.routing_status(content_swarm_test_dht);
   const auto before = client.peers().find(server.local_peer());
   BOOST_REQUIRE(before.has_value());
   const auto endpoint_before = std::ranges::find_if(before->endpoints, [&](const auto& current) {
      return current.address.to_string() == server_endpoint.to_string();
   });
   BOOST_REQUIRE(endpoint_before != before->endpoints.end());
   const auto peer_failures_before = before->failures;
   const auto endpoint_failures_before = endpoint_before->failures;
   const auto direct_failures_before = client.metrics().direct_failures;

   auto release_server = block_runtime(server_runtime, "cached protocol timeout barrier");
   try {
      static_cast<void>(forge::asio::blocking::run(
          client_runtime,
          client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                            node::open_options{.allow_relay = false,
                                                               .timeout = std::chrono::milliseconds{150},
                                                               .direct_attempt_timeout = std::chrono::milliseconds{150},
                                                               .max_direct_endpoints = 1})));
      BOOST_FAIL("expected cached protocol open timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_CHECK(*exceptions::code_of(error) == exceptions::code::timeout);
   }

   const auto after = client.peers().find(server.local_peer());
   BOOST_REQUIRE(after.has_value());
   const auto endpoint_after = std::ranges::find_if(after->endpoints, [&](const auto& current) {
      return current.address.to_string() == server_endpoint.to_string();
   });
   BOOST_REQUIRE(endpoint_after != after->endpoints.end());
   BOOST_TEST(after->failures == peer_failures_before + 1U);
   BOOST_TEST(endpoint_after->failures == endpoint_failures_before + 1U);
   const auto backoff_applied = endpoint_after->backoff_until > std::chrono::system_clock::now();
   BOOST_TEST(backoff_applied);
   BOOST_TEST(client.metrics().direct_failures == direct_failures_before + 1U);
   BOOST_TEST(client.routing_status(content_swarm_test_dht).active == routing_before.active);

   release_server->set_value();
   forge::asio::blocking::run(client_runtime, client.async_stop());
   forge::asio::blocking::run(server_runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_stop_before_direct_path_deadline_does_not_penalize_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client = node{runtime, options_for(peer(211))};
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   const auto stalled_endpoint = start_stalling_tcp_peer(runtime, std::chrono::seconds{2}, accepted);
   const auto stalled_peer = peer(212);
   client.peers().learn_endpoint(stalled_peer, stalled_endpoint, capability_set{.bits = capabilities::direct_quic});
   const auto before = client.peers().find(stalled_peer);
   BOOST_REQUIRE(before.has_value());
   BOOST_REQUIRE(!before->endpoints.empty());
   const auto peer_failures_before = before->failures;
   const auto endpoint_failures_before = before->endpoints.front().failures;
   const auto endpoint_backoff_before = before->endpoints.front().backoff_until;
   const auto direct_failures_before = client.metrics().direct_failures;

   auto opened = boost::asio::co_spawn(
       runtime.context(),
       client.async_open_protocol_stream(stalled_peer, builtins::echo,
                                         node::open_options{.allow_relay = false,
                                                            .timeout = std::chrono::milliseconds{150},
                                                            .direct_attempt_timeout = std::chrono::milliseconds{150},
                                                            .max_direct_endpoints = 1}),
       boost::asio::use_future);
   wait_for_server(accepted_future, std::chrono::seconds{2}, "direct path shutdown admission");
   auto release_runtime = block_runtime(runtime, "direct path shutdown barrier");
   client.stop();
   std::this_thread::sleep_for(std::chrono::milliseconds{250});
   release_runtime->set_value();

   BOOST_REQUIRE(opened.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   try {
      static_cast<void>(opened.get());
      BOOST_FAIL("expected direct path shutdown");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      const auto code = *exceptions::code_of(error);
      BOOST_CHECK_MESSAGE(code == exceptions::code::canceled || code == exceptions::code::closed,
                          "unexpected shutdown code " << static_cast<int>(code));
   }
   forge::asio::blocking::run(runtime, client.async_stop());

   const auto after = client.peers().find(stalled_peer);
   BOOST_REQUIRE(after.has_value());
   BOOST_REQUIRE(!after->endpoints.empty());
   BOOST_TEST(after->failures == peer_failures_before);
   BOOST_TEST(after->endpoints.front().failures == endpoint_failures_before);
   const auto backoff_unchanged = after->endpoints.front().backoff_until == endpoint_backoff_before;
   BOOST_TEST(backoff_unchanged);
   BOOST_TEST(client.metrics().direct_failures == direct_failures_before);
}

BOOST_AUTO_TEST_CASE(p2p_direct_path_timeout_before_stop_penalizes_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = node{runtime, options_for(peer(213))};
   const auto stalled_endpoint = start_stalling_tcp_peer(runtime);
   const auto stalled_peer = peer(214);
   client.peers().learn_endpoint(stalled_peer, stalled_endpoint, capability_set{.bits = capabilities::direct_quic});
   const auto before = client.peers().find(stalled_peer);
   BOOST_REQUIRE(before.has_value());
   BOOST_REQUIRE(!before->endpoints.empty());
   const auto peer_failures_before = before->failures;
   const auto endpoint_failures_before = before->endpoints.front().failures;
   const auto direct_failures_before = client.metrics().direct_failures;

   try {
      static_cast<void>(forge::asio::blocking::run(
          runtime,
          client.async_open_protocol_stream(stalled_peer, builtins::echo,
                                            node::open_options{.allow_relay = false,
                                                               .timeout = std::chrono::milliseconds{1'000},
                                                               .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                               .max_direct_endpoints = 1})));
      BOOST_FAIL("expected direct path timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*exceptions::code_of(error)) == static_cast<int>(exceptions::code::timeout));
   }
   client.stop();

   const auto after = client.peers().find(stalled_peer);
   BOOST_REQUIRE(after.has_value());
   BOOST_REQUIRE(!after->endpoints.empty());
   BOOST_TEST(after->failures > peer_failures_before);
   BOOST_TEST(after->endpoints.front().failures > endpoint_failures_before);
   BOOST_TEST(after->endpoints.front().backoff_until > std::chrono::system_clock::now());
   BOOST_TEST(client.metrics().direct_failures > direct_failures_before);
   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_direct_path_global_budget_cancel_does_not_penalize_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto client = node{runtime, options_for(peer(213))};
   const auto stalled_endpoint = start_stalling_tcp_peer(runtime);
   const auto stalled_peer = peer(214);
   client.peers().learn_endpoint(stalled_peer, stalled_endpoint, capability_set{.bits = capabilities::direct_quic});
   const auto before = client.peers().find(stalled_peer);
   BOOST_REQUIRE(before.has_value());
   BOOST_REQUIRE(!before->endpoints.empty());
   const auto peer_failures_before = before->failures;
   const auto endpoint_failures_before = before->endpoints.front().failures;
   const auto endpoint_backoff_before = before->endpoints.front().backoff_until;
   const auto direct_failures_before = client.metrics().direct_failures;
   const auto direct_attempts_before = client.metrics().path_direct_attempts;

   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(
           runtime, client.async_open_protocol_stream(stalled_peer, builtins::echo,
               node::open_options{.allow_relay = false,
                                  .timeout = std::chrono::milliseconds{100},
                                  .direct_attempt_timeout = std::chrono::milliseconds{1'000},
                                  .max_direct_endpoints = 1})),
       forge::exceptions::base, [](const auto& error) {
          return exceptions::code_of(error) == exceptions::code::timeout;
       });
   BOOST_TEST(client.metrics().path_direct_attempts == direct_attempts_before + 1U);
   forge::asio::blocking::run(runtime, client.async_stop());

   const auto after = client.peers().find(stalled_peer);
   BOOST_REQUIRE(after.has_value());
   BOOST_REQUIRE(!after->endpoints.empty());
   BOOST_TEST(after->failures == peer_failures_before);
   BOOST_TEST(after->endpoints.front().failures == endpoint_failures_before);
   BOOST_CHECK(after->endpoints.front().backoff_until == endpoint_backoff_before);
   BOOST_TEST(client.metrics().direct_failures == direct_failures_before);
   BOOST_TEST(client.diagnostics().resources.active_dials == 0U);
   BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_path_manager_tries_next_direct_endpoint_after_attempt_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(64))};
   auto client_options = options_for(peer(65));
   client_options.direct_dial.max_concurrent_attempts = 1;
   auto client = node{runtime, std::move(client_options)};
   register_echo(server);

   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), make_quic_endpoint(9),
                                 capability_set{.bits = capabilities::direct_quic});
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                  node::open_options{
                                                      .allow_relay = false,
                                                      .timeout = std::chrono::milliseconds{2'000},
                                                      .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                      .max_direct_endpoints = 2,
                                                  }));
   const auto payload = std::vector<std::uint8_t>{'d', 'i', 'r', 'e', 'c', 't'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(client.metrics().path_direct_attempts >= 2U);
   BOOST_TEST(client.metrics().path_direct_opens >= 1U);
   auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   auto failed = std::ranges::find_if(record->endpoints, [&](const peer_store::endpoint_record& current) {
      return current.address.to_string() == make_quic_endpoint(9).to_string();
   });
   auto succeeded = std::ranges::find_if(record->endpoints, [&](const peer_store::endpoint_record& current) {
      return current.address.to_string() == server_endpoint.to_string();
   });
   BOOST_REQUIRE(failed != record->endpoints.end());
   BOOST_REQUIRE(succeeded != record->endpoints.end());
   BOOST_TEST(failed->failures >= 1U);
   BOOST_TEST(failed->backoff_until > std::chrono::system_clock::now());
   BOOST_TEST(succeeded->successes >= 1U);
   BOOST_TEST(succeeded->backoff_until == std::chrono::system_clock::time_point{});

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_path_manager_tries_next_quic_endpoint_after_silent_udp_attempt_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(64))};
   auto client_options = options_for(peer(65));
   client_options.direct_dial.max_concurrent_attempts = 1;
   auto client = node{runtime, std::move(client_options)};
   register_echo(server);

   // Keep ownership of an ephemeral bound port throughout the dial. It cannot
   // produce a closed-port ICMP refusal and deliberately never replies.
   // IPv6 ranks before the working IPv4 endpoint regardless of ephemeral ports.
   using udp = boost::asio::ip::udp;
   auto silent = udp::socket{runtime.context(), udp::endpoint{boost::asio::ip::make_address("::1"), 0}};
   const auto stalled_endpoint = endpoint{.transport = {.host_type = endpoint::host_kind::ip6,
                                                       .protocol = endpoint::protocol_kind::quic_v1,
                                                       .host = "::1",
                                                       .port = silent.local_endpoint().port()}};
   const auto server_endpoint = listen(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), stalled_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
           node::open_options{.allow_relay = false,
                              .timeout = std::chrono::milliseconds{2'000},
                              .direct_attempt_timeout = std::chrono::milliseconds{100},
                              .max_direct_endpoints = 2}));
   const auto payload = std::vector<std::uint8_t>{'s', 'i', 'l', 'e', 'n', 't'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(client.metrics().path_direct_attempts >= 2U);
   BOOST_TEST(client.metrics().path_direct_opens >= 1U);
   const auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   const auto failed = std::ranges::find_if(record->endpoints, [&](const auto& current) {
      return current.address.to_string() == stalled_endpoint.to_string();
   });
   const auto succeeded = std::ranges::find_if(record->endpoints, [&](const auto& current) {
      return current.address.to_string() == server_endpoint.to_string();
   });
   BOOST_REQUIRE(failed != record->endpoints.end());
   BOOST_REQUIRE(succeeded != record->endpoints.end());
   BOOST_TEST(failed->failures >= 1U);
   BOOST_TEST(failed->backoff_until > std::chrono::system_clock::now());
   BOOST_TEST(succeeded->successes >= 1U);
   BOOST_TEST(succeeded->backoff_until == std::chrono::system_clock::time_point{});

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_path_manager_tries_next_tcp_endpoint_after_upgrade_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(205))};
   auto client = node{runtime, options_for(peer(206))};
   register_echo(server);

   const auto stalled_endpoint = start_stalling_tcp_peer(runtime);
   const auto server_endpoint = listen_tcp(server, runtime);
   client.peers().learn_endpoint(server.local_peer(), stalled_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});
   client.peers().learn_endpoint(server.local_peer(), server_endpoint,
                                 capability_set{.bits = capabilities::direct_quic});

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                  node::open_options{
                                                      .allow_relay = false,
                                                      .timeout = std::chrono::milliseconds{2'000},
                                                      .direct_attempt_timeout = std::chrono::milliseconds{100},
                                                      .max_direct_endpoints = 2,
                                                  }));
   const auto payload = std::vector<std::uint8_t>{'t', 'c', 'p', '-', 'd', 'e', 'a', 'd'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());

   BOOST_TEST(reply == payload, boost::test_tools::per_element());
   BOOST_TEST(client.metrics().path_direct_attempts >= 2U);
   BOOST_TEST(client.metrics().path_direct_opens >= 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_successful_connect_deadline_does_not_poison_session) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = node{runtime, options_for(peer(41))};
   auto client = node{runtime, options_for(peer(42))};
   register_echo(server);

   const auto server_endpoint = listen(server, runtime);
   (void)forge::asio::blocking::run(
       runtime,
       client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer(),
                                                                   .timeout = std::chrono::milliseconds{500}}));
   wait_on_runtime(runtime, std::chrono::milliseconds{700}, "post-connect deadline grace");

   auto stream = forge::asio::blocking::run(
       runtime, client.async_open_protocol_stream(server.local_peer(), builtins::echo,
                                                  node::open_options{.timeout = std::chrono::milliseconds{1'000}}));
   const auto payload = std::vector<std::uint8_t>{'d', 'e', 'a', 'd', 'l', 'i', 'n', 'e'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   const auto reply = forge::asio::blocking::run(runtime, stream.async_read_frame());
   BOOST_TEST(reply == payload, boost::test_tools::per_element());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_duplicate_protocol_handler_is_rejected) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto value = node{runtime, options_for(peer(3))};
   register_echo(value);

   try {
      register_echo(value);
      BOOST_FAIL("expected duplicate protocol handler rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::duplicate_protocol));
   }
}

BOOST_AUTO_TEST_CASE(p2p_product_message_packs_typed_payload_as_data) {
   const auto protocol = protocol_id{.value = "/product/chunk-announce/1"};
   const auto value = product_announce{.ref = "chunk-1"};

   const auto message = forge::net::p2p::message{protocol, value};

   BOOST_TEST(message.protocol().value == protocol.value);
   BOOST_TEST(message.codec().value == "forge.raw");
   BOOST_TEST(message.as<product_announce>().ref == value.ref);
   BOOST_TEST(!message.data().empty());
}

BOOST_AUTO_TEST_CASE(p2p_connect_rejects_non_positive_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client = node{runtime, options_for(peer(32))};

   try {
      (void)forge::asio::blocking::run(
          runtime,
          client.async_connect(make_quic_endpoint(9), node::connect_options{.expected_peer = peer(33),
                                                                            .timeout = std::chrono::milliseconds{0}}));
      BOOST_FAIL("expected invalid connect timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::invalid_options));
   }

   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_open_protocol_rejects_non_positive_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto client = node{runtime, options_for(peer(39))};

   try {
      (void)forge::asio::blocking::run(
          runtime, client.async_open_protocol_stream(peer(40), builtins::echo,
                                                     node::open_options{.timeout = std::chrono::milliseconds{0}}));
      BOOST_FAIL("expected invalid protocol open timeout");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::invalid_options));
   }

   forge::asio::blocking::run(runtime, client.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_expires_stale_reachability_observation) {
   auto store = peer_store{};
   store.upsert(peer_store::record{
       .peer = peer(70),
       .capabilities = capability_set{.bits = capabilities::direct_quic},
       .reachability = reachability::state::publicly_reachable,
       .observed_endpoint = make_quic_endpoint(12345),
       .reachability_expires_at = std::chrono::system_clock::now() - std::chrono::seconds{1},
   });

   const auto record = store.find(peer(70));
   BOOST_REQUIRE(record.has_value());
   BOOST_TEST(static_cast<int>(record->reachability) == static_cast<int>(reachability::state::unknown));
   BOOST_TEST(!record->observed_endpoint.has_value());

   const auto snapshot = store.snapshot(1);
   BOOST_REQUIRE_EQUAL(snapshot.size(), 1U);
   BOOST_TEST(static_cast<int>(snapshot.front().reachability) == static_cast<int>(reachability::state::unknown));
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_identify_update_preserves_independent_endpoint_sources) {
   auto store = peer_store{};
   const auto remote = peer(223);
   const auto first = parse_endpoint("/ip4/127.0.0.1/udp/4223/quic-v1/p2p/" + remote.to_string());
   const auto second = parse_endpoint("/ip4/127.0.0.1/udp/4224/quic-v1/p2p/" + remote.to_string());
   store.learn_endpoint(remote, first);

   const auto updated = store.apply_identify(remote, peer_store::identify_update{
                                                         .protocol_version = "/forge/atomic-identify/1",
                                                         .protocols = std::vector<protocol_id>{builtins::identify},
                                                         .capabilities = capability_set{},
                                                         .unsigned_endpoints = addresses_for({second}),
                                                     });

   const auto found = store.find(remote);
   BOOST_REQUIRE(found);
   BOOST_TEST(found->protocol_version == "/forge/atomic-identify/1");
   BOOST_TEST(updated.protocol_version == found->protocol_version);
   BOOST_TEST(std::ranges::any_of(found->endpoints,
                                  [&](const auto& value) { return value.address.to_string() == first.to_string(); }));
   const auto identified = std::ranges::find_if(
       found->endpoints, [&](const auto& value) { return value.address.to_string() == second.to_string(); });
   BOOST_REQUIRE(identified != found->endpoints.end());
   BOOST_TEST(identified->sources.identify_unsigned);
   BOOST_TEST(!identified->sources.learned);

   const auto third = parse_endpoint("/ip4/127.0.0.1/udp/4225/quic-v1/p2p/" + remote.to_string());
   store.learn_endpoint(remote, third);
   const auto observed_at = std::chrono::system_clock::now();
   const auto discovered = store.apply_discovery(remote, peer_store::discovery_update{
                                                             .source = discovery::source::rendezvous,
                                                             .observed_at = observed_at,
                                                             .expires_at = observed_at + std::chrono::minutes{5},
                                                         });
   BOOST_REQUIRE(discovered);
   BOOST_TEST(static_cast<int>(discovered->discovered_by) == static_cast<int>(discovery::source::rendezvous));
   BOOST_TEST(discovered->discovered_at == observed_at);
   BOOST_TEST(std::ranges::any_of(discovered->endpoints,
                                  [&](const auto& value) { return value.address.to_string() == third.to_string(); }));

   store.upsert_relay_reservation(peer_store::relay_record{
       .relay = remote,
       .reservation_id = 17,
       .expires_at = observed_at + std::chrono::minutes{2},
       .endpoints = {third},
   });
   const auto with_relay = store.find(remote);
   BOOST_REQUIRE(with_relay);
   BOOST_TEST(with_relay->protocol_version == "/forge/atomic-identify/1");
   BOOST_TEST(std::ranges::any_of(with_relay->endpoints,
                                  [&](const auto& value) { return value.address.to_string() == first.to_string(); }));
   BOOST_REQUIRE_EQUAL(with_relay->relay_reservations.size(), 1U);
   BOOST_TEST(with_relay->relay_reservations.front().reservation_id == 17U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_exchange_preserves_identify_discovery_and_relay_state) {
   auto store = peer_store{};
   const auto remote_identity = make_test_identity();
   const auto remote = remote_identity.peer;
   const auto public_key_bytes = encode_public_key(remote_identity.key);
   const auto now = std::chrono::system_clock::now();
   const auto original_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4227/quic-v1/p2p/" + remote.to_string());
   const auto exchanged_endpoint = parse_endpoint("/ip4/127.0.0.1/udp/4228/quic-v1/p2p/" + remote.to_string());
   store.upsert(peer_store::record{
       .peer = remote,
       .capabilities = capability_set{.bits = capabilities::direct_quic},
       .discovered_by = discovery::source::rendezvous,
       .protocol_version = "/ipfs/id/1.0.0",
       .agent_version = "forge-preserved",
       .public_key = public_key_bytes,
       .protocols = {builtins::identify},
       .signed_peer_record = {0x03, 0x04},
       .endpoints = {peer_store::endpoint_record{.address = address_for(original_endpoint)}},
       .relay_reservations = {peer_store::relay_record{
           .relay = remote,
           .reservation_id = 23,
           .expires_at = now + std::chrono::minutes{5},
       }},
       .discovered_at = now,
       .discovery_expires_at = now + std::chrono::minutes{10},
   });

   detail::learn_authenticated_peer_exchange_response(
       store,
       peer_exchange_message{
           .kind = peer_exchange_message::type::peer_exchange_response,
           .peer = remote,
           .capabilities = capability_set{.bits = capabilities::peer_exchange},
           .endpoints = {peer_exchange_message::endpoint_record{
               .peer = remote,
               .address = address_for(exchanged_endpoint),
               .capabilities = capability_set{.bits = capabilities::pubsub},
           }},
       },
       remote, exchanged_endpoint);

   const auto stored = store.find(remote);
   BOOST_REQUIRE(stored);
   BOOST_TEST(stored->protocol_version == "/ipfs/id/1.0.0");
   BOOST_TEST(stored->agent_version == "forge-preserved");
   BOOST_TEST(stored->public_key == public_key_bytes);
   BOOST_TEST(stored->signed_peer_record == (std::vector<std::uint8_t>{0x03, 0x04}));
   BOOST_TEST(static_cast<int>(stored->discovered_by) == static_cast<int>(discovery::source::rendezvous));
   BOOST_TEST(stored->discovered_at == now);
   BOOST_REQUIRE_EQUAL(stored->relay_reservations.size(), 1U);
   BOOST_TEST(stored->relay_reservations.front().reservation_id == 23U);
   BOOST_TEST(stored->capabilities.has(capabilities::direct_quic));
   BOOST_TEST(!stored->capabilities.has(capabilities::peer_exchange));
   BOOST_TEST(!stored->capabilities.has(capabilities::pubsub));
   BOOST_TEST(std::ranges::any_of(stored->endpoints, [&](const auto& value) {
      return value.address.to_string() == original_endpoint.to_string();
   }));
   BOOST_TEST(std::ranges::any_of(stored->endpoints, [&](const auto& value) {
      return value.address.to_string() == exchanged_endpoint.to_string();
   }));
}

BOOST_AUTO_TEST_CASE(p2p_relay_maintenance_preserves_concurrent_peer_facts) {
   auto store = peer_store{};
   const auto remote = peer(228);
   const auto now = std::chrono::system_clock::now();
   for (auto round = 0U; round < 64U; ++round) {
      const auto identified_agent = "forge-concurrent-identify-" + std::to_string(round);
      store.upsert(peer_store::record{
          .peer = remote,
          .protocol_version = "/ipfs/id/1.0.0",
          .agent_version = "forge-before-maintenance",
          .protocols = {builtins::identify},
          .relay_reservations =
              {
                  peer_store::relay_record{
                      .relay = remote, .reservation_id = 31, .expires_at = now - std::chrono::seconds{1}},
                  peer_store::relay_record{
                      .relay = remote, .reservation_id = 32, .expires_at = now + std::chrono::minutes{5}},
              },
      });

      auto start_promise = std::promise<void>{};
      auto start = start_promise.get_future().share();
      auto backoff = std::async(std::launch::async, [&] {
         start.wait();
         relay_discovery::backoff_candidate(store, remote, now + std::chrono::seconds{30});
      });
      auto identify = std::async(std::launch::async, [&] {
         start.wait();
         static_cast<void>(
             store.apply_identify(remote, peer_store::identify_update{.agent_version = identified_agent}));
      });
      auto prune = std::async(std::launch::async, [&] {
         start.wait();
         relay_discovery::prune_expired_reservations(store, now, 1);
      });
      start_promise.set_value();
      backoff.get();
      identify.get();
      prune.get();

      const auto stored = store.find(remote);
      BOOST_REQUIRE(stored);
      BOOST_TEST(stored->protocol_version == "/ipfs/id/1.0.0");
      BOOST_TEST(stored->agent_version == identified_agent);
      BOOST_TEST(stored->failures == 1U);
      BOOST_TEST(stored->discovery_backoff_until == now + std::chrono::seconds{30});
      BOOST_REQUIRE_EQUAL(stored->relay_reservations.size(), 1U);
      BOOST_TEST(stored->relay_reservations.front().reservation_id == 32U);
   }
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_bounds_variable_peer_record_state) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto subject = peer(70);
   auto store = peer_store{peer_store::options{
       .max_endpoints_per_peer = 1,
       .max_protocols_per_peer = 1,
       .max_relay_reservations_per_peer = 1,
       .max_relay_endpoints_per_reservation = 1,
       .max_peer_record_bytes = 128,
   }};

   store.learn_endpoint(subject, make_quic_endpoint(4070));
   BOOST_CHECK_THROW(store.learn_endpoint(subject, make_quic_endpoint(4071)), exceptions::backpressure_rejected);
   const auto stored = store.find(subject);
   BOOST_REQUIRE(stored.has_value());
   BOOST_REQUIRE_EQUAL(stored->endpoints.size(), 1U);
   BOOST_TEST(stored->endpoints.front().address.to_string() == make_quic_endpoint(4070).to_string());

   BOOST_CHECK_THROW(store.upsert(peer_store::record{
                         .peer = peer(71),
                         .protocols = std::vector<protocol_id>{{.value = "/forge/test/1"}, {.value = "/forge/test/2"}},
                     }),
                     exceptions::backpressure_rejected);
   BOOST_CHECK_THROW(store.upsert(peer_store::record{
                         .peer = peer(72),
                         .relay_reservations = std::vector<peer_store::relay_record>{peer_store::relay_record{
                             .relay = peer(73),
                             .endpoints = std::vector<endpoint>{make_quic_endpoint(4072), make_quic_endpoint(4073)},
                         }},
                     }),
                     exceptions::backpressure_rejected);
   BOOST_CHECK_THROW(store.upsert(peer_store::record{
                         .peer = peer(74),
                         .agent_version = std::string(129, 'x'),
                     }),
                     exceptions::backpressure_rejected);
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                                .namespace_name = "forge.bounds",
                                                .peer = peer(77),
                                                .signed_peer_record = std::vector<std::uint8_t>(129, 0x32U),
                                            }))),
       exceptions::backpressure_rejected);
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(
           runtime, store.async_upsert_rendezvous(rendezvous::registration{
                        .namespace_name = "forge.bounds",
                        .peer = peer(78),
                        .endpoints = addresses_for({make_quic_endpoint(4076), make_quic_endpoint(4077)}),
                    }))),
       exceptions::backpressure_rejected);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_rejects_oversized_hydration_page_atomically) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = peer_store::make_memory_persistence();
   forge::asio::blocking::run(
       runtime,
       persistence->async_apply(peer_store::mutation_batch{
           .peer_upserts =
               std::vector<peer_store::record>{
                   peer_store::record{.peer = peer(75)},
                   peer_store::record{
                       .peer = peer(76),
                       .protocols = std::vector<protocol_id>{{.value = "/forge/test/1"}, {.value = "/forge/test/2"}},
                   },
               },
       }));
   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .max_protocols_per_peer = 1,
   }};

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, store.async_hydrate()), exceptions::backpressure_rejected);
   BOOST_TEST(store.snapshot(10).empty());
   BOOST_TEST(store.persistence_state().degraded);

   forge::asio::blocking::run(runtime, store.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_rejects_oversized_rendezvous_hydration_atomically) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto rendezvous_persistence = peer_store::make_memory_persistence();
   (void)forge::asio::blocking::run(
       runtime, rendezvous_persistence->async_apply(peer_store::mutation_batch{
                    .rendezvous_upserts = std::vector<rendezvous::registration>{rendezvous::registration{
                        .namespace_name = "forge.bounds",
                        .peer = peer(80),
                        .signed_peer_record = std::vector<std::uint8_t>(129, 0x42U),
                        .sequence = 1,
                    }},
                    .rendezvous_sequence_high_watermark = 1,
                }));
   auto rendezvous_store = peer_store{peer_store::options{
       .persistence = rendezvous_persistence,
       .max_peer_record_bytes = 128,
   }};

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, rendezvous_store.async_hydrate()),
                     exceptions::backpressure_rejected);
   BOOST_TEST(rendezvous_store.discover_rendezvous("forge.bounds", 0, 1).empty());
   forge::asio::blocking::run(runtime, rendezvous_store.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_memory_persistence_hydrates_bounded_pages) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   auto batch = peer_store::mutation_batch{};
   for (auto index = std::uint8_t{}; index < 5; ++index) {
      batch.peer_upserts.push_back(peer_store::record{
          .peer = peer(static_cast<std::uint8_t>(110 + index)),
          .protocol_version = "/forge/hydrated/1",
          .discovery_expires_at = expires_at,
      });
   }
   for (auto index = std::uint8_t{}; index < 3; ++index) {
      batch.rendezvous_upserts.push_back(rendezvous::registration{
          .namespace_name = "forge.paged",
          .peer = peer(static_cast<std::uint8_t>(130 + index)),
          .ttl = std::chrono::hours{1},
          .expires_at = expires_at,
          .sequence = static_cast<std::uint64_t>(index + 1),
      });
   }
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(batch)));

   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .max_peers = 5,
       .max_rendezvous = 3,
       .max_pending = 5,
       .hydration_page_limit = 2,
       .prune_page_limit = 2,
   }};
   forge::asio::blocking::run(runtime, store.async_hydrate());

   BOOST_REQUIRE_EQUAL(store.snapshot(10).size(), 5U);
   BOOST_REQUIRE_EQUAL(store.discover_rendezvous("forge.paged", 0, 2).size(), 2U);
   BOOST_TEST(std::ranges::all_of(persistence->hydration_requests,
                                  [](const auto& request) { return request.limit > 0 && request.limit <= 2; }));
   BOOST_TEST(std::ranges::count_if(persistence->hydration_requests, [](const auto& request) {
                 return request.kind == peer_store::hydration_kind::peers;
              }) == 3U);
   BOOST_TEST(std::ranges::count_if(persistence->hydration_requests, [](const auto& request) {
                 return request.kind == peer_store::hydration_kind::rendezvous;
              }) == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_memory_persistence_paginates_deterministically) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = peer_store::make_memory_persistence();
   auto batch = peer_store::mutation_batch{};
   for (const auto value : std::vector<std::uint8_t>{175, 171, 174, 172, 173}) {
      batch.peer_upserts.push_back(peer_store::record{.peer = peer(value)});
   }
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(batch)));

   auto hydrated = std::vector<peer_id>{};
   auto cursor = std::optional<std::vector<std::byte>>{};
   do {
      auto page = forge::asio::blocking::run(runtime, persistence->async_hydrate(peer_store::hydration_request{
                                                          .kind = peer_store::hydration_kind::peers,
                                                          .cursor = cursor,
                                                          .limit = 2,
                                                      }));
      BOOST_TEST(page.peers.size() <= 2U);
      for (const auto& value : page.peers) {
         hydrated.push_back(value.peer);
      }
      cursor = std::move(page.cursor);
   } while (cursor);

   BOOST_REQUIRE_EQUAL(hydrated.size(), 5U);
   BOOST_TEST(std::ranges::is_sorted(hydrated));
   const auto unique = std::set<peer_id>{hydrated.begin(), hydrated.end()};
   BOOST_TEST(unique.size() == hydrated.size());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_hydration_durably_removes_peer_displaced_by_concurrent_mutation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_hydrate = true;
   persistence->retain_delegate_on_close = true;
   const auto durable_peer = peer(184);
   const auto concurrent_peer = peer(185);
   auto initial = peer_store::mutation_batch{};
   initial.peer_upserts.push_back(peer_store::record{.peer = durable_peer, .protocol_version = "/forge/old/1"});
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(initial)));

   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .max_peers = 1,
       .max_pending = 2,
   }};
   auto hydration = boost::asio::co_spawn(runtime.context(), store.async_hydrate(), boost::asio::use_future);
   BOOST_REQUIRE(persistence->wait_until_hydrate_blocked());
   store.upsert(peer_store::record{.peer = concurrent_peer, .protocol_version = "/forge/new/1"});
   persistence->release_hydrate();
   hydration.get();
   persistence->block_hydrate = false;
   forge::asio::blocking::run(runtime, store.async_flush());

   auto reopened = peer_store{peer_store::options{
       .persistence = persistence,
       .max_peers = 1,
       .max_pending = 2,
   }};
   forge::asio::blocking::run(runtime, reopened.async_hydrate());
   BOOST_REQUIRE_EQUAL(reopened.snapshot(10).size(), 1U);
   BOOST_TEST(reopened.find(concurrent_peer).has_value());
   BOOST_TEST(!reopened.find(durable_peer).has_value());
   BOOST_TEST(persistence->applied_peer_removals == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_rendezvous_high_watermark_survives_deleted_highest_registration) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->retain_delegate_on_close = true;
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   auto highest_sequence = std::uint64_t{};

   {
      auto store = peer_store{peer_store::options{.persistence = persistence}};
      forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                              .namespace_name = "forge.sequence",
                                              .peer = peer(176),
                                              .ttl = std::chrono::hours{1},
                                              .expires_at = expires_at,
                                          }));
      forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                              .namespace_name = "forge.sequence",
                                              .peer = peer(177),
                                              .ttl = std::chrono::hours{1},
                                              .expires_at = expires_at,
                                          }));
      const auto registrations = store.discover_rendezvous("forge.sequence", 0, 10);
      BOOST_REQUIRE_EQUAL(registrations.size(), 2U);
      highest_sequence = registrations.back().sequence;
      forge::asio::blocking::run(runtime, store.async_remove_rendezvous(peer(176), "forge.sequence"));
      forge::asio::blocking::run(runtime, store.async_remove_rendezvous(peer(177), "forge.sequence"));
      forge::asio::blocking::run(runtime, store.async_close());
   }

   auto reopened = peer_store{peer_store::options{.persistence = persistence}};
   forge::asio::blocking::run(runtime, reopened.async_hydrate());
   forge::asio::blocking::run(runtime, reopened.async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.sequence",
                                           .peer = peer(178),
                                           .ttl = std::chrono::hours{1},
                                           .expires_at = expires_at,
                                       }));
   const auto added = reopened.discover_rendezvous("forge.sequence", highest_sequence, 10);
   BOOST_REQUIRE_EQUAL(added.size(), 1U);
   BOOST_TEST(added.front().sequence > highest_sequence);
   BOOST_TEST(persistence->applied_rendezvous_high_watermark == added.front().sequence);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_rejects_exhausted_rendezvous_sequence) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = peer_store::make_memory_persistence();
   auto exhausted = peer_store::mutation_batch{};
   exhausted.rendezvous_sequence_high_watermark = std::numeric_limits<std::uint64_t>::max();
   forge::asio::blocking::run(runtime, persistence->async_apply(std::move(exhausted)));

   auto store = peer_store{peer_store::options{.persistence = persistence}};
   forge::asio::blocking::run(runtime, store.async_hydrate());
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                                .namespace_name = "forge.sequence.exhausted",
                                                .peer = peer(186),
                                                .ttl = std::chrono::hours{1},
                                                .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                                            }))),
       exceptions::sequence_exhausted);
   BOOST_TEST(store.discover_rendezvous("forge.sequence.exhausted", 0, 10).empty());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_memory_persistence_applies_flushes_and_prunes_bounded_state) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .prune_page_limit = 2,
   }};
   const auto now = std::chrono::system_clock::now();
   const auto expired_at = now - std::chrono::seconds{1};
   const auto live_until = now + std::chrono::hours{1};

   store.upsert(peer_store::record{.peer = peer(140), .discovery_expires_at = expired_at});
   store.upsert(peer_store::record{.peer = peer(141), .discovery_expires_at = expired_at});
   store.upsert(peer_store::record{.peer = peer(142), .discovery_expires_at = live_until});
   forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.prune",
                                           .peer = peer(145),
                                           .ttl = std::chrono::hours{1},
                                           .expires_at = expired_at,
                                       }));
   forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.prune",
                                           .peer = peer(146),
                                           .ttl = std::chrono::hours{1},
                                           .expires_at = live_until,
                                       }));
   forge::asio::blocking::run(runtime, store.async_flush());

   BOOST_TEST(persistence->durable_apply_attempts == 2U);

   const auto first = forge::asio::blocking::run(runtime, store.async_prune_expired(now));
   const auto second = forge::asio::blocking::run(runtime, store.async_prune_expired(now));
   const auto third = forge::asio::blocking::run(runtime, store.async_prune_expired(now));
   BOOST_TEST(first.peers.size() == 2U);
   BOOST_TEST(first.may_have_more);
   BOOST_TEST(second.rendezvous_registrations.size() == 1U);
   BOOST_TEST(!second.may_have_more);
   BOOST_TEST(third.peers.empty());
   BOOST_TEST(third.rendezvous_registrations.empty());
   BOOST_TEST(!third.may_have_more);
   BOOST_TEST(std::ranges::all_of(persistence->prune_limits, [](auto limit) { return limit == 2U; }));
   BOOST_REQUIRE_EQUAL(store.snapshot(10).size(), 1U);
   BOOST_REQUIRE_EQUAL(store.discover_rendezvous("forge.prune", 0, 10).size(), 1U);
   forge::asio::blocking::run(runtime, store.async_remove_rendezvous(peer(146), "forge.prune"));
   BOOST_TEST(store.discover_rendezvous("forge.prune", 0, 10).empty());
   BOOST_TEST(persistence->applied_rendezvous_removals == 1U);
   BOOST_TEST(persistence->durable_apply_attempts == 3U);
   forge::asio::blocking::run(runtime, store.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_bounds_pending_queue_and_recovers_after_flush) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .max_peers = 8,
       .max_pending = 2,
   }};

   store.upsert(peer_store::record{.peer = peer(150), .protocol_version = "/forge/queued/1"});
   store.upsert(peer_store::record{.peer = peer(151), .protocol_version = "/forge/queued/1"});
   BOOST_TEST(store.persistence_state().pending_peer_mutations == 2U);
   BOOST_CHECK_THROW(store.upsert(peer_store::record{.peer = peer(152)}), exceptions::backpressure_rejected);
   BOOST_TEST(!store.find(peer(152)).has_value());

   forge::asio::blocking::run(runtime, store.async_flush());
   BOOST_TEST(store.persistence_state().pending_peer_mutations == 0U);
   BOOST_TEST(persistence->applied_peer_upserts == 2U);

   store.upsert(peer_store::record{.peer = peer(152), .protocol_version = "/forge/queued/1"});
   forge::asio::blocking::run(runtime, store.async_flush());
   BOOST_TEST(persistence->applied_peer_upserts == 3U);
   forge::asio::blocking::run(runtime, store.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_eviction_supersedes_older_in_flight_upsert_durably) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_apply = true;
   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .max_peers = 1,
       .max_pending = 2,
   }};

   store.upsert(peer_store::record{.peer = peer(181), .protocol_version = "/forge/evicted/1"});
   auto first_flush = boost::asio::co_spawn(runtime.context(), store.async_flush(), boost::asio::use_future);
   BOOST_REQUIRE(persistence->wait_until_apply_blocked());

   store.upsert(peer_store::record{.peer = peer(180), .protocol_version = "/forge/retained/1"});
   BOOST_TEST(!store.find(peer(181)).has_value());
   BOOST_REQUIRE(store.find(peer(180)).has_value());
   BOOST_TEST(store.persistence_state().pending_peer_mutations == 2U);

   persistence->release_apply();
   first_flush.get();
   persistence->block_apply = false;
   forge::asio::blocking::run(runtime, store.async_flush());
   BOOST_TEST(persistence->applied_peer_upserts == 2U);
   BOOST_TEST(persistence->applied_peer_removals == 1U);

   auto reopened = peer_store{peer_store::options{
       .persistence = persistence,
       .max_peers = 1,
       .max_pending = 2,
   }};
   forge::asio::blocking::run(runtime, reopened.async_hydrate());
   BOOST_REQUIRE_EQUAL(reopened.snapshot(10).size(), 1U);
   BOOST_TEST(reopened.find(peer(180)).has_value());
   BOOST_TEST(!reopened.find(peer(181)).has_value());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_bounds_persistence_waiters_before_gate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_apply = true;
   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .max_persistence_waiters = 1,
   }};
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};

   auto first = boost::asio::co_spawn(runtime.context(),
                                      store.async_upsert_rendezvous(rendezvous::registration{
                                          .namespace_name = "forge.gate",
                                          .peer = peer(182),
                                          .ttl = std::chrono::hours{1},
                                          .expires_at = expires_at,
                                      }),
                                      boost::asio::use_future);
   BOOST_REQUIRE(persistence->wait_until_apply_blocked());
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_flush())), exceptions::backpressure_rejected);

   auto closing = boost::asio::co_spawn(runtime.context(), store.async_close(), boost::asio::use_future);
   for (auto attempt = 0; attempt < 200 && !store.persistence_state().closing; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
   }
   BOOST_REQUIRE(store.persistence_state().closing);
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_close())), exceptions::backpressure_rejected);
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_flush())), exceptions::closed);

   persistence->release_apply();
   first.get();
   persistence->block_apply = false;
   closing.get();
   BOOST_TEST(store.persistence_state().closed);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_canceled_close_releases_bounded_waiter_and_allows_retry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_apply = true;
   auto store = peer_store{peer_store::options{
       .persistence = persistence,
       .max_persistence_waiters = 1,
   }};
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   auto applying = boost::asio::co_spawn(runtime.context(),
                                         store.async_upsert_rendezvous(rendezvous::registration{
                                             .namespace_name = "forge.close",
                                             .peer = peer(187),
                                             .ttl = std::chrono::hours{1},
                                             .expires_at = expires_at,
                                         }),
                                         boost::asio::use_future);
   BOOST_REQUIRE(persistence->wait_until_apply_blocked());

   auto cancellation = boost::asio::cancellation_signal{};
   auto canceled_close =
       boost::asio::co_spawn(runtime.context(), store.async_close(),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   for (auto attempt = 0; attempt < 200 && !store.persistence_state().closing; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
   }
   BOOST_REQUIRE(store.persistence_state().closing);
   cancellation.emit(boost::asio::cancellation_type::terminal);
   BOOST_REQUIRE(canceled_close.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(canceled_close.get(), exceptions::canceled);

   auto retry = boost::asio::co_spawn(runtime.context(), store.async_close(), boost::asio::use_future);
   persistence->release_apply();
   applying.get();
   persistence->block_apply = false;
   retry.get();
   BOOST_TEST(store.persistence_state().closed);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_prune_preserves_newer_synchronous_peer_mutation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_prune = true;
   auto store = peer_store{peer_store::options{.persistence = persistence}};
   const auto expired_at = std::chrono::system_clock::now() - std::chrono::seconds{1};
   const auto id = peer(183);
   store.upsert(peer_store::record{
       .peer = id,
       .protocol_version = "/forge/prune/old",
       .discovery_expires_at = expired_at,
   });

   auto pruning = boost::asio::co_spawn(runtime.context(), store.async_prune_expired(), boost::asio::use_future);
   BOOST_REQUIRE(persistence->wait_until_prune_blocked());
   store.upsert(peer_store::record{
       .peer = id,
       .protocol_version = "/forge/prune/new",
       .discovery_expires_at = expired_at,
   });
   persistence->release_prune();
   const auto result = pruning.get();
   BOOST_REQUIRE_EQUAL(result.peers.size(), 1U);
   BOOST_TEST(result.peers.front().value == id.value);

   const auto current = store.find(id);
   BOOST_REQUIRE(current.has_value());
   BOOST_TEST(current->protocol_version == "/forge/prune/new");
   persistence->block_prune = false;
   forge::asio::blocking::run(runtime, store.async_flush());

   auto reopened = peer_store{peer_store::options{.persistence = persistence}};
   forge::asio::blocking::run(runtime, reopened.async_hydrate());
   const auto durable = reopened.find(id);
   BOOST_REQUIRE(durable.has_value());
   BOOST_TEST(durable->protocol_version == "/forge/prune/new");
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_reports_persistence_failures_and_retries_pending_batch) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto store = peer_store{peer_store::options{.persistence = persistence}};
   store.upsert(peer_store::record{.peer = peer(153), .protocol_version = "/forge/retry/1"});

   persistence->fail_apply = true;
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_flush())), std::runtime_error);
   auto status = store.persistence_state();
   BOOST_TEST(status.pending_peer_mutations == 1U);
   BOOST_TEST(status.failure_count == 1U);
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.last_failure.find("injected peer apply failure") != std::string::npos);

   persistence->fail_apply = false;
   forge::asio::blocking::run(runtime, store.async_flush());
   status = store.persistence_state();
   BOOST_TEST(status.pending_peer_mutations == 0U);
   BOOST_TEST(!status.degraded);
   BOOST_TEST(persistence->applied_peer_upserts == 1U);

   persistence->fail_hydrate = true;
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_hydrate())), std::runtime_error);
   persistence->fail_hydrate = false;
   persistence->fail_prune = true;
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_prune_expired())), std::runtime_error);
   persistence->fail_prune = false;
   store.mark_success(peer(153), path::kind::direct, std::chrono::milliseconds{1});
   persistence->fail_flush = true;
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_flush())), exceptions::durability_uncertain);
   status = store.persistence_state();
   BOOST_TEST(status.pending_peer_mutations == 0U);
   BOOST_TEST(status.degraded);
   persistence->fail_flush = false;
   (void)forge::asio::blocking::run(runtime, store.async_prune_expired());
   BOOST_TEST(store.persistence_state().degraded);
   forge::asio::blocking::run(runtime, store.async_flush());
   BOOST_TEST(!store.persistence_state().degraded);
   BOOST_TEST(store.persistence_state().failure_count == 4U);
   forge::asio::blocking::run(runtime, store.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_node_diagnostics_report_peer_persistence_degradation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto options = options_for(peer(232));
   options.peer_state.persistence = persistence;
   auto value = node{runtime, std::move(options)};
   value.peers().upsert(peer_store::record{.peer = peer(233), .protocol_version = "/forge/diagnostics/1"});

   persistence->fail_apply = true;
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, value.peers().async_flush())), std::runtime_error);
   const auto degraded = value.diagnostics().persistence;
   BOOST_TEST(degraded.degraded);
   BOOST_TEST(degraded.failure_count == 1U);
   BOOST_TEST(degraded.pending_peer_mutations == 1U);
   BOOST_TEST(degraded.last_failure.find("injected peer apply failure") != std::string::npos);

   persistence->fail_apply = false;
   forge::asio::blocking::run(runtime, value.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_applies_committed_state_when_durable_acknowledgement_is_uncertain) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->uncertain_durable_apply = true;
   auto store = peer_store{peer_store::options{.persistence = persistence}};
   const auto registered_peer = peer(188);
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};

   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                                              .namespace_name = "forge.uncertain",
                                                              .peer = registered_peer,
                                                              .ttl = std::chrono::hours{1},
                                                              .expires_at = expires_at,
                                                          }))),
                     exceptions::durability_uncertain);
   const auto status = store.persistence_state();
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.last_failure.find("uncertain durable acknowledgement") != std::string::npos);
   const auto operational = store.discover_rendezvous("forge.uncertain", 0, 1);
   BOOST_REQUIRE_EQUAL(operational.size(), 1U);
   BOOST_TEST(operational.front().peer.value == registered_peer.value);

   auto reopened = peer_store{peer_store::options{.persistence = persistence->delegate}};
   forge::asio::blocking::run(runtime, reopened.async_hydrate());
   const auto durable = reopened.discover_rendezvous("forge.uncertain", 0, 1);
   BOOST_REQUIRE_EQUAL(durable.size(), 1U);
   BOOST_TEST(durable.front().peer.value == registered_peer.value);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_enforces_rendezvous_backpressure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto store = peer_store{peer_store::options{
       .persistence = peer_store::make_memory_persistence(),
       .max_rendezvous = 1,
   }};
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.capacity",
                                           .peer = peer(156),
                                           .ttl = std::chrono::hours{1},
                                           .expires_at = expires_at,
                                       }));
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                                              .namespace_name = "forge.capacity",
                                                              .peer = peer(157),
                                                              .ttl = std::chrono::hours{1},
                                                              .expires_at = expires_at,
                                                          }))),
                     exceptions::backpressure_rejected);
   forge::asio::blocking::run(runtime, store.async_upsert_rendezvous(rendezvous::registration{
                                           .namespace_name = "forge.capacity",
                                           .peer = peer(156),
                                           .ttl = std::chrono::hours{1},
                                           .expires_at = expires_at,
                                       }));
   BOOST_REQUIRE_EQUAL(store.discover_rendezvous("forge.capacity", 0, 10).size(), 1U);
   forge::asio::blocking::run(runtime, store.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_enforces_rendezvous_capacity_per_peer_before_persistence) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto store = peer_store{peer_store::options{.persistence = persistence}};
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   const auto first_peer = peer(227);
   const auto second_peer = peer(228);
   const auto registration = [&](std::string namespace_name, peer_id id) {
      return rendezvous::registration{
          .namespace_name = std::move(namespace_name),
          .peer = std::move(id),
          .ttl = std::chrono::hours{1},
          .expires_at = expires_at,
      };
   };

   forge::asio::blocking::run(runtime, store.async_register_rendezvous(registration("forge.first", first_peer), 1));
   forge::asio::blocking::run(runtime, store.async_register_rendezvous(registration("forge.first", first_peer), 1));
   const auto applied_before_rejection = persistence->applied_rendezvous_upserts;
   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         runtime, store.async_register_rendezvous(registration("forge.second", first_peer), 1))),
                     exceptions::backpressure_rejected);
   BOOST_TEST(persistence->applied_rendezvous_upserts == applied_before_rejection);
   forge::asio::blocking::run(runtime, store.async_register_rendezvous(registration("forge.second", second_peer), 1));
   forge::asio::blocking::run(runtime, store.async_remove_rendezvous(first_peer, "forge.first"));
   forge::asio::blocking::run(runtime, store.async_register_rendezvous(registration("forge.second", first_peer), 1));
   BOOST_REQUIRE_EQUAL(store.discover_rendezvous("forge.second", 0, 10).size(), 2U);
   forge::asio::blocking::run(runtime, store.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_close_flushes_pending_state_and_is_idempotent) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto store = peer_store{peer_store::options{.persistence = persistence}};
   store.upsert(peer_store::record{.peer = peer(158), .protocol_version = "/forge/close/1"});

   forge::asio::blocking::run(runtime, store.async_close());
   const auto status = store.persistence_state();
   BOOST_TEST(status.pending_peer_mutations == 0U);
   BOOST_TEST(status.closed);
   BOOST_TEST(!status.closing);
   BOOST_TEST(persistence->applied_peer_upserts == 1U);
   BOOST_TEST(persistence->flush_attempts == 1U);
   BOOST_TEST(persistence->close_attempts == 1U);

   forge::asio::blocking::run(runtime, store.async_close());
   BOOST_TEST(persistence->close_attempts == 1U);
   BOOST_CHECK_THROW(store.upsert(peer_store::record{.peer = peer(159)}), exceptions::closed);
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_flush())), exceptions::closed);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_close_failure_is_degraded_and_retryable) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto store = peer_store{peer_store::options{.persistence = persistence}};
   store.upsert(peer_store::record{.peer = peer(160), .protocol_version = "/forge/close-retry/1"});

   persistence->fail_close = true;
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, store.async_close())), std::runtime_error);
   auto status = store.persistence_state();
   BOOST_TEST(status.closing);
   BOOST_TEST(!status.closed);
   BOOST_TEST(status.degraded);
   BOOST_TEST(status.last_failure.find("injected peer close failure") != std::string::npos);
   BOOST_CHECK_THROW(store.upsert(peer_store::record{.peer = peer(161)}), exceptions::closed);

   persistence->fail_close = false;
   forge::asio::blocking::run(runtime, store.async_close());
   status = store.persistence_state();
   BOOST_TEST(!status.closing);
   BOOST_TEST(status.closed);
   BOOST_TEST(!status.degraded);
   BOOST_TEST(persistence->flush_attempts == 2U);
   BOOST_TEST(persistence->close_attempts == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_peer_store_awaitable_owns_impl_before_first_resume) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   auto pending = std::optional<boost::asio::awaitable<void>>{};
   {
      auto store = peer_store{peer_store::options{.persistence = persistence}};
      pending.emplace(store.async_hydrate());
   }

   forge::asio::blocking::run(runtime, std::move(*pending));
   BOOST_TEST(!persistence->hydration_requests.empty());
}

BOOST_AUTO_TEST_CASE(p2p_dht_awaitables_own_node_impl_before_first_resume) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto identity = make_test_certificate_identity("dht-owned-awaitable");
   auto options = dht_options_for(identity, custom_test_dht_profile());
   options.dht_profiles.push_back(custom_test_value_dht_profile());
   auto advertised = make_dns_tcp_endpoint(4'192, "dht-owned-awaitable.example.com");
   advertised.peer = identity.peer;
   options.advertised_endpoints.push_back(std::move(advertised));
   auto source = node{runtime, std::move(options)};
   forge::asio::blocking::run(runtime, source.async_hydrate_peer_state());

   const auto provider_key = make_dht_key(std::vector<std::uint8_t>{'o', 'w', 'n', 'e', 'd'});
   const auto value_key = make_dht_key(std::vector<std::uint8_t>{'/', 'o', 'w', 'n', 'e', 'd'});
   auto find_peer = source.async_find_peer(content_swarm_test_dht, peer(219));
   auto provide = source.async_provide(content_swarm_test_dht, provider_key);
   auto find_providers = source.async_find_providers(content_swarm_test_dht, provider_key);
   auto put_value =
       source.async_put_value(content_swarm_value_test_dht, dht::record{.key_value = value_key, .value = {'v'}});
   auto get_value = source.async_get_value(content_swarm_value_test_dht, value_key);
   auto owner = std::move(source);

   const auto peer_result = forge::asio::blocking::run(runtime, std::move(find_peer));
   BOOST_TEST(!peer_result.complete);
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, std::move(provide))), exceptions::peer_not_found);
   BOOST_TEST(forge::asio::blocking::run(runtime, std::move(find_providers)).empty());
   const auto put_result = forge::asio::blocking::run(runtime, std::move(put_value));
   BOOST_TEST(!put_result.quorum_reached);
   const auto get_result = forge::asio::blocking::run(runtime, std::move(get_value));
   BOOST_REQUIRE(get_result.selected.has_value());
   BOOST_TEST(get_result.selected->value == std::vector<std::uint8_t>{'v'});

   forge::asio::blocking::run(runtime, owner.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_async_stop_owns_node_impl_before_first_resume) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pending = std::optional<boost::asio::awaitable<void>>{};
   {
      auto source = node{runtime, options_for(peer(218))};
      pending.emplace(source.async_stop());
   }

   forge::asio::blocking::run(runtime, std::move(*pending));
}

BOOST_AUTO_TEST_CASE(p2p_async_stop_completes_teardown_after_inherited_cancellation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   persistence->block_close = true;
   auto server = node{runtime, options_for(peer(219))};
   auto client_options = options_for(peer(220));
   client_options.peer_state.persistence = persistence;
   auto client = node{runtime, std::move(client_options)};
   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(server_endpoint, node::connect_options{.expected_peer = server.local_peer()})));
   BOOST_REQUIRE(client.diagnostics().metrics.active_sessions == 1U);

   auto cancellation = boost::asio::cancellation_signal{};
   auto stopped =
       boost::asio::co_spawn(runtime.context(), client.async_stop(),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   BOOST_REQUIRE(persistence->wait_until_close_blocked());

   cancellation.emit(boost::asio::cancellation_type::terminal);
   BOOST_CHECK(stopped.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   const auto stopping = client.diagnostics();
   BOOST_TEST(stopping.sessions.empty());
   BOOST_TEST(stopping.metrics.active_sessions == 0U);
   BOOST_TEST(static_cast<int>(client.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopping));
   BOOST_TEST(persistence->close_attempts == 1U);

   persistence->release_close();
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   stopped.get();

   const auto stopped_snapshot = client.diagnostics();
   BOOST_TEST(stopped_snapshot.persistence.closed);
   BOOST_TEST(client.metrics().stopped);
   BOOST_TEST(static_cast<int>(client.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));
   BOOST_TEST(persistence->close_attempts == 1U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_routing_refresh_task_retains_node_impl_until_stop_is_observed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto release_runtime = block_runtime(runtime, "DHT refresh ownership barrier");
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto weak_persistence = std::weak_ptr<dht::record_store::persistence>{persistence};
   {
      auto options = dht_options_for(peer(217), custom_test_dht_profile());
      options.dht_record_persistence.emplace(content_swarm_test_dht, persistence);
      auto source = node{runtime, std::move(options)};
      persistence.reset();
   }

   BOOST_TEST(!weak_persistence.expired());
   release_runtime->set_value();
   for (auto attempt = 0U; attempt < 100U && !weak_persistence.expired(); ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "DHT refresh ownership release");
   }
   BOOST_TEST(weak_persistence.expired());
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_maintenance_prunes_each_dht_record_store_profile) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto provider_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto value_persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto options = dht_options_for(peer(216), custom_test_dht_profile());
   options.dht_profiles.push_back(custom_test_value_dht_profile());
   options.dht_record_persistence.emplace(content_swarm_test_dht, provider_persistence);
   options.dht_record_persistence.emplace(content_swarm_value_test_dht, value_persistence);
   options.lifecycle.maintenance_interval = std::chrono::milliseconds{10};
   auto source = node{runtime, std::move(options)};

   static_cast<void>(forge::asio::blocking::run(runtime, source.async_start()));
   for (auto attempt = 0U;
        attempt < 100U && (provider_persistence->prune_attempts.load(std::memory_order_relaxed) == 0U ||
                           value_persistence->prune_attempts.load(std::memory_order_relaxed) == 0U);
        ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "DHT lifecycle prune");
   }
   BOOST_TEST(provider_persistence->prune_attempts.load(std::memory_order_relaxed) > 0U);
   BOOST_TEST(value_persistence->prune_attempts.load(std::memory_order_relaxed) > 0U);
   forge::asio::blocking::run(runtime, source.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_failed_initial_publication_retains_cleanup_for_stop_retry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto identity = make_test_certificate_identity("dht-initial-cleanup-retry");
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto options = dht_options_for(identity, custom_test_dht_profile());
   options.dht_record_persistence.emplace(content_swarm_test_dht, persistence);
   auto advertised = make_dns_tcp_endpoint(4'194, "dht-initial-cleanup-retry.example.com");
   advertised.peer = identity.peer;
   options.advertised_endpoints.push_back(std::move(advertised));
   auto provider = node{runtime, std::move(options)};
   forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());
   persistence->reject_next_provider_removal.store(true, std::memory_order_relaxed);

   const auto key = make_dht_key(std::vector<std::uint8_t>{'i', 'n', 'i', 't', 'i', 'a', 'l'});
   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, provider.async_provide(content_swarm_test_dht, key))),
                     std::runtime_error);
   BOOST_TEST(persistence->provider_upsert_attempts.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);

   forge::asio::blocking::run(runtime, provider.async_stop());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(persistence->close_attempts.load(std::memory_order_relaxed) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_dht_new_admission_retries_failed_destructor_cleanup) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("dht-readmission-server");
   const auto provider_identity = make_test_certificate_identity("dht-readmission-provider");
   auto server = node{runtime, dht_options_for(server_identity, custom_test_dht_profile(dht::mode::server))};
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto provider_options = dht_options_for(provider_identity, custom_test_dht_profile());
   provider_options.dht_record_persistence.emplace(content_swarm_test_dht, persistence);
   auto advertised = make_dns_tcp_endpoint(4'196, "dht-readmission.example.com");
   advertised.peer = provider_identity.peer;
   provider_options.advertised_endpoints.push_back(advertised);
   auto provider = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(listen(provider, runtime));
   verify_dht_server(runtime, provider, server, server_endpoint, content_swarm_test_dht);
   forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());

   const auto key = make_dht_key(std::vector<std::uint8_t>{'r', 'e', 'a', 'd', 'm', 'i', 't'});
   {
      auto registration = forge::asio::blocking::run(runtime, provider.async_provide(content_swarm_test_dht, key));
      BOOST_REQUIRE(registration.active());
      persistence->reject_next_provider_removal.store(true, std::memory_order_relaxed);
   }
   for (auto attempt = 0U; attempt < 40U && persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 0U;
        ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{25}, "DHT failed destructor cleanup");
   }
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);

   auto replacement = forge::asio::blocking::run(runtime, provider.async_provide(content_swarm_test_dht, key));
   BOOST_REQUIRE(replacement.active());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 2U);
   forge::asio::blocking::run(runtime, replacement.async_withdraw());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 3U);
   forge::asio::blocking::run(runtime, provider.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_provider_removal_failure_allows_stop_retry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("dht-stop-retry-server");
   const auto provider_identity = make_test_certificate_identity("dht-stop-retry-provider");
   auto server = node{runtime, dht_options_for(server_identity, custom_test_dht_profile(dht::mode::server))};
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto provider_options = dht_options_for(provider_identity, custom_test_dht_profile());
   provider_options.dht_record_persistence.emplace(content_swarm_test_dht, persistence);
   auto provider_endpoint = make_dns_tcp_endpoint(4'193, "dht-stop-retry.example.com");
   provider_endpoint.peer = provider_identity.peer;
   provider_options.advertised_endpoints.push_back(provider_endpoint);
   auto provider = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(listen(provider, runtime));
   verify_dht_server(runtime, provider, server, server_endpoint, content_swarm_test_dht);
   forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());

   const auto key = make_dht_key(std::vector<std::uint8_t>{'s', 't', 'o', 'p', '-', 'r', 'e', 't', 'r', 'y'});
   auto registration = forge::asio::blocking::run(runtime, provider.async_provide(content_swarm_test_dht, key));
   BOOST_REQUIRE(registration.active());
   persistence->reject_next_provider_removal.store(true, std::memory_order_relaxed);

   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, provider.async_stop())), std::runtime_error);
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(persistence->close_attempts.load(std::memory_order_relaxed) == 0U);
   BOOST_TEST(!registration.active());

   forge::asio::blocking::run(runtime, provider.async_stop());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(persistence->close_attempts.load(std::memory_order_relaxed) == 1U);
   forge::asio::blocking::run(runtime, registration.async_withdraw());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 2U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_provider_removal_failure_allows_withdraw_retry) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = make_test_certificate_identity("dht-withdraw-retry-server");
   const auto provider_identity = make_test_certificate_identity("dht-withdraw-retry-provider");
   auto server = node{runtime, dht_options_for(server_identity, custom_test_dht_profile(dht::mode::server))};
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto provider_options = dht_options_for(provider_identity, custom_test_dht_profile());
   provider_options.dht_record_persistence.emplace(content_swarm_test_dht, persistence);
   auto provider_endpoint = make_dns_tcp_endpoint(4'195, "dht-withdraw-retry.example.com");
   provider_endpoint.peer = provider_identity.peer;
   provider_options.advertised_endpoints.push_back(provider_endpoint);
   auto provider = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(listen(provider, runtime));
   verify_dht_server(runtime, provider, server, server_endpoint, content_swarm_test_dht);
   forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());

   const auto key = make_dht_key(std::vector<std::uint8_t>{'w', 'i', 't', 'h', 'd', 'r', 'a', 'w'});
   auto registration = forge::asio::blocking::run(runtime, provider.async_provide(content_swarm_test_dht, key));
   BOOST_REQUIRE(registration.active());
   persistence->reject_next_provider_removal.store(true, std::memory_order_relaxed);

   BOOST_CHECK_THROW((forge::asio::blocking::run(runtime, registration.async_withdraw())), std::runtime_error);
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(!registration.active());

   forge::asio::blocking::run(runtime, registration.async_withdraw());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 2U);
   forge::asio::blocking::run(runtime, provider.async_stop());
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 2U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_dht_withdraw_retry_joins_concurrent_shutdown_and_clears_reported_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 3}};
   const auto server_identity = make_test_certificate_identity("dht-withdraw-stop-join-server");
   const auto provider_identity = make_test_certificate_identity("dht-withdraw-stop-join-provider");
   auto server = node{runtime, dht_options_for(server_identity, custom_test_dht_profile(dht::mode::server))};
   auto persistence = std::make_shared<tracking_dht_record_store_persistence>();
   auto provider_options = dht_options_for(provider_identity, custom_test_dht_profile());
   provider_options.dht_record_persistence.emplace(content_swarm_test_dht, persistence);
   auto provider_endpoint = make_dns_tcp_endpoint(4'197, "dht-withdraw-stop-join.example.com");
   provider_endpoint.peer = provider_identity.peer;
   provider_options.advertised_endpoints.push_back(provider_endpoint);
   auto provider = node{runtime, std::move(provider_options)};
   const auto server_endpoint = listen(server, runtime);
   static_cast<void>(listen(provider, runtime));
   verify_dht_server(runtime, provider, server, server_endpoint, content_swarm_test_dht);
   forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());

   const auto key = make_dht_key(std::vector<std::uint8_t>{'j', 'o', 'i', 'n', '-', 'r', 'e', 't', 'r', 'y'});
   auto registration = forge::asio::blocking::run(runtime, provider.async_provide(content_swarm_test_dht, key));
   persistence->reject_next_provider_removal.store(true, std::memory_order_relaxed);
   provider.stop();
   for (auto attempt = 0U; attempt < 100U && registration.active(); ++attempt) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "DHT failed stop removal");
   }
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(!registration.active());

   persistence->block_next_provider_removal.store(true, std::memory_order_relaxed);
   auto withdrawal = boost::asio::co_spawn(runtime.context(), registration.async_withdraw(), boost::asio::use_future);
   BOOST_REQUIRE(persistence->wait_until_provider_removal_blocked());
   auto shutdown = boost::asio::co_spawn(runtime.context(), provider.async_stop(), boost::asio::use_future);
   persistence->release_provider_removal();

   BOOST_REQUIRE(withdrawal.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   withdrawal.get();
   BOOST_REQUIRE(shutdown.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   shutdown.get();
   BOOST_TEST(persistence->provider_remove_attempts.load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(persistence->close_attempts.load(std::memory_order_relaxed) == 1U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_production_options_reject_missing_mtls_identity) {
   try {
      validate(node::options{});
      BOOST_FAIL("expected missing mTLS identity rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::invalid_options));
   }
}

BOOST_AUTO_TEST_CASE(p2p_production_options_require_peer_state_persistence) {
   try {
      validate(node::options{
          .certificate_pem = std::string{test_certificate()},
          .private_key_pem = std::string{test_private_key()},
      });
      BOOST_FAIL("expected missing persistent peer store rejection");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(static_cast<int>(forge::net::p2p::exceptions::code_of(error).value()) ==
                 static_cast<int>(exceptions::code::invalid_options));
   }
}

BOOST_AUTO_TEST_CASE(p2p_node_options_allow_identify_decode_memory_reservation) {
   auto options = options_for(peer(249));
   options.limits.resources.stream.max_memory =
       options.identify.max_total_message_size + options.identify.max_message_size;

   BOOST_CHECK_NO_THROW(validate(options));
}

BOOST_AUTO_TEST_CASE(p2p_identify_decode_memory_rejection_releases_partial_reservation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server_options = options_for(peer(249));
   auto client_options = options_for(peer(250));
   server_options.limits.resources.service.max_memory = server_options.identify.max_total_message_size;
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};

   forge::asio::blocking::run(runtime, server.async_listen(make_quic_endpoint(0)));
   const auto endpoint = require_endpoint_for(server.local_endpoints(), endpoint::protocol_kind::quic_v1);
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(endpoint, node::connect_options{.expected_peer = server.local_peer()})));

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   auto server_session = diagnostics::session{};
   while (std::chrono::steady_clock::now() < deadline) {
      const auto sessions = server.diagnostics().sessions;
      if (!sessions.empty() && sessions.front().identify_state == identify::state::failed) {
         server_session = sessions.front();
         break;
      }
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "Identify decode memory rejection");
   }

   BOOST_TEST(static_cast<int>(server_session.identify_state) == static_cast<int>(identify::state::failed));
   BOOST_TEST(server_session.capabilities.bits == 0U);
   BOOST_TEST(!server.peers().find(client.local_peer()).has_value());
   const auto release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (server.diagnostics().resources.services.memory != 0U &&
          std::chrono::steady_clock::now() < release_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "Identify decode memory release");
   }
   const auto resources = server.diagnostics().resources;
   BOOST_TEST(resources.streams.memory == 0U);
   BOOST_TEST(resources.services.memory == 0U);
   BOOST_TEST(resources.denied_memory >= 1U);

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_options_reject_zero_global_dial_limit) {
   auto options = options_for(peer(250));
   options.limits.resources.max_dial_attempts = 0;

   BOOST_CHECK_THROW(validate(options), exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_node_options_fail_fast_on_invalid_address_resolution_and_dial_policies) {
   {
      auto options = options_for(peer(250));
      options.dns_resolution.bounds.max_dns_lookups = 0;
      BOOST_CHECK_THROW(validate(options), exceptions::invalid_options);
   }
   {
      auto options = options_for(peer(250));
      options.dns_resolver.max_in_flight = 0;
      BOOST_CHECK_THROW(validate(options), exceptions::invalid_options);
   }
   {
      auto options = options_for(peer(250));
      options.direct_dial.max_concurrent_attempts = 0;
      BOOST_CHECK_THROW(validate(options), exceptions::invalid_options);
   }
   {
      auto options = options_for(peer(250));
      options.direct_dial.max_concurrent_attempts = 5;
      BOOST_CHECK_THROW(validate(options), exceptions::invalid_options);
   }
   {
      auto options = options_for(peer(250));
      options.direct_dial.ranker.public_delay = std::chrono::milliseconds::zero();
      BOOST_CHECK_THROW(validate(options), exceptions::invalid_options);
   }
   {
      auto options = options_for(peer(250));
      options.direct_dial.black_holes.window_size = 0;
      BOOST_CHECK_THROW(validate(options), exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(p2p_production_options_use_peer_state_persistence) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto persistence = std::make_shared<tracking_peer_store_persistence>();
   const auto identity = make_test_certificate_identity("production-peer-state");
   auto value = node{runtime, node::options{
                                  .certificate_pem = identity.certificate_pem,
                                  .private_key_pem = identity.private_key_pem,
                                  .explicit_peer_id = identity.peer,
                                  .peer_state = peer_store::options{.persistence = persistence},
                              }};
   value.peers().upsert(peer_store::record{
       .peer = peer(84),
       .protocol_version = "/forge/node-memory/1",
       .agent_version = "forge-node-memory/1",
       .protocols = std::vector<protocol_id>{builtins::identify_push},
   });
   forge::asio::blocking::run(runtime, value.async_stop());

   BOOST_TEST(persistence->applied_peer_upserts == 1U);
   BOOST_TEST(persistence->flush_attempts == 1U);
   BOOST_TEST(persistence->close_attempts == 1U);
}

} // namespace forge::net::p2p
