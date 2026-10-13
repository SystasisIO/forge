#include <boost/test/unit_test.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

import forge.config.core.document;
import forge.config.core.value;
import forge.multiformats.multihash;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.exceptions;

#include "details/secure_node_fixture.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using forge::tests::p2p::secure_node_fixture;
using value = forge::config::core::value;
using namespace std::chrono_literals;
const auto first_profile = p2p::protocol_id{"/forge/test/configured-records/first/1"};
const auto second_profile = p2p::protocol_id{"/forge/test/configured-records/second/1"};

value profile(const p2p::protocol_id& id, std::uint64_t count, std::uint64_t bytes) {
   return value::object_type{
       {"kind", "custom"}, {"mode", "server"}, {"protocol", id.value},
       {"peers", true}, {"providers", false}, {"values", true},
       {"records", value::object_type{
           {"max-values", count}, {"max-total-bytes", bytes},
           {"hydration-page-limit", std::uint64_t{1}},
           {"prune-page-limit", std::uint64_t{1}},
           {"max-hydration-pages", std::uint64_t{4}}}}};
}

forge::config::core::document settings(std::uint64_t count, std::uint64_t bytes = 4096) {
   auto out = forge::config::core::document{};
   out.set("plugins.net.p2p.node.dht.profiles", value::array_type{
       profile(first_profile, count, bytes), profile(second_profile, count, bytes)});
   out.set("plugins.net.p2p.node.reachability.client-v1-enabled", false);
   out.set("plugins.net.p2p.node.reachability.client-v2-enabled", false);
   out.set("plugins.net.p2p.node.reachability.ping-enabled", false);
   return out;
}

std::vector<p2p::dht::value_policy> policies() {
   return {{
       .key_prefix = {'/', 't', '/'},
       .validate = [](const p2p::dht::record&, p2p::dht::value_validation_context) {},
       .select = [](std::span<const p2p::dht::record> candidates) {
          auto best = std::size_t{0};
          for (auto index = std::size_t{1}; index < candidates.size(); ++index) {
             if (candidates[index].value > candidates[best].value) { best = index; }
          }
          return best;
       },
       .expiry = [](const p2p::dht::record&, p2p::dht::value_expiry_context context) {
          return std::min(context.supplied_expires_at, context.now + 1h);
       },
   }};
}

void start(secure_node_fixture& fixture) {
   fixture.run(fixture.shell->initialize());
   auto composition = fixture.api<np::configuration_api>();
   composition->set_dht_value_policies(first_profile, policies());
   composition->set_dht_value_policies(second_profile, policies());
   fixture.start();
}

p2p::dht::record record(std::uint8_t suffix, std::size_t bytes = 32) {
   return {.key_value = {{'/', 't', '/', suffix}},
           .value = std::vector<std::uint8_t>(bytes, suffix), .ttl = 1h};
}

void expect_local_value(secure_node_fixture& fixture, const p2p::protocol_id& id,
                        const p2p::dht::record& expected) {
   const auto found = fixture.run(fixture.api<np::dht_api>()->get_value(id, expected.key_value));
   BOOST_REQUIRE(found.selected.has_value());
   BOOST_CHECK(found.selected->key_value == expected.key_value);
   BOOST_CHECK(found.selected->value == expected.value);
   BOOST_TEST(found.responses == 0U);
   BOOST_TEST(found.valid_records == 1U);
}

value provider_profile(const p2p::protocol_id& id, const std::string& mode,
                       std::uint64_t count, std::uint64_t local_count) {
   return value::object_type{
       {"kind", "custom"}, {"mode", mode}, {"protocol", id.value},
       {"peers", true}, {"providers", true}, {"values", false},
       {"records", value::object_type{
           {"max-providers", count}, {"max-local-providers", local_count},
           {"max-total-bytes", std::uint64_t{16384}},
           {"hydration-page-limit", std::uint64_t{1}},
           {"prune-page-limit", std::uint64_t{1}},
           {"max-hydration-pages", std::uint64_t{8}}}}};
}

forge::config::core::document provider_settings(const std::string& mode,
                                               std::uint64_t count, std::uint64_t local_count) {
   auto out = settings(1);
   out.set("plugins.net.p2p.node.dht.profiles", value::array_type{
       provider_profile(first_profile, mode, count, local_count),
       provider_profile(second_profile, mode, count, local_count)});
   return out;
}

