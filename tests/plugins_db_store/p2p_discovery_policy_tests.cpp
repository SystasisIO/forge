#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.config.core.document;
import forge.config.core.value;
import forge.multiformats.varint;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.rendezvous;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.protocol_api;

#include "../fixtures/local_dns_server.hxx"
#include "details/secure_node_fixture.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
namespace dns = forge::tests::dns;
using forge::tests::p2p::secure_node_fixture;
using value = forge::config::core::value;
using namespace std::chrono_literals;

template<class Predicate>
boost::asio::awaitable<bool> await_state(Predicate predicate) {
   const auto deadline = std::chrono::steady_clock::now() + 5s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 10ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   co_return true;
}

std::optional<dns::bytes> discovery_dns(const std::uint8_t* data, std::size_t size) {
   const auto question = dns::parse_question(data, size);
   if (!question) { return std::nullopt; }
   if (question->name != "configured-discovery.test") {
      return dns::make_failure_response(data, *question, 3);
   }
   const auto answers = question->type == 1
       ? std::vector<dns::response_answer>{{.type = 1, .value = {127, 0, 0, 1}}}
       : std::vector<dns::response_answer>{};
   return dns::make_response(data, *question, answers);
}

forge::config::core::document settings(std::uint16_t dns_port, std::string role) {
   auto config = forge::config::core::document{};
   config.set("plugins.net.p2p.node.dns.resolver.nameservers", value::array_type{value::object_type{
       {"address", "127.0.0.1"}, {"port", std::uint64_t{dns_port}}}});
   config.set("plugins.net.p2p.node.dht.profiles", value::array_type{});
   config.set("plugins.net.p2p.node.peer-exchange.enabled", false);
   config.set("plugins.net.p2p.node.rendezvous.role", std::move(role));
   config.set("plugins.net.p2p.node.reachability.client-v1-enabled", false);
   config.set("plugins.net.p2p.node.reachability.client-v2-enabled", false);
   config.set("plugins.net.p2p.node.reachability.ping-enabled", false);
   return config;
}

forge::config::core::document client_settings(std::uint16_t dns_port, const std::string& point,
                                             bool managed = true) {
   auto config = settings(dns_port, "client");
   config.set("plugins.net.p2p.node.topology.mode", managed ? "managed" : "static-only");
   config.set("plugins.net.p2p.node.topology.peers.low", std::uint64_t{2});
   config.set("plugins.net.p2p.node.topology.peers.target", std::uint64_t{2});
   config.set("plugins.net.p2p.node.topology.peers.high", std::uint64_t{3});
   config.set("plugins.net.p2p.node.topology.refresh-interval-ms", std::uint64_t{100});
   config.set("plugins.net.p2p.node.topology.query-timeout-ms", std::uint64_t{1000});
   config.set("plugins.net.p2p.node.topology.max-parallel-queries", std::uint64_t{1});
   config.set("plugins.net.p2p.node.topology.max-parallel-dials", std::uint64_t{1});
   config.set("plugins.net.p2p.node.topology.retry-jitter", 0.0);
   config.set("plugins.net.p2p.node.rendezvous.points", value::array_type{value::object_type{
       {"endpoint", point}, {"namespaces", value::array_type{value{"forge.configured.discovery"}}}}});
   return config;
}

bool identified(const std::shared_ptr<np::diagnostics_api>& api, const p2p::peer_id& peer) {
   const auto sessions = api->snapshot().sessions;
   return std::ranges::any_of(sessions, [&](const auto& session) {
      return session.remote_peer == peer && !session.closed &&
          session.identify_state == p2p::identify::state::identified &&
          session.authentication == p2p::peer_authentication::noise && session.muxer.value == "/yamux/1.0.0";
   });
}

