#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.events_api;
import forge.plugins.net.p2p.node.exceptions;

#include "details/secure_node_fixture.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using forge::tests::p2p::secure_node_fixture;
using namespace std::chrono_literals;

template<class Predicate>
boost::asio::awaitable<bool> await_state(Predicate predicate, std::chrono::milliseconds budget = 5s) {
   const auto deadline = std::chrono::steady_clock::now() + budget;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 10ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   co_return true;
}

forge::config::core::document reachability(bool ping) {
   auto config = forge::config::core::document{};
   config.set("plugins.net.p2p.node.reachability.client-v1-enabled", false);
   config.set("plugins.net.p2p.node.reachability.client-v2-enabled", false);
   config.set("plugins.net.p2p.node.reachability.ping-enabled", ping);
   config.set("plugins.net.p2p.node.reachability.ping-interval-ms", std::uint64_t{25});
   config.set("plugins.net.p2p.node.reachability.ping-timeout-ms", std::uint64_t{500});
   config.set("plugins.net.p2p.node.reachability.max-parallel-pings", std::uint64_t{1});
   config.set("plugins.net.p2p.node.reachability.max-event-subscribers", std::uint64_t{1});
   return config;
}

forge::config::core::document relay(bool service) {
   auto config = reachability(false);
   config.set("plugins.net.p2p.node.path.policy", service ? "direct-only" : "relay-only");
   config.set("plugins.net.p2p.node.relay.server-enabled", service);
   config.set("plugins.net.p2p.node.relay.client-enabled", !service);
   config.set("plugins.net.p2p.node.relay.auto-discovery-enabled", !service);
   config.set("plugins.net.p2p.node.relay.target-reservations", std::uint64_t{1});
   config.set("plugins.net.p2p.node.relay.refresh-margin-ms", std::uint64_t{1000});
   config.set("plugins.net.p2p.node.relay.candidate-backoff-ms", std::uint64_t{200});
   config.set("plugins.net.p2p.node.relay.max-candidates", std::uint64_t{2});
   config.set("plugins.net.p2p.node.relay.max-parallel-reservations", std::uint64_t{1});
   config.set("plugins.net.p2p.node.relay.reservation-ttl-ms", std::uint64_t{3000});
   return config;
}

p2p::endpoint address(const std::shared_ptr<np::api>& api) {
   auto endpoint = api->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   endpoint->peer = api->local_peer();
   return *endpoint;
}

void check_host_snapshot(const p2p::host_event& event, const p2p::host_event& latest) {
   BOOST_TEST(event.generation == latest.generation);
   BOOST_CHECK(event.phase == latest.phase);
   BOOST_CHECK(event.effective == latest.effective);
   BOOST_CHECK(event.autonat_v1 == latest.autonat_v1);
   BOOST_CHECK(std::ranges::equal(event.autonat_v2, latest.autonat_v2, [](const auto& left, const auto& right) {
      return left.value == right.value && left.address.to_string() == right.address.to_string();
   }));
   BOOST_CHECK(std::ranges::equal(event.confirmed_addresses, latest.confirmed_addresses,
       [](const auto& left, const auto& right) { return left.to_string() == right.to_string(); }));
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_plugin_service_policies,
                      *boost::unit_test::enable_if<FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB>())