p2p::dht::key provider_key(std::uint8_t suffix) {
   return p2p::make_dht_key(forge::multiformats::multihash::sha2_256(
       std::vector<std::uint8_t>{'p', 'r', 'o', 'v', 'i', 'd', 'e', '/', suffix}).encode());
}

boost::asio::awaitable<bool> await_provider_server(std::shared_ptr<np::diagnostics_api> diagnostics,
                                                  p2p::peer_id peer, std::uint64_t session_id) {
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   do {
      const auto state = diagnostics->snapshot();
      const auto identified = std::ranges::any_of(state.sessions, [&](const auto& session) {
         return session.id == session_id && session.remote_peer == peer && !session.closed &&
             session.authentication == p2p::peer_authentication::noise &&
             session.muxer.value == "/yamux/1.0.0" &&
             session.identify_state == p2p::identify::state::identified;
      });
      if (identified && diagnostics->routing_status(first_profile).active == 1 &&
          diagnostics->routing_status(second_profile).active == 1) { co_return true; }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 10ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   co_return false;
}

void connect_provider_server(secure_node_fixture& client, secure_node_fixture& server) {
   const auto server_api = server.api<np::api>();
   auto address = server_api->local_endpoint();
   BOOST_REQUIRE(address.has_value());
   BOOST_REQUIRE(address->is_direct_tcp());
   const auto peer = server_api->local_peer();
   address->peer = peer;
   const auto session = client.run(client.api<np::api>()->connect(
       *address, {.expected_peer = peer, .allow_relay = false, .timeout = 2s}), "provider server connect");
   BOOST_CHECK(session.remote_peer == peer);
   BOOST_REQUIRE(session.id != 0U);
   BOOST_TEST(session.muxer.value == "/yamux/1.0.0");
   const auto diagnostics = client.api<np::diagnostics_api>();
   BOOST_REQUIRE(client.run(await_provider_server(diagnostics, peer, session.id),
                            "provider server Identify and profile admission"));
   const auto state = diagnostics->snapshot();
   const auto observed = std::ranges::find_if(state.sessions, [&](const auto& entry) {
      return entry.id == session.id;
   });
   BOOST_REQUIRE(observed != state.sessions.end());
   BOOST_CHECK(observed->remote_peer == peer);
   BOOST_TEST(!observed->closed);
   BOOST_CHECK(observed->authentication == p2p::peer_authentication::noise);
   BOOST_TEST(observed->muxer.value == "/yamux/1.0.0");
   BOOST_CHECK(observed->identify_state == p2p::identify::state::identified);
}

std::vector<p2p::dht::peer> providers(secure_node_fixture& fixture, const p2p::protocol_id& id,
                                    const p2p::dht::key& key) {
   return fixture.run(fixture.api<np::dht_api>()->find_providers(
       id, key, {.requested_count = 1, .timeout = 2s}), "configured provider lookup");
}

void expect_provider(const std::vector<p2p::dht::peer>& found, const p2p::peer_id& peer) {
   BOOST_REQUIRE_EQUAL(found.size(), 1U);
   BOOST_CHECK(found.front().id == peer);
   BOOST_REQUIRE(!found.front().endpoints.empty());
   BOOST_CHECK(std::ranges::any_of(found.front().endpoints, [](const auto& endpoint) {
      return p2p::parse_endpoint(endpoint.to_string()).is_direct_tcp();
   }));
}

boost::asio::awaitable<bool> await_stored_provider(std::shared_ptr<np::dht_api> dht,
                                                  std::shared_ptr<np::diagnostics_api> diagnostics,
                                                  p2p::protocol_id id, p2p::dht::key key, p2p::peer_id peer) {
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   do {
      const auto before = diagnostics->snapshot().metrics.protocol_streams_opened;
      const auto found = co_await dht->find_providers(id, key, {.requested_count = 1, .timeout = 500ms});
      // provide() joins its outbound send, not the receiver's durable ADD_PROVIDER commit.
      if (found.size() == 1 && found.front().id == peer && !found.front().endpoints.empty() &&
          diagnostics->snapshot().metrics.protocol_streams_opened == before) { co_return true; }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 10ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   co_return false;
}

void expect_stored_provider(secure_node_fixture& fixture, const p2p::protocol_id& id,
                            const p2p::dht::key& key, const p2p::peer_id& peer) {
   const auto diagnostics = fixture.api<np::diagnostics_api>();
   BOOST_REQUIRE(fixture.run(await_stored_provider(fixture.api<np::dht_api>(), diagnostics, id, key, peer),
                             "durable provider receiver admission"));
   const auto before = diagnostics->snapshot().metrics.protocol_streams_opened;
   expect_provider(providers(fixture, id, key), peer);
   BOOST_TEST(diagnostics->snapshot().metrics.protocol_streams_opened == before);
}

void expect_provider_capacity(secure_node_fixture& fixture, const p2p::protocol_id& id,
                              const p2p::dht::key& key) {
   // Native prepare rejects before upsert; registry rollback may still issue an idempotent durable removal.
   BOOST_CHECK_EXCEPTION(fixture.run(fixture.api<np::dht_api>()->provide(id, key, {.timeout = 2s}),
                                      "configured provider quota rejection"),
                         p2p::exceptions::backpressure_rejected, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::backpressure_rejected;
   });
   const auto profiles = fixture.api<np::diagnostics_api>()->snapshot().dht_profiles;
   const auto state = std::ranges::find_if(profiles, [&](const auto& entry) { return entry.protocol == id; });
   BOOST_REQUIRE(state != profiles.end());
   BOOST_TEST(state->persistence_failures == 0U);
   BOOST_TEST(!state->persistence_degraded);
   BOOST_TEST(!state->durability_uncertain);
}

void stop_node(secure_node_fixture& fixture) {
   fixture.request_stop_node();
   fixture.run(fixture.shutdown_node(), "provider fixture node shutdown");
}

void expect_fresh_reader(secure_node_fixture& reader, secure_node_fixture& server,
                         const p2p::peer_id& provider) {
   const auto reader_peer = reader.api<np::api>()->local_peer();
   BOOST_CHECK(reader_peer != provider);
   BOOST_CHECK(reader_peer != server.api<np::api>()->local_peer());
   BOOST_CHECK(provider != server.api<np::api>()->local_peer());
   const auto diagnostics = reader.api<np::diagnostics_api>();
   BOOST_TEST(diagnostics->snapshot().peers.empty());
   BOOST_TEST(diagnostics->routing_status(first_profile).active == 0U);
   connect_provider_server(reader, server);
   const auto state = diagnostics->snapshot();
   BOOST_CHECK(std::ranges::none_of(state.peers, [&](const auto& peer) { return peer.peer == provider; }));
   BOOST_CHECK(std::ranges::none_of(state.sessions, [&](const auto& session) { return session.remote_peer == provider; }));
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_plugin_dht_policies,
                      *boost::unit_test::enable_if<FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB>())

BOOST_AUTO_TEST_CASE(configured_record_capacity_survives_reopen_and_isolates_profiles) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto fixture = secure_node_fixture{backend, {.private_profile = false}, "noise", settings(1)};
         start(fixture);
         const auto first = record('a');
         const auto second = record('b');
         auto dht = fixture.api<np::dht_api>();
         const auto stored = fixture.run(dht->put_value(first_profile, first));
         // No remote peers: this proves local durable storage, not network publication.
         BOOST_TEST(!stored.quorum_reached);
         BOOST_TEST(stored.accepted == 0U);
         BOOST_TEST(stored.attempted == 0U);
         BOOST_CHECK_THROW(fixture.run(dht->put_value(first_profile, second)), p2p::exceptions::backpressure_rejected);
         fixture.run(dht->put_value(second_profile, second));
         expect_local_value(fixture, first_profile, first);
         expect_local_value(fixture, second_profile, second);

         fixture.reopen();
         start(fixture);
         BOOST_CHECK_THROW(fixture.run(dht->get_value(first_profile, first.key_value)),
                           p2p::exceptions::canceled);
         dht = fixture.api<np::dht_api>();
         expect_local_value(fixture, first_profile, first);
         expect_local_value(fixture, second_profile, second);
         BOOST_CHECK_THROW(fixture.run(dht->put_value(first_profile, second)), p2p::exceptions::backpressure_rejected);

         fixture.config.set("plugins.net.p2p.node.dht.profiles", value::array_type{
             profile(first_profile, 2, 4096), profile(second_profile, 1, 4096)});
         fixture.reopen();
         start(fixture);
         fixture.run(fixture.api<np::dht_api>()->put_value(first_profile, second));
         fixture.reopen();
         start(fixture);
         expect_local_value(fixture, first_profile, first);
         expect_local_value(fixture, first_profile, second);
         BOOST_CHECK_THROW(fixture.run(fixture.api<np::dht_api>()->put_value(second_profile, first)),
                           p2p::exceptions::backpressure_rejected);
      }
   }
}