boost::asio::awaitable<std::vector<std::uint8_t>> read_rendezvous_frame(p2p::stream& stream) {
   // Reuse the interop fixture's bounded unsigned-varint frame pattern, not transport's four-byte framing.
   constexpr auto maximum_payload = std::size_t{64 * 1024};
   auto buffered = std::vector<std::uint8_t>{};
   auto prefix = std::size_t{};
   auto size = std::size_t{};
   auto complete = false;
   while (!complete || buffered.size() < prefix + size) {
      auto next = co_await stream.async_read();
      if (next.empty() || next.size() > maximum_payload + 3 - buffered.size()) {
         throw std::runtime_error{"truncated or oversized Rendezvous observation frame"};
      }
      buffered.insert(buffered.end(), next.begin(), next.end());
      while (!complete && prefix < buffered.size()) {
         const auto byte = buffered[prefix++];
         if (prefix == 3 && (byte & 128U)) {
            throw std::runtime_error{"oversized Rendezvous observation prefix"};
         }
         if (!(byte & 128U)) {
            const auto decoded = forge::multiformats::varint_decode(
                std::span<const std::uint8_t>{buffered}.first(prefix));
            if (decoded.value == 0 || decoded.value > maximum_payload) {
               throw std::runtime_error{"Rendezvous observation frame outside fixture bound"};
            }
            size = static_cast<std::size_t>(decoded.value);
            complete = true;
         }
      }
   }
   if (buffered.size() != prefix + size) {
      throw std::runtime_error{"concatenated Rendezvous observation response"};
   }
   co_return buffered;
}

boost::asio::awaitable<p2p::rendezvous::discover_response>
discover_rendezvous_once(std::shared_ptr<np::protocol_api> protocols, p2p::peer_id point) {
   auto stream = co_await protocols->open(point, p2p::builtins::rendezvous,
                                          {.allow_relay = false, .timeout = 1s});
   try {
      if (stream.authentication() != p2p::peer_authentication::noise) {
         throw std::runtime_error{"Rendezvous observer did not open an authenticated Noise stream"};
      }
      // Empty cookie is essential: incremental DISCOVER responses contain no deletion tombstones.
      co_await stream.async_write(p2p::rendezvous::codec::encode(p2p::rendezvous::message{
          .type = p2p::rendezvous::message_type::discover,
          .discover_value = p2p::rendezvous::discover_request{
              .namespace_name = "forge.configured.discovery", .limit = 8, .cookie = {}}}));
      auto limits = p2p::rendezvous::options{};
      limits.max_message_size = 64 * 1024;
      limits.max_discover_limit = 8;
      auto response = p2p::rendezvous::codec::decode(co_await read_rendezvous_frame(stream), limits);
      if (response.type != p2p::rendezvous::message_type::discover_response ||
          !response.discover_response_value ||
          response.discover_response_value->status_value != p2p::rendezvous::status::ok) {
         throw std::runtime_error{"Rendezvous observer did not receive a successful full snapshot"};
      }
      co_await stream.async_close();
      co_return std::move(*response.discover_response_value);
   } catch (...) {
      stream.request_cancel();
      throw;
   }
}

boost::asio::awaitable<p2p::rendezvous::discover_response>
observe_rendezvous(std::shared_ptr<np::protocol_api> protocols, p2p::peer_id point,
                   std::chrono::milliseconds timeout = 2s) {
   const auto executor = boost::asio::make_strand(co_await boost::asio::this_coro::executor);
   // The timeout and request share one strand; co_spawn completion joins cancellation, with no detached task.
   co_return co_await boost::asio::co_spawn(executor, discover_rendezvous_once(std::move(protocols), std::move(point)),
       boost::asio::bind_executor(executor, boost::asio::cancel_after(timeout, boost::asio::use_awaitable)));
}

