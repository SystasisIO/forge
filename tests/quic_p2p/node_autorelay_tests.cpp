module;

#include <forge/exceptions/macros.hpp>

#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.runtime;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.relay;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.yamux.exceptions;

#include "../../libraries/net/p2p/details/length_delimited.hxx"

namespace {
namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

template <typename T>
T bounded(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto future = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   BOOST_REQUIRE(future.wait_for(8s) == std::future_status::ready);
   return future.get();
}

boost::asio::awaitable<void> pause(std::chrono::milliseconds duration) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   timer.expires_after(duration);
   co_await timer.async_wait(boost::asio::use_awaitable);
}

template <typename Predicate>
bool eventually(forge::asio::runtime& runtime, Predicate predicate, std::chrono::milliseconds timeout = 4s) {
   const auto end = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < end) {
      if (predicate()) {
         return true;
      }
      bounded(runtime, pause(10ms));
   }
   return predicate();
}

p2p::node::options options_for(std::string name, bool service = false) {
   auto identity = forge::tests::p2p::make_identity_fixture(name);
   auto options = p2p::node::options{};
   options.certificate_pem = std::move(identity.certificate_pem);
   options.private_key_pem = std::move(identity.private_key_pem);
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   options.limits.topology.operating_mode = p2p::topology::mode::static_only;
   options.relay_policy.service_enabled = service;
   options.relay_policy.client_enabled = !service;
   options.relay_policy.auto_discovery_enabled = !service;
   options.relay_policy.target_reservations = 1;
   options.relay_policy.refresh_margin = 1s;
   options.relay_policy.candidate_backoff = 200ms;
   options.limits.relay.reservation_ttl = 3s;
   options.capabilities.bits = p2p::capabilities::direct_quic | p2p::capabilities::relay_reservation;
   if (service) {
      options.capabilities.bits |= p2p::capabilities::relay;
   }
   options.lifecycle.listen.push_back(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0"));
   return options;
}

std::vector<p2p::endpoint> circuits(const p2p::node& value) {
   auto out = std::vector<p2p::endpoint>{};
   for (const auto& address : value.local_endpoints()) {
      if (address.relayed) {
         out.push_back(address);
      }
   }
   return out;
}

bool has_circuit(const p2p::node& value, const p2p::peer_id& relay) {
   return std::ranges::any_of(circuits(value), [&](const auto& address) {
      return address.peer == relay && address.relayed->target == value.local_peer();
   });
}

struct handler_observation {
   std::atomic_size_t entered{0};
   std::atomic_size_t decoded{0};
   std::atomic_size_t acknowledged{0};
   std::atomic_size_t completed{0};
};

struct complete_on_exit {
   std::shared_ptr<handler_observation> observation;
   ~complete_on_exit() { ++observation->completed; }
};

struct release_on_exit {
   std::shared_ptr<forge::asio::notification> notification;
   ~release_on_exit() { notification->notify(); }
};

void require_wire(bool condition, const char* message) {
   if (!condition) {
      FORGE_THROW_EXCEPTION(p2p::exceptions::protocol_error, message);
   }
}

void require_direct_sender(const p2p::node::incoming_protocol_stream& incoming,
                           const p2p::protocol_id& protocol, const p2p::peer_id& peer) {
   require_wire(incoming.protocol == protocol, "custom relay handler negotiated the wrong protocol");
   require_wire(incoming.session.remote_peer == peer, "custom relay handler received the wrong peer");
   require_wire(incoming.session.path == p2p::path::kind::direct && !incoming.session.relay_peer,
                "custom relay handler requires a direct session");
   require_wire(incoming.stream.authentication() == p2p::peer_authentication::libp2p_tls,
                "custom relay handler requires authenticated TCP/TLS");
}

boost::asio::awaitable<void> read_reserve_request(p2p::node::incoming_protocol_stream& incoming,
                                                 handler_observation& observation,
                                                 p2p::peer_id client) {
   ++observation.entered;
   require_direct_sender(incoming, p2p::builtins::relay_hop, client);
   auto buffered = std::vector<std::uint8_t>{};
   const auto request = p2p::relay::codec::decode_hop(
       co_await p2p::async_read_length_delimited(incoming.stream, buffered, 4096));
   require_wire(request.kind == p2p::relay::hop_message::message_kind::reserve && !request.target &&
                    !request.reservation_value && !request.limit_value && buffered.empty(),
                "custom HOP handler requires exactly one complete RESERVE request");
   ++observation.decoded;
}

boost::asio::awaitable<std::vector<std::uint8_t>> read_stop_request(
    p2p::node::incoming_protocol_stream& incoming, handler_observation& observation,
    p2p::peer_id relay, std::vector<p2p::peer_id> sources) {
   ++observation.entered;
   require_direct_sender(incoming, p2p::builtins::relay_stop, relay);
   auto buffered = std::vector<std::uint8_t>{};
   const auto request = p2p::relay::codec::decode_stop(
       co_await p2p::async_read_length_delimited(incoming.stream, buffered, 4096));
   require_wire(request.kind == p2p::relay::stop_message::message_kind::connect && request.source &&
                    std::ranges::find(sources, request.source->id) != sources.end() && buffered.empty(),
                "custom STOP handler requires a complete CONNECT with the actual source peer");
   ++observation.decoded;
   co_return buffered;
}

boost::asio::awaitable<std::vector<std::uint8_t>> read_exact_bytes(
    p2p::stream& stream, std::vector<std::uint8_t>& buffered, std::size_t size) {
   require_wire(size <= 4096 && buffered.size() <= 4096, "test byte stream exceeds its bound");
   while (buffered.size() < size) {
      const auto next = co_await stream.async_read();
      require_wire(!next.empty(), "test byte stream ended before all expected bytes");
      require_wire(next.size() <= 4096 - buffered.size(), "test byte stream remainder exceeds its bound");
      buffered.insert(buffered.end(), next.begin(), next.end());
   }
   auto out = std::vector<std::uint8_t>{buffered.begin(), buffered.begin() + size};
   buffered.erase(buffered.begin(), buffered.begin() + size);
   co_return out;
}

boost::asio::awaitable<void> require_stream_end(p2p::stream& stream, std::vector<std::uint8_t>& buffered) {
   require_wire(buffered.empty(), "test byte stream contains duplicated or unexpected trailing bytes");
   try {
      require_wire((co_await stream.async_read()).empty(), "test byte stream contains bytes after the exact reply");
   } catch (const forge::exceptions::base& error) {
      // Native Yamux reports remote FIN as typed closed after draining inbound bytes.
      if (!forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::closed)) {
         throw;
      }
   }
}

