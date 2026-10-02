module;

#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <atomic>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
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
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.relay;
import forge.net.p2p.stream;
import forge.net.p2p.topology;

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

boost::asio::awaitable<std::optional<p2p::relay::status>> unsolicited_stop(
    p2p::node& relay, const p2p::node& client) {
   try {
      auto stream = co_await relay.async_open_protocol_stream(client.local_peer(), p2p::builtins::relay_stop);
      co_await stream.async_write(p2p::relay::codec::encode_stop(p2p::relay::stop_message{
          .kind = p2p::relay::stop_message::message_kind::connect,
          .source = p2p::relay::peer{.id = relay.local_peer()},
      }));
      auto buffered = std::vector<std::uint8_t>{};
      const auto response = p2p::relay::codec::decode_stop(
          co_await p2p::async_read_length_delimited(stream, buffered, 4096));
      co_await stream.async_close();
      co_return response.status;
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
      service.register_protocol_handler(p2p::builtins::relay_hop,
          [expires, address](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
             auto ignored = co_await incoming.stream.async_read();
             static_cast<void>(ignored);
             co_await incoming.stream.async_write(p2p::relay::codec::encode_hop(p2p::relay::hop_message{
                 .kind = p2p::relay::hop_message::message_kind::status,
                 .reservation_value = p2p::relay::reservation{.expires_at = expires,
                                                             .relay_endpoints = {address}},
                 .status = p2p::relay::status::ok,
             }));
             co_await incoming.stream.async_close();
          });
      static_cast<void>(bounded(runtime, client.async_connect(address)));
      BOOST_CHECK_THROW(bounded(runtime, client.async_reserve_relay(service.local_peer())), forge::exceptions::base);
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
   auto seen = std::make_shared<std::atomic_bool>(false);
   auto release = std::make_shared<forge::asio::notification>();
   const auto observed = release->epoch();
   service.register_protocol_handler(p2p::builtins::relay_hop,
       [seen, release, observed, address](p2p::node::incoming_protocol_stream incoming)
           -> boost::asio::awaitable<void> {
          static_cast<void>(co_await incoming.stream.async_read());
          seen->store(true);
          static_cast<void>(co_await release->async_wait(observed));
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
   const auto admitted = eventually(runtime, [&] { return seen->load(); });
   BOOST_TEST(admitted);
   bounded(runtime, client.async_cancel_relay(service.local_peer()));
   const auto canceled_promptly = pending.wait_for(300ms) == std::future_status::ready;
   release->notify();
   BOOST_TEST(canceled_promptly);
   BOOST_REQUIRE(pending.wait_for(5s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(pending.get()), forge::exceptions::base);
   const auto record = client.peers().find(service.local_peer());
   BOOST_REQUIRE(record);
   BOOST_TEST(record->relay_reservations.empty());
   BOOST_TEST(circuits(client).empty());
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
      service.register_protocol_handler(p2p::builtins::relay_hop,
          [explicit_zero, address](p2p::node::incoming_protocol_stream incoming)
              -> boost::asio::awaitable<void> {
             static_cast<void>(co_await incoming.stream.async_read());
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
   BOOST_CHECK_THROW(bounded(runtime, second.async_reserve_relay(service.local_peer())), p2p::exceptions::relay_rejected);
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
   BOOST_CHECK_THROW(bounded(runtime, client.async_reserve_relay(service.local_peer())), p2p::exceptions::relay_rejected);
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
   auto refusals = std::make_shared<std::atomic_size_t>(0);
   refusing.register_protocol_handler(p2p::builtins::relay_hop,
       [refusals](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          static_cast<void>(co_await incoming.stream.async_read());
          ++*refusals;
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
   BOOST_REQUIRE(eventually(runtime, [&] { return refusals->load() > 0; }));
   static_cast<void>(bounded(runtime, client.async_connect(healthy.local_endpoints().front())));
   BOOST_TEST(eventually(runtime, [&] { return has_circuit(client, healthy.local_peer()); }));
   BOOST_TEST(!has_circuit(client, refusing.local_peer()));
   BOOST_TEST(client.diagnostics().autorelay.candidates <= 1U);
   bounded(runtime, client.async_stop());
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
   target.register_protocol_handler(p2p::builtins::relay_stop,
       [](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          static_cast<void>(co_await incoming.stream.async_read());
          // Keep the authenticated session alive but never answer STOP.
          static_cast<void>(co_await incoming.stream.async_read());
       });
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, target.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, target.async_reserve_relay(service.local_peer())));
   static_cast<void>(bounded(runtime, source.async_connect(service.local_endpoints().front())));
   const auto sessions = service.metrics().active_sessions;
   const auto started = std::chrono::steady_clock::now();
   auto result = bounded(runtime, hop_connect(source, service, target));
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
   destination.register_protocol_handler(p2p::builtins::relay_stop,
       [payload, marker](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          auto stream = std::move(incoming.stream);
          auto buffered = std::vector<std::uint8_t>{};
          static_cast<void>(p2p::relay::codec::decode_stop(
              co_await p2p::async_read_length_delimited(stream, buffered, 4096)));
          auto response = p2p::relay::codec::encode_stop(p2p::relay::stop_message{
              .kind = p2p::relay::stop_message::message_kind::status,
              .status = p2p::relay::status::ok,
          });
          response.insert(response.end(), payload.begin(), payload.end());
          co_await stream.async_write(std::move(response));
          static_cast<void>(co_await stream.async_read());
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
   BOOST_CHECK(bounded(runtime, stream.async_read()) == payload);
   bounded(runtime, stream.async_write(std::vector<std::uint8_t>{1}));
   BOOST_CHECK(bounded(runtime, stream.async_read()) == marker);
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
   target.register_protocol_handler(p2p::builtins::relay_stop,
       [](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          static_cast<void>(co_await incoming.stream.async_read());
          co_await incoming.stream.async_write(p2p::relay::codec::encode_stop(p2p::relay::stop_message{
              .kind = p2p::relay::stop_message::message_kind::status,
              .status = p2p::relay::status::ok,
          }));
          static_cast<void>(co_await incoming.stream.async_read());
       });
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, target.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, target.async_reserve_relay(service.local_peer())));
   static_cast<void>(bounded(runtime, first.async_connect(service.local_endpoints().front())));
   static_cast<void>(bounded(runtime, second.async_connect(service.local_endpoints().front())));
   auto accepted = bounded(runtime, hop_connect(first, service, target));
   BOOST_CHECK(accepted.first == p2p::relay::status::ok);
   BOOST_REQUIRE(eventually(runtime, [&] { return service.metrics().active_relays == 1U; }));
   static_cast<void>(bounded(runtime, target.async_reserve_relay(service.local_peer())));
   auto denied = bounded(runtime, hop_connect(second, service, target));
   BOOST_CHECK(denied.first == p2p::relay::status::resource_limit_exceeded);
   bounded(runtime, denied.second.async_close());
   bounded(runtime, accepted.second.async_close());
   BOOST_REQUIRE(eventually(runtime, [&] { return service.metrics().active_relays == 0U; }));
   auto retried = bounded(runtime, hop_connect(second, service, target));
   BOOST_CHECK(retried.first == p2p::relay::status::ok);
   bounded(runtime, retried.second.async_close());
   bounded(runtime, second.async_stop());
   bounded(runtime, first.async_stop());
   bounded(runtime, target.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_SUITE_END()