void expect_signed_registration(const p2p::rendezvous::discover_response& response, const p2p::peer_id& peer,
                                const std::vector<p2p::endpoint>& endpoints) {
   BOOST_TEST(std::ranges::count_if(response.registrations, [&](const auto& entry) { return entry.peer == peer; }) == 1);
   const auto entry = std::ranges::find_if(response.registrations, [&](const auto& value) { return value.peer == peer; });
   BOOST_REQUIRE(entry != response.registrations.end());
   BOOST_TEST(entry->namespace_name == "forge.configured.discovery");
   BOOST_TEST(entry->ttl.count() == 7200);
   BOOST_REQUIRE(!entry->signed_peer_record.empty());
   const auto record = p2p::rendezvous::codec::open_peer_record(
       p2p::signed_envelope::decode(entry->signed_peer_record), peer);
   BOOST_CHECK(record.peer == peer);
   BOOST_REQUIRE(!endpoints.empty());
   BOOST_TEST(record.endpoints.size() == endpoints.size());
   for (auto expected : endpoints) {
      expected.peer = peer;
      BOOST_CHECK(std::ranges::any_of(record.endpoints, [&](const auto& address) {
         auto actual = p2p::parse_endpoint(address.to_string());
         if (actual.peer && *actual.peer != peer) { return false; }
         actual.peer = peer;
         return actual.to_string() == expected.to_string();
      }));
   }
}

boost::asio::awaitable<p2p::rendezvous::discover_response>
await_seeker_unregistered(std::shared_ptr<np::protocol_api> protocols, p2p::peer_id point,
                          p2p::peer_id seeker, p2p::peer_id hidden) {
   const auto deadline = std::chrono::steady_clock::now() + 5s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   do {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining <= 0ms) { break; }
      auto response = co_await observe_rendezvous(protocols, point, std::min(remaining, std::chrono::milliseconds{2000}));
      const auto retained = std::ranges::any_of(response.registrations, [&](const auto& entry) {
         return entry.peer == hidden;
      });
      if (!retained) { throw std::runtime_error{"Rendezvous unregister observation lost its live hidden control"}; }
      if (std::ranges::none_of(response.registrations, [&](const auto& entry) { return entry.peer == seeker; })) {
         co_return response;
      }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 10ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   throw std::runtime_error{"configured seeker remained registered after joined shutdown"};
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_plugin_discovery_policies,
                      *boost::unit_test::enable_if<FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB>())