boost::asio::awaitable<p2p::identify::document> wire_identify(p2p::node& observer, p2p::peer_id peer) {
   auto stream = co_await observer.async_open_protocol_stream(peer, p2p::builtins::identify);
   require_wire(stream.authentication() == p2p::peer_authentication::libp2p_tls,
                "Identify evidence requires the actual authenticated TCP/TLS session");
   auto buffered = std::vector<std::uint8_t>{};
   const auto frame = co_await p2p::async_read_length_delimited(stream, buffered, 8192);
   const auto decoded = p2p::protocol_negotiation::decode_frame(frame);
   require_wire(decoded.consumed == frame.size() && buffered.empty(), "Identify evidence has unexpected remainder");
   auto document = p2p::identify::decode(decoded.payload);
   require_wire(!document.public_key.empty() &&
                    p2p::make_peer_id(p2p::decode_public_key(document.public_key)) == peer,
                "Identify evidence public key does not match the authenticated peer");
   co_await stream.async_close();
   co_return document;
}

void check_unique_protocols(const p2p::identify::document& document) {
   auto ids = std::set<std::string>{};
   for (const auto& protocol : document.protocols) {
      BOOST_TEST(ids.insert(protocol.value).second);
   }
   BOOST_TEST(ids.size() == document.protocols.size());
}

boost::asio::awaitable<p2p::relay::status> stop_exchange(p2p::node& relay, const p2p::node& client) {
   auto stream = co_await relay.async_open_protocol_stream(client.local_peer(), p2p::builtins::relay_stop);
   co_await stream.async_write(p2p::relay::codec::encode_stop(p2p::relay::stop_message{
       .kind = p2p::relay::stop_message::message_kind::connect,
       .source = p2p::relay::peer{.id = relay.local_peer()},
   }));
   auto buffered = std::vector<std::uint8_t>{};
   const auto response = p2p::relay::codec::decode_stop(
       co_await p2p::async_read_length_delimited(stream, buffered, 4096));
   require_wire(response.kind == p2p::relay::stop_message::message_kind::status && buffered.empty(),
                "STOP exchange requires a complete STATUS response");
   co_await stream.async_close();
   co_return response.status;
}

boost::asio::awaitable<std::optional<p2p::relay::status>> unsolicited_stop(
    p2p::node& relay, const p2p::node& client) {
   try {
      co_return co_await stop_exchange(relay, client);
   } catch (const forge::exceptions::base&) {
      co_return std::nullopt;
   }
}

boost::asio::awaitable<std::pair<p2p::relay::status, p2p::stream>> hop_connect(
    p2p::node& source, const p2p::node& service, const p2p::node& destination) {
   auto stream = co_await source.async_open_protocol_stream(service.local_peer(), p2p::builtins::relay_hop);
   co_await stream.async_write(p2p::relay::codec::encode_hop(p2p::relay::hop_message{
       .kind = p2p::relay::hop_message::message_kind::connect,
       .target = p2p::relay::peer{.id = destination.local_peer()},
   }));
   auto buffered = std::vector<std::uint8_t>{};
   const auto response = p2p::relay::codec::decode_hop(
       co_await p2p::async_read_length_delimited(stream, buffered, 4096));
   require_wire(response.kind == p2p::relay::hop_message::message_kind::status,
                "HOP exchange requires a complete STATUS response");
   co_return std::pair{response.status, p2p::detail::stream_access::with_buffer(
       std::move(stream), std::move(buffered))};
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_node_autorelay)

BOOST_AUTO_TEST_CASE(start_acquires_and_advertises_without_manual_refresh) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto service = p2p::node{runtime, options_for("autorelay-automatic-service", true)};
   auto client = p2p::node{runtime, options_for("autorelay-automatic-client")};
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));

   const auto acquired = eventually(runtime, [&] { return has_circuit(client, service.local_peer()); });
   BOOST_TEST(acquired);
   BOOST_TEST(service.metrics().relay_reservations >= 1U);
   BOOST_TEST(service.metrics().active_relay_reservations == 1U);
   BOOST_TEST(circuits(client).size() == 1U);

   bounded(runtime, client.async_stop());
   BOOST_TEST(circuits(client).empty());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(renewal_tracks_lease_expiry_not_topology_refresh_interval) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto service = p2p::node{runtime, options_for("autorelay-renew-service", true)};
   auto client = p2p::node{runtime, options_for("autorelay-renew-client")};
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   const auto acquired = eventually(runtime, [&] { return has_circuit(client, service.local_peer()); });
   BOOST_TEST(acquired);
   const auto first = service.metrics().relay_reservations;
   const auto renewed = eventually(runtime, [&] { return service.metrics().relay_reservations > first; }, 5s);
   BOOST_TEST(renewed);
   BOOST_TEST(has_circuit(client, service.local_peer()));
   BOOST_TEST(service.metrics().active_relay_reservations == 1U);

   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(relay_loss_removes_address_and_replaces_lease) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto first = p2p::node{runtime, options_for("autorelay-loss-first", true)};
   auto replacement = p2p::node{runtime, options_for("autorelay-loss-replacement", true)};
   auto client = p2p::node{runtime, options_for("autorelay-loss-client")};
   static_cast<void>(bounded(runtime, first.async_start()));
   static_cast<void>(bounded(runtime, replacement.async_start()));
   static_cast<void>(bounded(runtime, client.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(first.local_endpoints().front())));
   BOOST_TEST(eventually(runtime, [&] { return has_circuit(client, first.local_peer()); }));
   static_cast<void>(bounded(runtime, client.async_connect(replacement.local_endpoints().front())));
   bounded(runtime, first.async_stop());

   const auto replaced = eventually(runtime, [&] {
      return !has_circuit(client, first.local_peer()) && has_circuit(client, replacement.local_peer());
   });
   BOOST_TEST(replaced);
   BOOST_TEST(circuits(client).size() == 1U);
   bounded(runtime, client.async_stop());
   bounded(runtime, replacement.async_stop());
}