BOOST_AUTO_TEST_CASE(configured_autonat_roles_are_advertised_only_when_effective) {
   for (const auto& backend : secure_node_fixture::backends()) {
      for (const auto enabled : {false, true}) {
         for (const auto private_network : {false, true}) {
            BOOST_TEST_CONTEXT(backend << " enabled=" << enabled << " private=" << private_network) {
               auto settings = reachability(false);
               settings.set("plugins.net.p2p.node.reachability.service-v1-enabled", enabled);
               settings.set("plugins.net.p2p.node.reachability.service-v2-enabled", enabled);
               auto server = secure_node_fixture{backend, {.private_profile = private_network}, "noise", settings};
               auto client = secure_node_fixture{backend, {.private_profile = private_network}, "noise", reachability(false)};
               server.start();
               client.start();
               const auto server_api = server.api<np::api>();
               const auto peer = server_api->local_peer();
               const auto connected = client.run(client.api<np::api>()->connect(address(server_api)));
               BOOST_CHECK(connected.remote_peer == peer);
               const auto diagnostics = client.api<np::diagnostics_api>();
               BOOST_REQUIRE(client.run(await_state([&] {
                  const auto peers = diagnostics->snapshot().peers;
                  return std::ranges::any_of(peers, [&](const auto& entry) {
                     return entry.peer == peer && !entry.protocols.empty();
                  });
               })));
               const auto remote = diagnostics->peer(peer);
               const auto effective = enabled && !private_network;
               BOOST_TEST((std::ranges::find(remote.protocols, p2p::builtins::autonat_v1) != remote.protocols.end()) == effective);
               BOOST_TEST((std::ranges::find(remote.protocols, p2p::builtins::autonat_v2_dial_request) != remote.protocols.end()) == effective);
               const auto native = server.api<np::diagnostics_api>()->snapshot().reachability;
               BOOST_TEST(native.service_v1_enabled == effective);
               BOOST_TEST(native.service_v2_enabled == effective);
               BOOST_TEST(!native.client_v1_enabled);
               BOOST_TEST(!native.client_v2_enabled);
               BOOST_TEST(client.run(client.api<np::api>()->ping(peer)).count() >= 0);
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(configured_ping_and_event_capacity_use_native_owners) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto server = secure_node_fixture{backend, {.private_profile = false}, "noise", reachability(false)};
         auto client = secure_node_fixture{backend, {.private_profile = false}, "noise", reachability(true)};
         server.start();
         client.run(client.shell->initialize());
         for (const auto* field : {"reachability.max-event-subscribers", "mdns.max-packet-size", "relay.max-reservations"}) {
            auto invalid = client.config;
            invalid.set("plugins.net.p2p.node.reachability.service-v1-enabled", true);
            invalid.set(std::string{"plugins.net.p2p.node."} + field, std::uint64_t{0});
            BOOST_CHECK_THROW(client.configure_node(invalid), np::exceptions::invalid_config);
         }
         client.start();
         const auto server_api = server.api<np::api>();
         const auto client_api = client.api<np::api>();
         const auto diagnostics = client.api<np::diagnostics_api>();
         const auto events = client.api<np::events_api>();
         auto subscription = events->host_events();
         const auto initial = client.run(subscription.async_read());
         BOOST_REQUIRE(initial.has_value());
         BOOST_TEST(initial->generation == events->reachability_status().generation);
         BOOST_CHECK_THROW((void)events->host_events(), p2p::exceptions::backpressure_rejected);
         subscription.close();
         subscription = events->host_events();
         BOOST_REQUIRE(client.run(subscription.async_read()).has_value());
         const auto connected = client.run(client_api->connect(address(server_api)));
         BOOST_CHECK(connected.remote_peer == server_api->local_peer());
         BOOST_REQUIRE(client.run(await_state([&] {
            return diagnostics->snapshot().reachability.ping_successes >= 2;
         })));
         const auto state = diagnostics->snapshot().reachability;
         BOOST_TEST(state.max_parallel_pings == 1U);
         BOOST_TEST(!state.client_v1_enabled);
         BOOST_TEST(!state.client_v2_enabled);
         BOOST_TEST(!state.service_v1_enabled);
         BOOST_TEST(!state.service_v2_enabled);
         BOOST_TEST(state.ping_failures == 0U);
         BOOST_TEST(server.api<np::diagnostics_api>()->snapshot().reachability.ping_successes == 0U);
         for (const auto* field : {"reachability.max-parallel-pings", "mdns.max-peers", "relay.target-reservations"}) {
            auto candidate = client.config;
            candidate.set(std::string{"plugins.net.p2p.node."} + field, std::uint64_t{3});
            BOOST_CHECK_THROW(client.configure_node(candidate), np::exceptions::route_conflict);
         }
         client.request_stop_node();
         client.run(client.shutdown_node());
         BOOST_TEST(!client.run(subscription.async_read()).has_value());
         BOOST_TEST(!subscription.active());
         BOOST_CHECK_THROW((void)diagnostics->snapshot(), np::exceptions::plugin_not_initialized);
      }
   }
}

BOOST_AUTO_TEST_CASE(configured_autorelay_acquires_renews_and_withdraws_without_manual_trigger) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto service = secure_node_fixture{backend, {.private_profile = false}, "noise", relay(true)};
         auto client = secure_node_fixture{backend, {.private_profile = false}, "noise", relay(false)};
         auto observer_config = relay(false);
         observer_config.set("plugins.net.p2p.node.relay.auto-discovery-enabled", false);
         auto observer = secure_node_fixture{backend, {.private_profile = false}, "noise", observer_config};
         service.start();
         client.start();
         observer.start();
         const auto service_api = service.api<np::api>();
         const auto client_api = client.api<np::api>();
         const auto client_diag = client.api<np::diagnostics_api>();
         const auto peer = service_api->local_peer();
         const auto connected = client.run(client_api->connect(address(service_api)));
         BOOST_CHECK(connected.remote_peer == peer);
         BOOST_REQUIRE(client.run(await_state([&] {
            const auto snapshot = client_diag->snapshot();
            return snapshot.autorelay.automatic_reservations == 1 && snapshot.relay_reservations.size() == 1;
         })));
         auto state = client_diag->snapshot();
         const auto initial_renewals = state.autorelay.renewals;
         const auto initial_expiry = state.relay_reservations.front().expires_at;
         BOOST_TEST(state.autorelay.target_reservations == 1U);
         BOOST_TEST(state.autorelay.max_parallel_reservations == 1U);
         BOOST_TEST(state.autorelay.max_candidates == 2U);
         BOOST_CHECK(state.relay_reservations.front().relay_peer == peer);
         const auto advertised = client_api->local_endpoints();
         const auto circuit = std::ranges::find_if(advertised, [](const auto& value) { return value.relayed.has_value(); });
         BOOST_REQUIRE(circuit != advertised.end());
         // connect() admits a direct carrier; peer operations select the relay path.
         const auto carrier = observer.run(observer.api<np::api>()->connect(address(service_api)));
         BOOST_CHECK(carrier.remote_peer == peer);
         auto ping_options = p2p::node::open_options{};
         ping_options.relay_peer = peer;
         ping_options.allow_hole_punch = false;
         BOOST_TEST(observer.run(observer.api<np::api>()->ping(client_api->local_peer(), ping_options)).count() >= 0);
         const auto observed = observer.api<np::diagnostics_api>()->snapshot();
         BOOST_CHECK(std::ranges::any_of(observed.sessions, [&](const auto& session) {
            return session.remote_peer == client_api->local_peer() && session.path == p2p::path::kind::relay &&
                session.relay_peer == peer && !session.closed;
         }));
         BOOST_REQUIRE(client.run(await_state([&] {
            const auto renewed = client_diag->snapshot();
            return renewed.autorelay.renewals > initial_renewals &&
                std::ranges::any_of(renewed.relay_reservations, [&](const auto& reservation) {
                   return reservation.relay_peer == peer && reservation.expires_at > initial_expiry;
                });
         })));
         BOOST_TEST(client_diag->snapshot().autorelay.automatic_reservations == 1U);
         service.request_stop_node();
         service.run(service.shutdown_node());
         BOOST_REQUIRE(client.run(await_state([&] {
            return client_diag->snapshot().relay_reservations.empty() &&
                std::ranges::none_of(client_api->local_endpoints(), [](const auto& value) { return value.relayed.has_value(); });
         })));
         client.request_stop_node();
         client.run(client.shutdown_node());
      }
   }
}