BOOST_AUTO_TEST_CASE(configured_record_bytes_reject_before_commit_and_preserve_existing_value) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto fixture = secure_node_fixture{backend, {.private_profile = false}, "noise", settings(8, 256)};
         start(fixture);
         const auto first = record('a', 128);
         const auto second = record('b', 128);
         auto dht = fixture.api<np::dht_api>();
         fixture.run(dht->put_value(first_profile, first));
         // Each record fits individually; only their combined size exceeds this profile's budget.
         fixture.run(dht->put_value(second_profile, second));
         expect_local_value(fixture, second_profile, second);
         BOOST_CHECK_THROW(fixture.run(dht->put_value(first_profile, second)), p2p::exceptions::backpressure_rejected);
         expect_local_value(fixture, first_profile, first);
         fixture.reopen();
         start(fixture);
         expect_local_value(fixture, first_profile, first);
         expect_local_value(fixture, second_profile, second);
         const auto rejected = fixture.run(fixture.api<np::dht_api>()->get_value(first_profile, second.key_value));
         BOOST_TEST(!rejected.selected.has_value());
         BOOST_TEST(!rejected.quorum_reached);
      }
   }
}

BOOST_AUTO_TEST_CASE(configured_provider_discovery_local_quota_and_reconfirmation) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto server = secure_node_fixture{backend, {.private_profile = false}, "noise", provider_settings("server", 4, 4)};
         auto provider = secure_node_fixture{backend, {.private_profile = false}, "noise", provider_settings("client", 4, 1)};
         server.start();
         provider.start();
         connect_provider_server(provider, server);
         const auto peer = provider.api<np::api>()->local_peer();
         const auto first = provider_key('a');
         const auto second = provider_key('b');
         auto dht = provider.api<np::dht_api>();
         auto primary = provider.run(dht->provide(first_profile, first, {.timeout = 2s}));
         BOOST_TEST(primary.active());
         BOOST_CHECK(primary.profile() == first_profile);
         BOOST_CHECK(primary.key().bytes == first.bytes);
         expect_stored_provider(server, first_profile, first, peer);
         expect_provider_capacity(provider, first_profile, second);
         BOOST_TEST(primary.active());
         BOOST_TEST(providers(server, first_profile, second).empty());
         auto isolated = provider.run(dht->provide(second_profile, second, {.timeout = 2s}));
         BOOST_TEST(isolated.active());
         expect_stored_provider(server, second_profile, second, peer);

         auto reader = secure_node_fixture{backend, {.private_profile = false}, "noise", provider_settings("client", 4, 4)};
         reader.start();
         expect_fresh_reader(reader, server, peer);
         const auto before = reader.api<np::diagnostics_api>()->snapshot().metrics.protocol_streams_opened;
         expect_provider(providers(reader, first_profile, first), peer);
         BOOST_TEST(reader.api<np::diagnostics_api>()->snapshot().metrics.protocol_streams_opened > before);
         expect_provider(providers(reader, second_profile, second), peer);
         BOOST_TEST(providers(reader, second_profile, first).empty());
         stop_node(reader);
         stop_node(server);

         provider.reopen();
         provider.start();
         BOOST_TEST(!primary.active());
         BOOST_TEST(!isolated.active());
         // With the other hosts stopped, no remote TTL record can masquerade as restored local ownership.
         BOOST_TEST(providers(provider, first_profile, first).empty());
         BOOST_TEST(providers(provider, first_profile, second).empty());
         BOOST_TEST(providers(provider, second_profile, second).empty());
         BOOST_TEST(provider.api<np::diagnostics_api>()->snapshot().metrics.handshakes_completed == 0U);
         server.reopen();
         server.start();
         expect_stored_provider(server, first_profile, first, peer);
         expect_stored_provider(server, second_profile, second, peer);
         BOOST_TEST(providers(server, first_profile, second).empty());
         connect_provider_server(provider, server);
         dht = provider.api<np::dht_api>();
         auto reconfirmed = provider.run(dht->provide(first_profile, first, {.timeout = 2s}));
         BOOST_TEST(reconfirmed.active());
         expect_provider(providers(provider, first_profile, first), peer);
         expect_provider_capacity(provider, first_profile, second);
         provider.run(reconfirmed.async_withdraw(), "local provider withdrawal");
         BOOST_TEST(!reconfirmed.active());
         // Withdrawal releases the local quota; it is not a remote ADD_PROVIDER deletion.
         expect_stored_provider(server, first_profile, first, peer);
         auto replacement = provider.run(dht->provide(first_profile, second, {.timeout = 2s}));
         BOOST_TEST(replacement.active());
         expect_stored_provider(server, first_profile, second, peer);
         provider.run(replacement.async_withdraw(), "replacement provider withdrawal");
         BOOST_TEST(!replacement.active());
      }
   }
}