BOOST_AUTO_TEST_CASE(unidentified_cache_claim_does_not_create_circuit_address) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service_options = options_for("autorelay-cache-non-service", true);
   service_options.relay_policy.service_enabled = false;
   auto service = p2p::node{runtime, std::move(service_options)};
   auto client = p2p::node{runtime, options_for("autorelay-cache-client")};
   static_cast<void>(bounded(runtime, service.async_start()));
   client.peers().learn_endpoint(service.local_peer(), service.local_endpoints().front(),
                                p2p::capability_set{.bits = p2p::capabilities::relay |
                                                           p2p::capabilities::relay_reservation});
   static_cast<void>(bounded(runtime, client.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   bounded(runtime, pause(300ms));
   BOOST_TEST(circuits(client).empty());
   BOOST_TEST(service.metrics().relay_reservations == 0U);

   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(client_disabled_does_not_reserve_or_advertise) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service = p2p::node{runtime, options_for("autorelay-disabled-service", true)};
   auto client_options = options_for("autorelay-disabled-client");
   client_options.relay_policy.client_enabled = false;
   client_options.relay_policy.auto_discovery_enabled = false;
   auto client = p2p::node{runtime, std::move(client_options)};
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   bounded(runtime, pause(300ms));
   BOOST_TEST(circuits(client).empty());
   BOOST_TEST(service.metrics().relay_reservations == 0U);

   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(expired_and_unrepresentable_reservations_are_rejected_before_state_mutation) {
   for (const auto expires : {std::uint64_t{1}, (std::numeric_limits<std::uint64_t>::max)()}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto service = p2p::node{runtime, options_for("autorelay-invalid-expiry-service", true)};
      auto client_options = options_for("autorelay-invalid-expiry-client");
      client_options.relay_policy.auto_discovery_enabled = false;
      auto client = p2p::node{runtime, std::move(client_options)};
      static_cast<void>(bounded(runtime, service.async_start()));
      const auto address = service.local_endpoints().front();
      auto observation = std::make_shared<handler_observation>();
      service.register_protocol_handler(p2p::builtins::relay_hop,
          [expires, address, observation, peer = client.local_peer()](p2p::node::incoming_protocol_stream incoming)
              -> boost::asio::awaitable<void> {
             co_await read_reserve_request(incoming, *observation, peer);
             co_await incoming.stream.async_write(p2p::relay::codec::encode_hop(p2p::relay::hop_message{
                 .kind = p2p::relay::hop_message::message_kind::status,
                 .reservation_value = p2p::relay::reservation{.expires_at = expires,
                                                             .relay_endpoints = {address}},
                 .status = p2p::relay::status::ok,
             }));
             co_await incoming.stream.async_close();
          });
      static_cast<void>(bounded(runtime, client.async_connect(address)));
      BOOST_CHECK_EXCEPTION(
          bounded(runtime, client.async_reserve_relay(service.local_peer())), forge::exceptions::base,
          [](const forge::exceptions::base& error) {
             return p2p::exceptions::is(error, p2p::exceptions::code::protocol_error);
          });
      BOOST_TEST(observation->entered.load() == 1U);
      BOOST_TEST(observation->decoded.load() == 1U);
      BOOST_TEST(service.metrics().relay_reservations == 0U);
      const auto record = client.peers().find(service.local_peer());
      BOOST_REQUIRE(record);
      BOOST_TEST(record->relay_reservations.empty());
      BOOST_TEST(circuits(client).empty());
      bounded(runtime, client.async_stop());
      bounded(runtime, service.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(relay_service_is_opt_in_even_with_capability_claims) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service_options = options_for("autorelay-role-service", true);
   service_options.relay_policy = p2p::relay::policy{};
   service_options.relay_policy.client_enabled = false;
   service_options.relay_policy.auto_discovery_enabled = false;
   auto service = p2p::node{runtime, std::move(service_options)};
   auto client_options = options_for("autorelay-role-client");
   client_options.relay_policy.auto_discovery_enabled = false;
   auto client = p2p::node{runtime, std::move(client_options)};
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   BOOST_CHECK_THROW(bounded(runtime, client.async_reserve_relay(service.local_peer())), forge::exceptions::base);
   BOOST_TEST(service.metrics().active_relay_reservations == 0U);
   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(cancel_pending_reserve_prevents_late_grant_and_preserves_direct_session) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto service = p2p::node{runtime, options_for("autorelay-cancel-service", true)};
   auto client_options = options_for("autorelay-cancel-client");
   client_options.relay_policy.auto_discovery_enabled = false;
   auto client = p2p::node{runtime, std::move(client_options)};
   static_cast<void>(bounded(runtime, service.async_start()));
   const auto address = service.local_endpoints().front();
   auto observation = std::make_shared<handler_observation>();
   auto release = std::make_shared<forge::asio::notification>();
   auto guard = release_on_exit{release};
   const auto observed = release->epoch();
   service.register_protocol_handler(p2p::builtins::relay_hop,
       [observation, release, observed, address, peer = client.local_peer()](p2p::node::incoming_protocol_stream incoming)
           -> boost::asio::awaitable<void> {
          co_await read_reserve_request(incoming, *observation, peer);
          static_cast<void>(co_await release->async_wait(observed));
          auto completion = complete_on_exit{observation};
          const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch() + 30s);
          co_await incoming.stream.async_write(p2p::relay::codec::encode_hop(p2p::relay::hop_message{
              .kind = p2p::relay::hop_message::message_kind::status,
              .reservation_value = p2p::relay::reservation{
                  .expires_at = static_cast<std::uint64_t>(expires.count()), .relay_endpoints = {address}},
              .status = p2p::relay::status::ok,
          }));
          co_await incoming.stream.async_close();
       });
   static_cast<void>(bounded(runtime, client.async_connect(address)));
   const auto sessions = client.metrics().active_sessions;
   auto pending = boost::asio::co_spawn(runtime.context(), client.async_reserve_relay(service.local_peer()),
                                       boost::asio::use_future);
   BOOST_REQUIRE(eventually(runtime, [&] { return observation->decoded.load() == 1U; }));
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_CHECK(pending.wait_for(0ms) == std::future_status::timeout);
   bounded(runtime, client.async_cancel_relay(service.local_peer()));
   const auto canceled_promptly = pending.wait_for(300ms) == std::future_status::ready;
   release->notify();
   BOOST_TEST(canceled_promptly);
   BOOST_REQUIRE(pending.wait_for(5s) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(pending.get()), forge::exceptions::base,
       [](const forge::exceptions::base& error) {
          return p2p::exceptions::is(error, p2p::exceptions::code::canceled);
       });
   BOOST_REQUIRE(eventually(runtime, [&] { return observation->completed.load() == 1U; }));
   const auto record = client.peers().find(service.local_peer());
   BOOST_REQUIRE(record);
   BOOST_TEST(record->relay_reservations.empty());
   BOOST_TEST(circuits(client).empty());
   BOOST_TEST(client.metrics().active_sessions == sessions);
   const auto opened = client.metrics().sessions_opened;
   static_cast<void>(bounded(runtime, client.async_ping(service.local_peer())));
   BOOST_TEST(client.metrics().sessions_opened == opened);
   BOOST_TEST(client.metrics().active_sessions == sessions);
   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(voucherless_peerless_addresses_and_unlimited_wire_limits_keep_local_caps) {
   for (const auto explicit_zero : {false, true}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto service = p2p::node{runtime, options_for("autorelay-unlimited-service", true)};
      auto client_options = options_for("autorelay-unlimited-client");
      client_options.relay_policy.auto_discovery_enabled = false;
      client_options.limits.relay.max_relay_bytes = 8192;
      auto client = p2p::node{runtime, std::move(client_options)};
      static_cast<void>(bounded(runtime, service.async_start()));
      auto address = service.local_endpoints().front();
      const auto connect_address = address;
      address.peer.reset();
      auto observation = std::make_shared<handler_observation>();
      service.register_protocol_handler(p2p::builtins::relay_hop,
          [explicit_zero, address, observation, peer = client.local_peer()](p2p::node::incoming_protocol_stream incoming)
              -> boost::asio::awaitable<void> {
             co_await read_reserve_request(incoming, *observation, peer);
             const auto expires = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::system_clock::now().time_since_epoch() + 30s);
             co_await incoming.stream.async_write(p2p::relay::codec::encode_hop(p2p::relay::hop_message{
                 .kind = p2p::relay::hop_message::message_kind::status,
                 .reservation_value = p2p::relay::reservation{
                     .expires_at = static_cast<std::uint64_t>(expires.count()), .relay_endpoints = {address}},
                 .limit_value = explicit_zero ? std::optional{p2p::relay::limit{}} : std::nullopt,
                 .status = p2p::relay::status::ok,
             }));
             co_await incoming.stream.async_close();
          });
      static_cast<void>(bounded(runtime, client.async_connect(connect_address)));
      const auto info = bounded(runtime, client.async_reserve_relay(service.local_peer()));
      BOOST_TEST(observation->entered.load() == 1U);
      BOOST_TEST(observation->decoded.load() == 1U);
      BOOST_TEST(service.metrics().relay_reservations == 0U);
      BOOST_TEST(info.remote_limit.has_value() == explicit_zero);
      if (info.remote_limit) {
         BOOST_TEST(info.remote_limit->duration.count() == 0);
         BOOST_TEST(info.remote_limit->data == 0U);
      }
      BOOST_TEST(info.max_bytes == 8192U);
      BOOST_REQUIRE_EQUAL(info.relay_endpoints.size(), 1U);
      BOOST_CHECK(info.relay_endpoints.front().peer == service.local_peer());
      BOOST_TEST(has_circuit(client, service.local_peer()));
      bounded(runtime, client.async_stop());
      bounded(runtime, service.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(service_ip_reservation_cap_releases_after_owner_disconnect) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto service_options = options_for("autorelay-ip-cap-service", true);
   service_options.limits.relay.max_reservations_per_ip = 1;
   auto service = p2p::node{runtime, std::move(service_options)};
   auto first_options = options_for("autorelay-ip-cap-first");
   first_options.relay_policy.auto_discovery_enabled = false;
   auto second_options = options_for("autorelay-ip-cap-second");
   second_options.relay_policy.auto_discovery_enabled = false;
   auto first = p2p::node{runtime, std::move(first_options)};
   auto second = p2p::node{runtime, std::move(second_options)};
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, first.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, second.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, first.async_reserve_relay(service.local_peer())));
   BOOST_CHECK_EXCEPTION(
       bounded(runtime, second.async_reserve_relay(service.local_peer())), forge::exceptions::base,
       [](const forge::exceptions::base& error) {
          return p2p::exceptions::is(error, p2p::exceptions::code::relay_rejected);
       });
   BOOST_TEST(service.metrics().active_relay_reservations == 1U);
   BOOST_TEST(circuits(second).empty());
   bounded(runtime, first.async_stop());
   BOOST_REQUIRE(eventually(runtime, [&] { return service.metrics().active_relay_reservations == 0U; }));
   static_cast<void>(bounded(runtime, second.async_reserve_relay(service.local_peer())));
   BOOST_TEST(service.metrics().active_relay_reservations == 1U);
   bounded(runtime, second.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(service_rate_denial_does_not_replace_existing_grant) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service_options = options_for("autorelay-rate-service", true);
   service_options.limits.relay.max_service_requests_per_peer = 1;
   service_options.limits.relay.service_request_window = 1s;
   auto service = p2p::node{runtime, std::move(service_options)};
   auto client_options = options_for("autorelay-rate-client");
   client_options.relay_policy.auto_discovery_enabled = false;
   auto client = p2p::node{runtime, std::move(client_options)};
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, client.async_reserve_relay(service.local_peer())));
   BOOST_CHECK_EXCEPTION(
       bounded(runtime, client.async_reserve_relay(service.local_peer())), forge::exceptions::base,
       [](const forge::exceptions::base& error) {
          return p2p::exceptions::is(error, p2p::exceptions::code::relay_rejected);
       });
   BOOST_TEST(service.metrics().active_relay_reservations == 1U);
   BOOST_TEST(service.metrics().relay_reservations == 1U);
   BOOST_TEST(has_circuit(client, service.local_peer()));
   bounded(runtime, pause(1100ms));
   static_cast<void>(bounded(runtime, client.async_reserve_relay(service.local_peer())));
   BOOST_TEST(service.metrics().relay_reservations == 2U);
   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(refusing_live_candidate_does_not_starve_later_relay_with_one_candidate_slot) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto refusing = p2p::node{runtime, options_for("autorelay-starvation-refusing", true)};
   auto healthy = p2p::node{runtime, options_for("autorelay-starvation-healthy", true)};
   auto client_options = options_for("autorelay-starvation-client");
   client_options.relay_policy.max_candidates_per_refresh = 1;
   client_options.relay_policy.max_parallel_reservations = 1;
   auto client = p2p::node{runtime, std::move(client_options)};
   auto observation = std::make_shared<handler_observation>();
   refusing.register_protocol_handler(p2p::builtins::relay_hop,
       [observation, peer = client.local_peer()](p2p::node::incoming_protocol_stream incoming)
           -> boost::asio::awaitable<void> {
          co_await read_reserve_request(incoming, *observation, peer);
          co_await incoming.stream.async_write(p2p::relay::codec::encode_hop(p2p::relay::hop_message{
              .kind = p2p::relay::hop_message::message_kind::status,
              .status = p2p::relay::status::reservation_refused,
          }));
          co_await incoming.stream.async_close();
       });
   static_cast<void>(bounded(runtime, refusing.async_start()));
   static_cast<void>(bounded(runtime, healthy.async_start()));
   static_cast<void>(bounded(runtime, client.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(refusing.local_endpoints().front())));
   BOOST_REQUIRE(eventually(runtime, [&] { return observation->decoded.load() > 0; }));
   static_cast<void>(bounded(runtime, client.async_connect(healthy.local_endpoints().front())));
   BOOST_TEST(eventually(runtime, [&] { return has_circuit(client, healthy.local_peer()); }));
   BOOST_TEST(!has_circuit(client, refusing.local_peer()));
   BOOST_TEST(refusing.metrics().relay_reservations == 0U);
   BOOST_TEST(client.diagnostics().autorelay.candidates <= 1U);
   bounded(runtime, client.async_stop());
   BOOST_TEST(observation->entered.load() > 0U);
   BOOST_TEST(observation->decoded.load() > 0U);
   bounded(runtime, healthy.async_stop());
   bounded(runtime, refusing.async_stop());
}

BOOST_AUTO_TEST_CASE(stop_without_current_reservation_never_admits_a_relayed_session) {
   for (const auto state : {0, 1, 2, 3}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
      auto service = p2p::node{runtime, options_for("autorelay-stop-owner-service", true)};
      auto client_options = options_for("autorelay-stop-owner-client");
      client_options.relay_policy.auto_discovery_enabled = false;
      client_options.relay_policy.client_enabled = state != 0;
      auto client = p2p::node{runtime, std::move(client_options)};
      static_cast<void>(bounded(runtime, service.async_start()));
      static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
      if (state >= 2) {
         static_cast<void>(bounded(runtime, client.async_reserve_relay(service.local_peer())));
         if (state == 2) {
            bounded(runtime, client.async_cancel_relay(service.local_peer()));
         } else {
            bounded(runtime, pause(3100ms));
         }
      }
      const auto sessions = client.metrics().active_sessions;
      const auto status = bounded(runtime, unsolicited_stop(service, client));
      BOOST_CHECK(!status || *status != p2p::relay::status::ok);
      BOOST_TEST(client.metrics().active_sessions == sessions);
      bounded(runtime, client.async_stop());
      bounded(runtime, service.async_stop());
   }
}

BOOST_AUTO_TEST_CASE(stalled_stop_handshake_expires_and_releases_service_admission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto service_options = options_for("autorelay-stalled-stop-service", true);
   service_options.limits.relay.handshake_timeout = 100ms;
   service_options.limits.relay.max_active_relays = 1;
   auto service = p2p::node{runtime, std::move(service_options)};
   auto target_options = options_for("autorelay-stalled-stop-target");
   target_options.relay_policy.auto_discovery_enabled = false;
   auto source_options = options_for("autorelay-stalled-stop-source");
   source_options.relay_policy.auto_discovery_enabled = false;
   auto target = p2p::node{runtime, std::move(target_options)};
   auto source = p2p::node{runtime, std::move(source_options)};
   auto observation = std::make_shared<handler_observation>();
   auto release = std::make_shared<forge::asio::notification>();
   auto guard = release_on_exit{release};
   const auto observed = release->epoch();
   target.register_protocol_handler(p2p::builtins::relay_stop,
       [observation, release, observed, relay = service.local_peer(), peer = source.local_peer()]
       (p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          const auto buffered = co_await read_stop_request(incoming, *observation, relay, {peer});
          require_wire(buffered.empty(), "stalled STOP request has unexpected payload");
          // Hold only after authenticating and fully decoding CONNECT; never answer STOP.
          static_cast<void>(co_await release->async_wait(observed));
          co_await incoming.stream.async_close();
       });
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, target.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, target.async_reserve_relay(service.local_peer())));
   static_cast<void>(bounded(runtime, source.async_connect(service.local_endpoints().front())));
   const auto sessions = service.metrics().active_sessions;
   const auto started = std::chrono::steady_clock::now();
   auto result = bounded(runtime, hop_connect(source, service, target));
   release->notify();
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   BOOST_CHECK(result.first == p2p::relay::status::connection_failed);
   BOOST_CHECK(std::chrono::steady_clock::now() - started < 2s);
   bounded(runtime, result.second.async_close());
   BOOST_TEST(eventually(runtime, [&] { return service.metrics().active_relays == 0U; }));
   BOOST_TEST(service.metrics().active_sessions == sessions);
   bounded(runtime, source.async_stop());
   bounded(runtime, target.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(stop_status_coalesced_payload_survives_handshake_once) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto service_options = options_for("autorelay-coalesced-service", true);
   service_options.limits.relay.reservation_ttl = 10s;
   auto service = p2p::node{runtime, std::move(service_options)};
   auto destination_options = options_for("autorelay-coalesced-destination");
   destination_options.relay_policy.auto_discovery_enabled = false;
   auto destination = p2p::node{runtime, std::move(destination_options)};
   auto source_options = options_for("autorelay-coalesced-source");
   source_options.relay_policy.auto_discovery_enabled = false;
   auto source = p2p::node{runtime, std::move(source_options)};
   const auto payload = std::vector<std::uint8_t>{11, 22, 33, 44};
   const auto marker = std::vector<std::uint8_t>{55, 66};
   auto observation = std::make_shared<handler_observation>();
   destination.register_protocol_handler(p2p::builtins::relay_stop,
       [payload, marker, observation, relay = service.local_peer(), peer = source.local_peer()]
       (p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          auto buffered = co_await read_stop_request(incoming, *observation, relay, {peer});
          auto stream = std::move(incoming.stream);
          auto response = p2p::relay::codec::encode_stop(p2p::relay::stop_message{
              .kind = p2p::relay::stop_message::message_kind::status,
              .status = p2p::relay::status::ok,
          });
          response.insert(response.end(), payload.begin(), payload.end());
          co_await stream.async_write(std::move(response));
          const auto ack = co_await read_exact_bytes(stream, buffered, 1);
          require_wire(ack == std::vector<std::uint8_t>{1} && buffered.empty(),
                       "coalesced STOP payload requires exactly one source acknowledgement");
          ++observation->acknowledged;
          co_await stream.async_write(marker);
          co_await stream.async_close();
       });
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, destination.async_start()));
   static_cast<void>(bounded(runtime, source.async_start()));
   static_cast<void>(bounded(runtime, destination.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, destination.async_reserve_relay(service.local_peer())));
   static_cast<void>(bounded(runtime, source.async_connect(service.local_endpoints().front())));
   auto [status, stream] = bounded(runtime, hop_connect(source, service, destination));
   BOOST_CHECK(status == p2p::relay::status::ok);
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   auto buffered = std::vector<std::uint8_t>{};
   BOOST_CHECK(bounded(runtime, read_exact_bytes(stream, buffered, payload.size())) == payload);
   bounded(runtime, stream.async_write(std::vector<std::uint8_t>{1}));
   BOOST_CHECK(bounded(runtime, read_exact_bytes(stream, buffered, marker.size())) == marker);
   bounded(runtime, require_stream_end(stream, buffered));
   BOOST_TEST(observation->acknowledged.load() == 1U);
   bounded(runtime, stream.async_close());
   BOOST_TEST(eventually(runtime, [&] { return service.metrics().active_relays == 0; }));
   bounded(runtime, source.async_stop());
   bounded(runtime, destination.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(circuit_limit_counts_destination_across_independent_sources_and_renewal) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto service_options = options_for("autorelay-target-circuit-limit-service", true);
   service_options.limits.relay.max_circuits_per_peer = 1;
   service_options.limits.relay.max_streams_per_reservation = 8;
   service_options.limits.relay.reservation_ttl = 30s;
   auto service = p2p::node{runtime, std::move(service_options)};
   auto target_options = options_for("autorelay-target-circuit-limit-target");
   target_options.relay_policy.auto_discovery_enabled = false;
   auto first_options = options_for("autorelay-target-circuit-limit-first");
   first_options.relay_policy.auto_discovery_enabled = false;
   auto second_options = options_for("autorelay-target-circuit-limit-second");
   second_options.relay_policy.auto_discovery_enabled = false;
   auto target = p2p::node{runtime, std::move(target_options)};
   auto first = p2p::node{runtime, std::move(first_options)};
   auto second = p2p::node{runtime, std::move(second_options)};
   auto observation = std::make_shared<handler_observation>();
   target.register_protocol_handler(p2p::builtins::relay_stop,
       [observation, relay = service.local_peer(), first_peer = first.local_peer(), second_peer = second.local_peer()]
       (p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          const auto peer = observation->entered.load() == 0U ? first_peer : second_peer;
          auto buffered = co_await read_stop_request(incoming, *observation, relay, {peer});
          require_wire(buffered.empty(), "circuit-limit STOP request has unexpected payload");
          co_await incoming.stream.async_write(p2p::relay::codec::encode_stop(p2p::relay::stop_message{
              .kind = p2p::relay::stop_message::message_kind::status,
              .status = p2p::relay::status::ok,
          }));
          co_await require_stream_end(incoming.stream, buffered);
       });
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, target.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, target.async_reserve_relay(service.local_peer())));
   static_cast<void>(bounded(runtime, first.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, second.async_connect(service.local_endpoints().front())));
   auto accepted = bounded(runtime, hop_connect(first, service, target));
   BOOST_CHECK(accepted.first == p2p::relay::status::ok);
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   BOOST_REQUIRE(eventually(runtime, [&] { return service.metrics().active_relays == 1U; }));
   static_cast<void>(bounded(runtime, target.async_reserve_relay(service.local_peer())));
   auto denied = bounded(runtime, hop_connect(second, service, target));
   BOOST_CHECK(denied.first == p2p::relay::status::resource_limit_exceeded);
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   bounded(runtime, denied.second.async_close());
   bounded(runtime, accepted.second.async_close());
   BOOST_REQUIRE(eventually(runtime, [&] { return service.metrics().active_relays == 0U; }));
   auto retried = bounded(runtime, hop_connect(second, service, target));
   BOOST_CHECK(retried.first == p2p::relay::status::ok);
   BOOST_TEST(observation->entered.load() == 2U);
   BOOST_TEST(observation->decoded.load() == 2U);
   bounded(runtime, retried.second.async_close());
   bounded(runtime, second.async_stop());
   bounded(runtime, first.async_stop());
   bounded(runtime, target.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(unregister_hop_restores_native_reservation_service) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service = p2p::node{runtime, options_for("autorelay-hop-fallback-service", true)};
   auto client_options = options_for("autorelay-hop-fallback-client");
   client_options.relay_policy.auto_discovery_enabled = false;
   auto client = p2p::node{runtime, std::move(client_options)};
   auto observation = std::make_shared<handler_observation>();
   service.register_protocol_handler(p2p::builtins::relay_hop,
       [observation, peer = client.local_peer()](p2p::node::incoming_protocol_stream incoming)
           -> boost::asio::awaitable<void> {
          co_await read_reserve_request(incoming, *observation, peer);
          co_await incoming.stream.async_write(p2p::relay::codec::encode_hop(p2p::relay::hop_message{
              .kind = p2p::relay::hop_message::message_kind::status,
              .status = p2p::relay::status::reservation_refused,
          }));
          co_await incoming.stream.async_close();
       });
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   BOOST_CHECK_EXCEPTION(
       bounded(runtime, client.async_reserve_relay(service.local_peer())), forge::exceptions::base,
       [](const forge::exceptions::base& error) {
          return p2p::exceptions::is(error, p2p::exceptions::code::relay_rejected);
       });
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   BOOST_TEST(service.metrics().relay_reservations == 0U);
   BOOST_TEST(circuits(client).empty());
   BOOST_TEST(service.unregister_protocol_handler(p2p::builtins::relay_hop));
   BOOST_TEST(!service.unregister_protocol_handler(p2p::builtins::relay_hop));

   const auto reservation = bounded(runtime, client.async_reserve_relay(service.local_peer()));
   BOOST_CHECK(reservation.relay_peer == service.local_peer());
   BOOST_TEST(service.metrics().relay_reservations == 1U);
   BOOST_TEST(service.metrics().active_relay_reservations == 1U);
   BOOST_TEST(has_circuit(client, service.local_peer()));
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(unregister_stop_restores_native_owner_guard) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service = p2p::node{runtime, options_for("autorelay-stop-fallback-service", true)};
   auto client_options = options_for("autorelay-stop-fallback-client");
   client_options.relay_policy.auto_discovery_enabled = false;
   auto client = p2p::node{runtime, std::move(client_options)};
   auto observation = std::make_shared<handler_observation>();
   client.register_protocol_handler(p2p::builtins::relay_stop,
       [observation, peer = service.local_peer()](p2p::node::incoming_protocol_stream incoming)
           -> boost::asio::awaitable<void> {
          const auto buffered = co_await read_stop_request(incoming, *observation, peer, {peer});
          require_wire(buffered.empty(), "custom STOP rejection has unexpected request payload");
          co_await incoming.stream.async_write(p2p::relay::codec::encode_stop(p2p::relay::stop_message{
              .kind = p2p::relay::stop_message::message_kind::status,
              .status = p2p::relay::status::resource_limit_exceeded,
          }));
          co_await incoming.stream.async_close();
       });
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   const auto sessions = client.metrics().active_sessions;
   BOOST_CHECK(bounded(runtime, stop_exchange(service, client)) == p2p::relay::status::resource_limit_exceeded);
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   BOOST_TEST(client.unregister_protocol_handler(p2p::builtins::relay_stop));
   BOOST_TEST(!client.unregister_protocol_handler(p2p::builtins::relay_stop));

   BOOST_CHECK(bounded(runtime, stop_exchange(service, client)) == p2p::relay::status::permission_denied);
   BOOST_TEST(observation->entered.load() == 1U);
   BOOST_TEST(observation->decoded.load() == 1U);
   BOOST_TEST(client.metrics().active_sessions == sessions);
   BOOST_TEST(service.metrics().relay_reservations == 0U);
   BOOST_TEST(circuits(client).empty());
   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(identify_wire_protocol_ids_remain_unique_across_builtin_overrides_and_unregister) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service = p2p::node{runtime, options_for("autorelay-identify-unique-service", true)};
   auto observer_options = options_for("autorelay-identify-unique-observer");
   observer_options.relay_policy.auto_discovery_enabled = false;
   auto observer = p2p::node{runtime, std::move(observer_options)};
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, observer.async_connect(service.local_endpoints().front())));
   const auto application = p2p::protocol_id{.value = "/forge/autorelay/unique/1"};
   const auto builtin_ids = std::array{p2p::builtins::relay_hop, p2p::builtins::relay_stop, p2p::builtins::ping};
   const auto check_builtins = [&](const p2p::identify::document& document) {
      check_unique_protocols(document);
      for (const auto& id : builtin_ids) {
         BOOST_TEST(std::ranges::count(document.protocols, id) == 1);
      }
   };
   const auto before = bounded(runtime, wire_identify(observer, service.local_peer()));
   check_builtins(before);
   BOOST_TEST(std::ranges::count(before.protocols, application) == 0);
   const auto close = [](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
      co_await incoming.stream.async_close();
   };
   for (const auto& id : builtin_ids) {
      service.register_protocol_handler(id, close);
   }
   service.register_protocol_handler(application, close);
   const auto overridden = bounded(runtime, wire_identify(observer, service.local_peer()));
   check_builtins(overridden);
   BOOST_TEST(std::ranges::count(overridden.protocols, application) == 1);
   BOOST_TEST(overridden.protocols.size() == before.protocols.size() + 1U);
   for (const auto& id : builtin_ids) {
      BOOST_TEST(service.unregister_protocol_handler(id));
   }
   BOOST_TEST(service.unregister_protocol_handler(application));
   const auto restored = bounded(runtime, wire_identify(observer, service.local_peer()));
   check_builtins(restored);
   BOOST_TEST(std::ranges::count(restored.protocols, application) == 0);
   BOOST_TEST(restored.protocols.size() == before.protocols.size());
   bounded(runtime, observer.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(private_psk_blocks_relay_overrides_but_keeps_authenticated_private_echo) {
   for (const auto egress : {p2p::private_network::internet_egress_policy::deny_external,
                             p2p::private_network::internet_egress_policy::allow_internet}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      const auto private_options = [egress](std::string name) {
         auto options = options_for(std::move(name));
         options.capabilities.bits = p2p::capabilities::peer_exchange;
         options.relay_policy.service_enabled = false;
         options.relay_policy.client_enabled = false;
         options.relay_policy.auto_discovery_enabled = false;
         options.path_policy.allow_direct = true;
         options.path_policy.allow_relay = false;
         options.path_policy.allow_hole_punch = false;
         auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
         for (auto index = std::size_t{}; index < key.size(); ++index) {
            key[index] = static_cast<std::uint8_t>(index);
         }
         options.private_network = p2p::private_network::options{
             .protector = std::make_shared<const forge::net::pnet::protector>(
                 forge::net::pnet::protector{forge::net::pnet::pre_shared_key{key}}),
             .internet_egress = egress,
         };
         return options;
      };
      auto server = p2p::node{runtime, private_options("autorelay-private-server")};
      auto client = p2p::node{runtime, private_options("autorelay-private-client")};
      auto observation = std::make_shared<handler_observation>();
      const auto payload = std::vector<std::uint8_t>{'p', 's', 'k', '-', 'e', 'c', 'h', 'o'};
      server.register_protocol_handler(p2p::builtins::echo,
          [observation, payload, peer = client.local_peer()](p2p::node::incoming_protocol_stream incoming)
              -> boost::asio::awaitable<void> {
             ++observation->entered;
             require_direct_sender(incoming, p2p::builtins::echo, peer);
             const auto request = co_await incoming.stream.async_read_frame();
             require_wire(request == payload, "private echo requires the exact source payload");
             ++observation->decoded;
             co_await incoming.stream.async_write_frame(request);
             co_await incoming.stream.async_close();
          });
      auto rejected_entries = std::make_shared<std::atomic_size_t>(0);
      const auto forbidden = [rejected_entries](p2p::node::incoming_protocol_stream incoming)
          -> boost::asio::awaitable<void> {
         ++*rejected_entries;
         co_await incoming.stream.async_close();
      };
      for (const auto& id : {p2p::builtins::relay_hop, p2p::builtins::relay_stop}) {
         BOOST_CHECK_EXCEPTION(server.register_protocol_handler(id, forbidden), forge::exceptions::base,
             [](const forge::exceptions::base& error) {
                return p2p::exceptions::is(error, p2p::exceptions::code::invalid_options);
             });
         BOOST_TEST(!server.unregister_protocol_handler(id));
      }
      static_cast<void>(bounded(runtime, server.async_start()));
      const auto session = bounded(runtime, client.async_connect(server.local_endpoints().front()));
      BOOST_CHECK(session.remote_peer == server.local_peer());
      BOOST_CHECK(session.path == p2p::path::kind::direct);
      const auto direct_attempts = client.metrics().path_direct_attempts;
      for (const auto& id : {p2p::builtins::relay_hop, p2p::builtins::relay_stop}) {
         BOOST_CHECK_EXCEPTION(bounded(runtime, client.async_open_protocol_stream(server.local_peer(), id)),
             forge::exceptions::base, [](const forge::exceptions::base& error) {
                return p2p::exceptions::is(error, p2p::exceptions::code::invalid_options);
             });
      }
      BOOST_TEST(client.metrics().path_direct_attempts == direct_attempts);
      auto stream = bounded(runtime, client.async_open_protocol_stream(server.local_peer(), p2p::builtins::echo));
      BOOST_CHECK(stream.authentication() == p2p::peer_authentication::libp2p_tls);
      bounded(runtime, stream.async_write_frame(payload));
      BOOST_CHECK(bounded(runtime, stream.async_read_frame()) == payload);
      BOOST_TEST(observation->entered.load() == 1U);
      BOOST_TEST(observation->decoded.load() == 1U);
      BOOST_TEST(rejected_entries->load() == 0U);
      const auto document = bounded(runtime, wire_identify(client, server.local_peer()));
      check_unique_protocols(document);
      BOOST_TEST(std::ranges::count(document.protocols, p2p::builtins::relay_hop) == 0);
      BOOST_TEST(std::ranges::count(document.protocols, p2p::builtins::relay_stop) == 0);
      BOOST_TEST(circuits(server).empty());
      BOOST_TEST(circuits(client).empty());
      bounded(runtime, stream.async_close());
      bounded(runtime, client.async_stop());
      bounded(runtime, server.async_stop());
   }
}

BOOST_AUTO_TEST_SUITE_END()