BOOST_AUTO_TEST_CASE(configured_rendezvous_discovers_hidden_peer_without_manual_discovery) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto dns_server = dns::local_dns_server{discovery_dns};
         auto point = secure_node_fixture{backend, {.private_profile = false}, "noise", settings(dns_server.port(), "server")};
         point.start();
         auto point_address = point.api<np::api>()->local_endpoint();
         BOOST_REQUIRE(point_address);
         point_address->peer = point.api<np::api>()->local_peer();
         const auto coordinate = point_address->to_string();

         // Reserve an OS-selected port until immediately before startup, so the
         // signed DNS advertisement names the actual listener, not a placeholder.
         auto io = boost::asio::io_context{};
         auto reserved = boost::asio::ip::tcp::acceptor{io,
             {boost::asio::ip::make_address("127.0.0.1"), 0}};
         const auto port = std::to_string(reserved.local_endpoint().port());
         auto hidden_config = client_settings(dns_server.port(), coordinate);
         hidden_config.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/127.0.0.1/tcp/" + port}});
         hidden_config.set("plugins.net.p2p.node.advertised-endpoints",
                           value::array_type{value{"/dns4/configured-discovery.test/tcp/" + port}});
         auto hidden = secure_node_fixture{backend, {.private_profile = false}, "noise", hidden_config};
         reserved.close();
         const auto registration_started = std::chrono::steady_clock::now();
         hidden.start();
         BOOST_REQUIRE(point.run(await_state([&] {
            return point.api<np::diagnostics_api>()->snapshot().metrics.rendezvous_registrations > 0;
         })));

         auto seeker = secure_node_fixture{backend, {.private_profile = false}, "noise",
                                          client_settings(dns_server.port(), coordinate)};
         seeker.start();
         const auto target = hidden.api<np::api>()->local_peer();
         const auto diagnostics = seeker.api<np::diagnostics_api>();
         BOOST_REQUIRE(seeker.run(await_state([&] { return identified(diagnostics, target); })));
         const auto learned = diagnostics->peer(target);
         BOOST_CHECK(learned.discovered_by == p2p::discovery::source::rendezvous);
         const auto state = diagnostics->snapshot();
         BOOST_TEST(state.topology.configured_rendezvous_points == 1U);
         BOOST_TEST(!state.topology.dht_enabled);
         BOOST_TEST(!state.topology.peer_exchange_enabled);
         const auto served = point.api<np::diagnostics_api>()->snapshot().metrics;
         BOOST_TEST(served.rendezvous_registrations >= 2U);
         BOOST_TEST(served.rendezvous_discovers >= 2U);
         BOOST_TEST(seeker.run(seeker.api<np::api>()->ping(target)).count() >= 0);

         auto idle = secure_node_fixture{backend, {.private_profile = false}, "noise",
                                        client_settings(dns_server.port(), coordinate, false)};
         idle.start();
         const auto idle_diagnostics = idle.api<np::diagnostics_api>();
         auto delay = boost::asio::steady_timer{idle.shell->runtime().context(), 300ms};
         idle.run(delay.async_wait(boost::asio::use_awaitable));
         const auto idle_state = idle_diagnostics->snapshot();
         BOOST_TEST(idle_state.topology.mode == "static-only");
         BOOST_TEST(idle_state.metrics.rendezvous_registrations == 0U);
         BOOST_TEST(idle_state.metrics.rendezvous_discovers == 0U);
         BOOST_TEST(idle_state.metrics.sessions_opened == 0U);
         BOOST_TEST(idle_state.topology.active_operations == 0U);

         // This observer starts only AFTER autonomous discovery and static-only inactivity are proven.
         const auto point_peer = point.api<np::api>()->local_peer();
         const auto seeker_peer = seeker.api<np::api>()->local_peer();
         const auto hidden_endpoints = hidden.api<np::api>()->local_endpoints();
         const auto seeker_endpoints = seeker.api<np::api>()->local_endpoints();
         const auto observed_session = idle.run(idle.api<np::api>()->connect(*point_address,
             {.expected_peer = point_peer, .allow_relay = false, .timeout = 2s}), "Rendezvous observer connect");
         BOOST_CHECK(observed_session.remote_peer == point_peer);
         BOOST_REQUIRE(idle.run(await_state([&] { return identified(idle_diagnostics, point_peer); })));
         const auto protocols = idle.api<np::protocol_api>();
         const auto registered = idle.run(observe_rendezvous(protocols, point_peer), "full registered peer observation");
         BOOST_REQUIRE_EQUAL(registered.registrations.size(), 2U);
         expect_signed_registration(registered, seeker_peer, seeker_endpoints);
         expect_signed_registration(registered, target, hidden_endpoints);
         seeker.request_stop_node();
         seeker.run(seeker.shutdown_node(), "configured seeker joined shutdown");
         const auto removed = idle.run(await_seeker_unregistered(protocols, point_peer, seeker_peer, target),
                                       "full automatic unregister observation");
         BOOST_REQUIRE_EQUAL(removed.registrations.size(), 1U);
         BOOST_CHECK(std::ranges::none_of(removed.registrations, [&](const auto& entry) { return entry.peer == seeker_peer; }));
         expect_signed_registration(removed, target, hidden_endpoints);
         // Point uses its native 7200s default TTL; this removal must occur long before natural expiry.
         BOOST_TEST((std::chrono::steady_clock::now() - registration_started < 1min));
         hidden.request_stop_node();
         hidden.run(hidden.shutdown_node());
         dns_server.close();
      }
   }
}