BOOST_AUTO_TEST_CASE(configured_host_events_coalesce_and_resync, *boost::unit_test::disabled()) {
   const auto* configured_address = std::getenv("FORGE_TEST_P2P_LAN_ADDRESS");
   if (!configured_address || !*configured_address) {
      BOOST_TEST_MESSAGE("NOT_RUN: host-event runtime proof requires FORGE_TEST_P2P_LAN_ADDRESS");
      return;
   }
   const auto ip = boost::asio::ip::make_address(configured_address);
   BOOST_REQUIRE(ip.is_v4());
   BOOST_REQUIRE(!ip.is_loopback());
   BOOST_REQUIRE(!ip.is_unspecified());
   BOOST_REQUIRE(!ip.is_multicast());
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend << " listener=" << ip.to_string()) {
         auto settings = reachability(false);
         settings.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
             forge::config::core::value{"/ip4/" + ip.to_string() + "/tcp/0"}});
         settings.set("plugins.net.p2p.node.reachability.min-observers", std::uint64_t{1});
         settings.set("plugins.net.p2p.node.reachability.observation-ttl-ms", std::uint64_t{500});
         settings.set("plugins.net.p2p.node.reachability.timeout-ms", std::uint64_t{250});
         settings.set("plugins.net.p2p.node.reachability.refresh-interval-ms", std::uint64_t{50});
         auto observed = secure_node_fixture{backend, {.private_profile = false}, "noise", settings};
         auto observer = secure_node_fixture{backend, {.private_profile = false}, "noise", reachability(false)};
         observed.start();
         observer.start();
         const auto events = observed.api<np::events_api>();
         const auto diagnostics = observed.api<np::diagnostics_api>();
         auto subscription = events->host_events();
         const auto initial = observed.run(subscription.async_read());
         BOOST_REQUIRE(initial);
         BOOST_TEST(!initial->resync_required);
         BOOST_TEST(initial->confirmed_addresses.empty());
         const auto listener = address(observed.api<np::api>());
         const auto remote = observer.api<np::api>()->local_peer();
         const auto connection = observer.run(observer.api<np::api>()->connect(listener));
         BOOST_CHECK(connection.remote_peer == observed.api<np::api>()->local_peer());
         BOOST_REQUIRE(observed.run(await_state([&] {
            const auto state = events->reachability_status();
            return state.generation > initial->generation && std::ranges::any_of(state.confirmed_addresses,
                [&](const auto& entry) {
                   return entry.is_direct_tcp() && entry.transport.host == listener.transport.host &&
                       entry.transport.port == listener.transport.port;
                });
         })));
         const auto confirmed = events->reachability_status();
         // Leave the confirmation unread. Expiry must overwrite it with the
         // latest native snapshot and explicitly report that history was lost.
         BOOST_REQUIRE(observed.run(await_state([&] {
            const auto state = events->reachability_status();
            return state.generation > confirmed.generation && state.confirmed_addresses.empty();
         })));
         const auto latest = events->reachability_status();
         const auto update = observed.run(subscription.async_read());
         BOOST_REQUIRE(update);
         BOOST_TEST(update->resync_required);
         check_host_snapshot(*update, latest);
         BOOST_TEST(!latest.resync_required);
         const auto sessions = diagnostics->snapshot().sessions;
         BOOST_CHECK(std::ranges::any_of(sessions, [&](const auto& session) {
            return session.remote_peer == remote && !session.closed &&
                session.direction == p2p::diagnostics::session_direction::inbound &&
                session.identify_state == p2p::identify::state::identified;
         }));
         subscription.close();
         subscription = events->host_events();
         const auto fresh = observed.run(subscription.async_read());
         BOOST_REQUIRE(fresh);
         BOOST_TEST(!fresh->resync_required);
         check_host_snapshot(*fresh, latest);
         observed.request_stop_node();
         observed.run(observed.shutdown_node());
         BOOST_TEST(!subscription.active());
         BOOST_TEST(!observed.run(subscription.async_read()).has_value());
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