BOOST_AUTO_TEST_CASE(configured_provider_capacity_preserves_remote_records_across_reopen) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto server = secure_node_fixture{backend, {.private_profile = false}, "noise", provider_settings("server", 1, 4)};
         auto provider = secure_node_fixture{backend, {.private_profile = false}, "noise", provider_settings("client", 4, 4)};
         server.start();
         provider.start();
         connect_provider_server(provider, server);
         const auto peer = provider.api<np::api>()->local_peer();
         const auto first = provider_key('a');
         const auto second = provider_key('b');
         auto primary = provider.run(provider.api<np::dht_api>()->provide(first_profile, first, {.timeout = 2s}));
         auto isolated = provider.run(provider.api<np::dht_api>()->provide(second_profile, second, {.timeout = 2s}));
         BOOST_TEST(primary.active());
         BOOST_TEST(isolated.active());
         expect_stored_provider(server, first_profile, first, peer);
         expect_stored_provider(server, second_profile, second, peer);
         // The occupied rows are REMOTE, so max-providers (not max-local-providers) owns these rejections.
         expect_provider_capacity(server, first_profile, second);
         expect_provider_capacity(server, second_profile, first);

         auto reader = secure_node_fixture{backend, {.private_profile = false}, "noise", provider_settings("client", 4, 4)};
         reader.start();
         expect_fresh_reader(reader, server, peer);
         expect_provider(providers(reader, first_profile, first), peer);
         expect_provider(providers(reader, second_profile, second), peer);
         stop_node(reader);
         stop_node(provider);
         BOOST_TEST(!primary.active());
         BOOST_TEST(!isolated.active());
         server.reopen();
         server.start();
         // Only the server is running: retained REMOTE rows cannot be repaired by querying another live host.
         expect_stored_provider(server, first_profile, first, peer);
         expect_stored_provider(server, second_profile, second, peer);
         BOOST_TEST(providers(server, first_profile, second).empty());
         BOOST_TEST(providers(server, second_profile, first).empty());
         expect_provider_capacity(server, first_profile, second);
         expect_provider_capacity(server, second_profile, first);

         server.config.set("plugins.net.p2p.node.dht.profiles", value::array_type{
             provider_profile(first_profile, "server", 2, 4), provider_profile(second_profile, "server", 1, 4)});
         server.reopen();
         server.start();
         provider.reopen();
         provider.start();
         connect_provider_server(provider, server);
         // Raising only the first profile's quota admits the exact previously rejected key through ADD_PROVIDER.
         auto admitted = provider.run(provider.api<np::dht_api>()->provide(first_profile, second, {.timeout = 2s}));
         BOOST_TEST(admitted.active());
         expect_stored_provider(server, first_profile, second, peer);
         expect_provider_capacity(server, second_profile, first);
         stop_node(provider);
         BOOST_TEST(!admitted.active());
         server.reopen();
         server.start();
         expect_stored_provider(server, first_profile, first, peer);
         expect_stored_provider(server, first_profile, second, peer);
         expect_stored_provider(server, second_profile, second, peer);
         BOOST_TEST(providers(server, second_profile, first).empty());
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