BOOST_AUTO_TEST_CASE(configured_private_mdns_discovers_without_seed_and_retires_observations,
                     *boost::unit_test::disabled()) {
   const auto* configured_address = std::getenv("FORGE_TEST_P2P_LAN_ADDRESS");
   if (!configured_address || !*configured_address) {
      BOOST_TEST_MESSAGE("NOT_RUN: mDNS runtime proof requires FORGE_TEST_P2P_LAN_ADDRESS");
      return;
   }
   const auto ip = boost::asio::ip::make_address(configured_address);
   BOOST_REQUIRE(ip.is_v4());
   BOOST_REQUIRE(!ip.is_loopback());
   BOOST_REQUIRE(!ip.is_unspecified());
   BOOST_REQUIRE(!ip.is_multicast());
   const auto configured_mdns = [&](bool enabled) {
      auto config = forge::config::core::document{};
      config.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/" + ip.to_string() + "/tcp/0"}});
      config.set("plugins.net.p2p.node.dht.profiles", value::array_type{});
      config.set("plugins.net.p2p.node.peer-exchange.enabled", false);
      config.set("plugins.net.p2p.node.topology.mode", "managed");
      config.set("plugins.net.p2p.node.topology.peers.low", std::uint64_t{1});
      config.set("plugins.net.p2p.node.topology.peers.target", std::uint64_t{1});
      config.set("plugins.net.p2p.node.topology.peers.high", std::uint64_t{2});
      config.set("plugins.net.p2p.node.topology.refresh-interval-ms", std::uint64_t{100});
      config.set("plugins.net.p2p.node.topology.query-timeout-ms", std::uint64_t{1000});
      config.set("plugins.net.p2p.node.topology.max-parallel-dials", std::uint64_t{1});
      config.set("plugins.net.p2p.node.topology.retry-jitter", 0.0);
      config.set("plugins.net.p2p.node.mdns.enabled", enabled);
      config.set("plugins.net.p2p.node.mdns.ipv4-enabled", true);
      config.set("plugins.net.p2p.node.mdns.ipv6-enabled", false);
      config.set("plugins.net.p2p.node.mdns.record-ttl-seconds", std::uint64_t{2});
      config.set("plugins.net.p2p.node.mdns.query-interval-ms", std::uint64_t{1000});
      config.set("plugins.net.p2p.node.reachability.client-v1-enabled", false);
      config.set("plugins.net.p2p.node.reachability.client-v2-enabled", false);
      config.set("plugins.net.p2p.node.reachability.ping-enabled", false);
      return config;
   };
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend << " listener=" << ip.to_string()) {
         // This isolated test namespace cannot discover ordinary public-network peers.
         const auto private_network = secure_node_fixture::secret_policy{.hex = std::string(64, '9')};
         auto advertiser = secure_node_fixture{backend, private_network, "noise", configured_mdns(true)};
         advertiser.start();
         auto disabled = secure_node_fixture{backend, private_network, "noise", configured_mdns(false)};
         disabled.start();
         auto delay = boost::asio::steady_timer{disabled.shell->runtime().context(), 750ms};
         disabled.run(delay.async_wait(boost::asio::use_awaitable));
         const auto quiet = disabled.api<np::diagnostics_api>()->snapshot();
         BOOST_TEST(quiet.topology.mdns_observations == 0U);
         BOOST_TEST(quiet.metrics.sessions_opened == 0U);

         auto seeker = secure_node_fixture{backend, private_network, "noise", configured_mdns(true)};
         seeker.start();
         const auto diagnostics = seeker.api<np::diagnostics_api>();
         const auto target = advertiser.api<np::api>()->local_peer();
         BOOST_REQUIRE(seeker.run(await_state([&] {
            return diagnostics->snapshot().topology.mdns_observations == 1U && identified(diagnostics, target);
         })));
         BOOST_TEST(!diagnostics->snapshot().topology.dht_enabled);
         BOOST_TEST(!diagnostics->snapshot().topology.peer_exchange_enabled);
         BOOST_TEST(diagnostics->snapshot().topology.configured_rendezvous_points == 0U);
         BOOST_TEST(seeker.run(seeker.api<np::api>()->ping(target)).count() >= 0);
         BOOST_TEST(disabled.api<np::diagnostics_api>()->snapshot().metrics.sessions_opened == 0U);
         advertiser.request_stop_node();
         advertiser.run(advertiser.shutdown_node());
         BOOST_REQUIRE(seeker.run(await_state([&] {
            return diagnostics->snapshot().topology.mdns_observations == 0U;
         })));
         seeker.request_stop_node();
         seeker.run(seeker.shutdown_node());
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
