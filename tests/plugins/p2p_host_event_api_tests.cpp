#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.descriptor;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.app.events;
import forge.app.exceptions;
import forge.app.plugin_context;
import forge.app.plugin_lifecycle;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.events_api;
import forge.plugins.net.p2p.node.plugin;

namespace {

namespace p2p = forge::net::p2p;
namespace node_plugin = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;
using namespace std::chrono_literals;

const auto routing_profile = p2p::protocol_id{.value = "/forge/host-event-test/kad/1"};

class host_identity_source final : public secrets::api {
 public:
   forge::tests::p2p::identity_fixture identity{
       forge::tests::p2p::make_identity_fixture("host-diagnostics-target")};

   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override {
      co_return secrets::snapshot{.configured_secrets = 2};
   }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      const auto* bytes = request.secret_id == "host/cert" ? &identity.certificate_pem
          : request.secret_id == "host/key" ? &identity.private_key_pem : nullptr;
      if (!bytes) { throw std::invalid_argument{"unknown host fixture secret"}; }
      co_return secrets::get_result{.secret_id = request.secret_id, .bytes = {bytes->begin(), bytes->end()}};
   }
   boost::asio::awaitable<secrets::derive_result> derive_hkdf_sha256(secrets::derive_request) override {
      throw std::logic_error{"not a fixture operation"}; co_return secrets::derive_result{};
   }
   boost::asio::awaitable<secrets::aead_encrypt_result> encrypt_aes_gcm(secrets::aead_encrypt_request) override {
      throw std::logic_error{"not a fixture operation"}; co_return secrets::aead_encrypt_result{};
   }
   boost::asio::awaitable<secrets::aead_decrypt_result> decrypt_aes_gcm(secrets::aead_decrypt_request) override {
      throw std::logic_error{"not a fixture operation"}; co_return secrets::aead_decrypt_result{};
   }
};

template <typename Predicate>
boost::asio::awaitable<bool> await_state(Predicate predicate) {
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 5ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   co_return true;
}

template <typename T>
T joined(std::future<T>& result) {
   if (result.wait_for(5s) != std::future_status::ready) {
      // Never unwind a fixture while its native coroutine/thread can still use it.
      std::cerr << "P2P host-event test owner failed to join within five seconds\n";
      std::terminate();
   }
   return result.get();
}

// The barrier only schedules test callers; it never intercepts a native read.
template <typename T>
struct concurrent_call {
   std::promise<void> ready;
   std::promise<void> release;
   std::shared_future<void> released{release.get_future().share()};
   std::future<T> result;
   std::thread worker;
   bool started = false;

   template <typename Callable>
   explicit concurrent_call(Callable operation) {
      auto task = std::packaged_task<T()>{[this, operation = std::move(operation)]() mutable {
         ready.set_value();
         released.get();
         return operation();
      }};
      result = task.get_future();
      worker = std::thread{std::move(task)};
   }

   ~concurrent_call() {
      release_once();
      if (result.valid() && result.wait_for(5s) != std::future_status::ready) {
         std::cerr << "P2P concurrent native read did not join\n";
         std::terminate();
      }
      if (worker.joinable()) {
         worker.join();
      }
   }

   void start() {
      auto entered = ready.get_future();
      joined(entered);
      release_once();
   }

   T finish() {
      auto value = joined(result);
      worker.join();
      return value;
   }

 private:
   void release_once() {
      if (!std::exchange(started, true)) {
         release.set_value();
      }
   }
};

struct plugin_environment {
   forge::asio::runtime runtime{};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry apis;
   forge::app::event_journal events{};
   forge::app::plugin_context context{scheduler, apis, events};

   ~plugin_environment() {
      run(scheduler.shutdown());
   }

   template <typename T>
   T run(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return joined(result);
   }
};

void check_repeated_config(plugin_environment& environment, node_plugin::plugin& node,
                           const forge::config::core::document& document) {
   const auto before = node.lifecycle().snapshot();
   BOOST_CHECK_THROW(environment.run(node.configure({document, "plugins.net.p2p.node"})),
                     forge::app::exceptions::invalid_state);
   const auto after = node.lifecycle().snapshot();
   BOOST_CHECK(after.phase == before.phase);
   BOOST_TEST(after.epoch == before.epoch);
   BOOST_TEST(after.generation == before.generation);
   BOOST_TEST(after.completed_operations == before.completed_operations);
   BOOST_TEST(after.admission_open == before.admission_open);
   BOOST_TEST(after.failure.has_value() == before.failure.has_value());
}

p2p::peer_id test_peer(std::uint8_t seed) {
   return p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
                           .data = std::vector<std::uint8_t>(32, seed)});
}

struct host_fixture {
   plugin_environment environment;
   node_plugin::plugin node;
   forge::config::core::document config;

   explicit host_fixture(bool authenticated = false) {
      using value = forge::config::core::value;
      // In-memory persistence remains test-only; secure fixtures use real TLS identities.
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      if (authenticated) {
         environment.apis.install<secrets::api>(std::make_shared<host_identity_source>());
         config.set("plugins.net.p2p.node.identity.certificate-secret", "host/cert");
         config.set("plugins.net.p2p.node.identity.private-key-secret", "host/key");
         config.set("plugins.net.p2p.node.listen", value::array_type{"/ip4/127.0.0.1/tcp/0"});
      } else {
         config.set("plugins.net.p2p.node.peer-id", test_peer(73).value);
      }
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      config.set("plugins.net.p2p.node.diagnostics.max-peers", 7U);
      config.set("plugins.net.p2p.node.dht.profiles", value::array_type{
          value{value::object_type{{"kind", "custom"}, {"mode", "client"}, {"protocol", routing_profile.value},
                                   {"peers", true}, {"providers", false}, {"values", false}}},
      });
      environment.run(node.configure({config, "plugins.net.p2p.node"}));
      auto provider = forge::api::core::installer{environment.apis};
      environment.run(node.provide(provider));
      environment.run(node.initialize(environment.context));
      environment.run(node.after_initialize());
   }

   ~host_fixture() {
      node.request_stop();
      environment.run(node.shutdown());
   }

   void start() {
      environment.run(node.startup());
   }

   auto facade() {
      return environment.apis.get<node_plugin::events_api>(
          {.id = {"forge.plugins.net.p2p.node.events"}, .major = 1, .min_revision = 0});
   }

   auto snapshots() {
      return environment.apis.get<node_plugin::diagnostics_api>(
          {.id = {"forge.plugins.net.p2p.node.diagnostics"}, .major = 1, .min_revision = 0});
   }

   void seed_authenticated_peers() {
      const auto api = environment.apis.get<node_plugin::api>(node_plugin::api::ref());
      const auto endpoint = api->local_endpoint();
      BOOST_REQUIRE(endpoint);
      const auto reads = snapshots();
      auto authenticated = std::vector<p2p::peer_id>{};
      // One client at a time shares the existing runtime; only authenticated records
      // persist in the target's native store after each joined disconnect.
      for (auto index = 0; index != 12; ++index) {
         auto identity = forge::tests::p2p::make_identity_fixture("host-diagnostics-peer-" + std::to_string(index));
         auto options = p2p::node::options{};
         options.certificate_pem = std::move(identity.certificate_pem);
         options.private_key_pem = std::move(identity.private_key_pem);
         options.allow_insecure_test_mode = true;
         options.limits.topology.operating_mode = p2p::topology::mode::static_only;
         auto peer = p2p::node{environment.runtime, std::move(options)};
         auto stopped = false;
         auto cleanup = boost::scope::scope_exit{[&] {
            if (!stopped) {
               peer.request_stop();
               environment.run(peer.async_stop());
            }
         }};
         environment.run(peer.async_start());
         const auto session = environment.run(peer.async_connect(*endpoint,
             {.expected_peer = api->local_peer(), .allow_relay = false, .timeout = 2s,
              .direct_attempt_timeout = 2s, .allow_hole_punch = false}));
         BOOST_CHECK(session.remote_peer == api->local_peer());
         BOOST_REQUIRE(environment.run(await_state([&] {
            const auto value = reads->snapshot({.max_peers = 16});
            return std::ranges::any_of(value.sessions, [&](const auto& current) {
               return current.remote_peer == peer.local_peer() && !current.closed &&
                   current.authentication == p2p::peer_authentication::libp2p_tls &&
                   current.muxer.value == "/yamux/1.0.0" &&
                   current.identify_state == p2p::identify::state::identified;
            }) && std::ranges::any_of(value.peers, [&](const auto& record) {
               return record.peer == peer.local_peer();
            });
         })));
         BOOST_REQUIRE(std::ranges::find(authenticated, peer.local_peer()) == authenticated.end());
         authenticated.push_back(peer.local_peer());
         peer.request_stop();
         environment.run(peer.async_stop());
         stopped = true;
         BOOST_REQUIRE(environment.run(await_state([&] {
            const auto value = reads->snapshot({.max_peers = 16});
            return value.connections.active_sessions == 0 &&
                std::ranges::none_of(value.sessions, [](const auto& current) { return !current.closed; });
         })));
      }
      const auto all = reads->snapshot({.max_peers = 16});
      BOOST_REQUIRE_EQUAL(all.peers.size(), 12U);
      BOOST_TEST(all.connections.active_sessions == 0U);
      for (const auto& peer : all.peers) {
         BOOST_CHECK(std::ranges::find(authenticated, peer.peer) != authenticated.end());
      }
   }
};

std::optional<p2p::host_event> read_result(boost::asio::io_context& io,
                                         p2p::host_event_subscription& subscription,
                                         std::future<std::optional<p2p::host_event>>& result) {
   io.restart();
   io.run_for(250ms);
   if (result.wait_for(1s) != std::future_status::ready) {
      subscription.close();
      io.restart();
      io.run_for(1s);
      (void)joined(result);
      BOOST_FAIL("native host-event read did not complete before the bounded read deadline");
   }
   return joined(result);
}

// A normal immutable native contribution makes shared plugin-state ownership observable.
struct lifetime_gater final : p2p::connection_gater {};

} // namespace

BOOST_AUTO_TEST_CASE(p2p_host_event_api_contracts_are_node_owned_local_and_reject_before_startup) {
   auto environment = plugin_environment{};
   auto node = node_plugin::plugin{};
   auto provider = forge::api::core::installer{environment.apis};
   environment.run(node.provide(provider));
   const auto events = environment.apis.get<node_plugin::events_api>(node_plugin::events_api::ref());
   const auto reads = environment.apis.get<node_plugin::diagnostics_api>(node_plugin::diagnostics_api::ref());
   BOOST_TEST(node_plugin::events_api::describe().id.value == "forge.plugins.net.p2p.node.events");
   BOOST_TEST(node_plugin::diagnostics_api::describe().id.value == "forge.plugins.net.p2p.node.diagnostics");
   for (const auto& descriptor : {node_plugin::events_api::describe(), node_plugin::diagnostics_api::describe()}) {
      BOOST_TEST(descriptor.version.major == 1U);
      BOOST_TEST(descriptor.version.revision == 0U);
      BOOST_TEST(descriptor.methods.empty());
      BOOST_CHECK(descriptor.supported_surfaces == forge::api::core::surface::local);
      BOOST_CHECK(environment.apis.describe({.id = descriptor.id, .major = 1, .min_revision = 0}) != nullptr);
      BOOST_CHECK(environment.apis.describe({.id = descriptor.id, .major = 2}) == nullptr);
      BOOST_CHECK(environment.apis.describe({.id = descriptor.id, .major = 3}) == nullptr);
   }
   for (const auto* retired : {"node.host_event_source", "node.diagnostics_source", "diagnostics.events", "diagnostics"}) {
      const auto id = std::string{"forge.plugins.net.p2p."} + retired;
      for (auto major = std::uint16_t{1}; major <= 3; ++major) {
         BOOST_CHECK(environment.apis.describe({.id = {id}, .major = major}) == nullptr);
      }
   }
   BOOST_CHECK_THROW((void)reads->snapshot(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)reads->snapshot({.max_peers = 3}), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)reads->routing_status(routing_profile), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)reads->pubsub_scores(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)events->reachability_status(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)events->host_events(), node_plugin::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_delivers_the_native_initial_snapshot) {
   auto fixture = host_fixture{};
   BOOST_CHECK_THROW((void)fixture.facade()->host_events(), node_plugin::exceptions::plugin_not_initialized);
   fixture.start();
   const auto before = fixture.facade()->reachability_status();
   auto first = fixture.facade()->host_events();
   auto second = fixture.facade()->host_events();
   const auto after = fixture.facade()->reachability_status();
   auto io = boost::asio::io_context{};
   for (auto* subscription : {&first, &second}) {
      auto result = boost::asio::co_spawn(io, subscription->async_read(), boost::asio::use_future);
      const auto initial = read_result(io, *subscription, result);
      BOOST_REQUIRE(initial.has_value());
      BOOST_TEST(initial->generation >= before.generation);
      BOOST_TEST(initial->generation <= after.generation);
      BOOST_CHECK(initial->phase == p2p::lifecycle_phase::maintenance);
      BOOST_CHECK(initial->effective == after.effective);
      BOOST_CHECK(initial->autonat_v1 == after.autonat_v1);
      BOOST_TEST(initial->autonat_v2.empty());
      BOOST_TEST(initial->confirmed_addresses.empty());
      BOOST_TEST(subscription->active());
   }
   const auto snapshot = fixture.snapshots()->snapshot();
   BOOST_CHECK(snapshot.reachability.host.phase == after.phase);
   BOOST_CHECK(snapshot.reachability.host.effective == after.effective);
   BOOST_TEST(snapshot.reachability.host.generation >= before.generation);
   fixture.node.request_stop();
   // Diagnostics retain the shared native owner until unified shutdown completes.
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->snapshot());
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->snapshot({.max_peers = 1}));
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->routing_status(routing_profile));
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->pubsub_scores());
   BOOST_CHECK_THROW((void)fixture.facade()->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)fixture.facade()->reachability_status(), p2p::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_unified_node_stop_wakes_pending_and_closes_late_reads) {
   auto fixture = host_fixture{};
   fixture.start();
   auto first = fixture.facade()->host_events();
   auto second = fixture.facade()->host_events();
   auto io = boost::asio::io_context{};
   for (auto* subscription : {&first, &second}) {
      auto initial = boost::asio::co_spawn(io, subscription->async_read(), boost::asio::use_future);
      BOOST_REQUIRE(read_result(io, *subscription, initial).has_value());
   }
   auto pending = boost::asio::co_spawn(io, first.async_read(), boost::asio::use_future);
   auto other = boost::asio::co_spawn(io, second.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(pending.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(other.wait_for(0ms) != std::future_status::ready);
   fixture.node.request_stop();
   BOOST_CHECK_THROW((void)fixture.facade()->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)fixture.facade()->reachability_status(), p2p::exceptions::canceled);
   BOOST_CHECK_NO_THROW((void)fixture.snapshots()->snapshot());
   fixture.environment.run(fixture.node.shutdown());
   BOOST_TEST(!read_result(io, first, pending).has_value());
   BOOST_TEST(!read_result(io, second, other).has_value());
   BOOST_TEST(!first.active());
   BOOST_TEST(!second.active());
   BOOST_CHECK_THROW((void)fixture.snapshots()->snapshot(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)fixture.snapshots()->routing_status(routing_profile),
                     node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)fixture.snapshots()->pubsub_scores(), node_plugin::exceptions::plugin_not_initialized);
   auto late = boost::asio::co_spawn(io, first.async_read(), boost::asio::use_future);
   BOOST_TEST(!read_result(io, first, late).has_value());
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_canceled_read_keeps_the_issued_subscription_open) {
   auto fixture = host_fixture{};
   fixture.start();
   auto subscription = fixture.facade()->host_events();
   auto independent = fixture.facade()->host_events();
   auto io = boost::asio::io_context{};
   for (auto* current : {&subscription, &independent}) {
      auto initial = boost::asio::co_spawn(io, current->async_read(), boost::asio::use_future);
      BOOST_REQUIRE(read_result(io, *current, initial).has_value());
   }
   auto cancellation = boost::asio::cancellation_signal{};
   auto canceled = boost::asio::co_spawn(io, subscription.async_read(),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   auto other = boost::asio::co_spawn(io, independent.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(canceled.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(other.wait_for(0ms) != std::future_status::ready);
   cancellation.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK_THROW(read_result(io, subscription, canceled), std::exception);
   BOOST_TEST(subscription.active());
   BOOST_TEST(independent.active());
   BOOST_CHECK(other.wait_for(0ms) != std::future_status::ready);
   auto next = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(next.wait_for(0ms) != std::future_status::ready);
   fixture.node.request_stop();
   fixture.environment.run(fixture.node.shutdown());
   BOOST_TEST(!read_result(io, subscription, next).has_value());
   BOOST_TEST(!read_result(io, independent, other).has_value());
   BOOST_TEST(!subscription.active());
   BOOST_TEST(!independent.active());
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_diagnostics_bounds_stay_frozen_with_independent_read_overrides) {
   auto fixture = host_fixture{true};
   fixture.start();
   fixture.seed_authenticated_peers();
   const auto reads = fixture.snapshots();
   const auto captured = reads->snapshot();
   BOOST_REQUIRE_EQUAL(captured.peers.size(), 7U);
   BOOST_TEST(reads->snapshot({.max_peers = 3}).peers.size() == 3U);

   auto invalid = fixture.config;
   invalid.set("plugins.net.p2p.node.diagnostics.max-peers", 9U);
   invalid.set("plugins.net.p2p.node.diagnostics.max-sessions", 0U);
   {
      auto fresh = node_plugin::plugin{};
      BOOST_CHECK_THROW(fixture.environment.run(fresh.configure({invalid, "plugins.net.p2p.node"})),
                        node_plugin::exceptions::invalid_config);
   }
   check_repeated_config(fixture.environment, fixture.node, invalid);
   BOOST_TEST(reads->snapshot().peers.size() == 7U);

   auto mixed = fixture.config;
   mixed.set("plugins.net.p2p.node.diagnostics.max-peers", 9U);
   mixed.set("plugins.net.p2p.node.peer-id", test_peer(74).value);
   check_repeated_config(fixture.environment, fixture.node, mixed);
   BOOST_TEST(reads->snapshot().peers.size() == 7U);
   BOOST_CHECK(reads->network().local_peer == captured.network.local_peer);

   fixture.config.set("plugins.net.p2p.node.diagnostics.max-peers", 9U);
   check_repeated_config(fixture.environment, fixture.node, fixture.config);
   BOOST_TEST(reads->snapshot().peers.size() == 7U);
   BOOST_TEST(reads->snapshot({.max_peers = 9}).peers.size() == 9U);
   BOOST_TEST(reads->snapshot({.max_peers = 3}).peers.size() == 3U);
   BOOST_TEST(captured.peers.size() == 7U);
   BOOST_CHECK(reads->network().local_peer == captured.network.local_peer);
   BOOST_CHECK(fixture.facade()->reachability_status().phase == p2p::lifecycle_phase::maintenance);
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_native_reads_race_rejected_reconfiguration) {
   auto fixture = host_fixture{true};
   fixture.start();
   fixture.seed_authenticated_peers();
   // Removed source-callback barriers cannot stall concrete native reads. Exercise
   // real concurrent calls instead; this is not a post-capture pause injection.
   struct observations {
      std::vector<std::size_t> configured;
      bool explicit_bounds = true;
      bool native_empty_state = true;
   };
   auto reader = concurrent_call<observations>{[reads = fixture.snapshots().shared()] {
      auto out = observations{};
      for (auto index = 0; index != 64; ++index) {
         out.configured.push_back(reads->snapshot().peers.size());
         out.explicit_bounds = out.explicit_bounds && reads->snapshot({.max_peers = 3}).peers.size() == 3;
         const auto routing = reads->routing_status(routing_profile);
         const auto scores = reads->pubsub_scores();
         out.native_empty_state = out.native_empty_state && routing.active == 0 && routing.nonempty_buckets == 0 &&
                                  scores.peers.empty() && scores.connected_peers == 0;
      }
      return out;
   }};
   reader.start();
   for (auto index = 0; index != 16; ++index) {
      fixture.config.set("plugins.net.p2p.node.diagnostics.max-peers", index % 2 == 0 ? 9U : 7U);
      check_repeated_config(fixture.environment, fixture.node, fixture.config);
   }
   const auto observed = reader.finish();
   BOOST_REQUIRE_EQUAL(observed.configured.size(), 64U);
   for (const auto size : observed.configured) {
      BOOST_TEST(size == 7U);
   }
   BOOST_TEST(observed.explicit_bounds);
   BOOST_TEST(observed.native_empty_state);
   BOOST_TEST(fixture.snapshots()->snapshot().peers.size() == 7U);
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_native_reads_race_unified_shutdown_with_owned_handles) {
   auto fixture = host_fixture{};
   fixture.start();
   auto reads = fixture.snapshots().shared();
   auto events = fixture.facade().shared();
   const auto weak_reads = std::weak_ptr<node_plugin::diagnostics_api>{reads};
   const auto weak_events = std::weak_ptr<node_plugin::events_api>{events};
   auto subscription = events->host_events();
   auto io = boost::asio::io_context{};
   auto initial = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(read_result(io, subscription, initial).has_value());
   auto pending = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(pending.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK_NO_THROW((void)reads->routing_status(routing_profile));
   BOOST_TEST(reads->pubsub_scores().peers.empty());

   struct observations {
      std::size_t completed = 0;
      std::size_t closed = 0;
      bool native_state = true;
   };
   auto reader = concurrent_call<observations>{[reads, events] {
      auto out = observations{};
      for (auto index = 0; index != 64; ++index) {
         try {
            switch (index % 4) {
               case 0: out.native_state = out.native_state && reads->snapshot().network.local_peer == test_peer(73); break;
               case 1: out.native_state = out.native_state && reads->snapshot({.max_peers = 3}).peers.empty(); break;
               case 2: out.native_state = out.native_state && reads->routing_status(routing_profile).active == 0; break;
               case 3: out.native_state = out.native_state && reads->pubsub_scores().peers.empty(); break;
            }
            ++out.completed;
         } catch (const node_plugin::exceptions::plugin_not_initialized&) {
            ++out.closed;
         }
         try {
            auto issued = events->host_events();
         } catch (const p2p::exceptions::canceled&) {
         }
      }
      return out;
   }};
   fixture.environment.apis.clear();
   reads.reset();
   events.reset();
   BOOST_TEST(!weak_reads.expired());
   BOOST_TEST(!weak_events.expired());
   reader.start();
   fixture.node.request_stop();
   fixture.environment.run(fixture.node.shutdown());
   const auto observed = reader.finish();
   BOOST_TEST(observed.completed + observed.closed == 64U);
   BOOST_TEST(observed.native_state);
   BOOST_TEST(weak_reads.expired());
   BOOST_TEST(weak_events.expired());
   BOOST_TEST(!read_result(io, subscription, pending).has_value());
   BOOST_TEST(!subscription.active());
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_stop_before_initialize_cannot_reopen_admission) {
   auto environment = plugin_environment{};
   auto node = node_plugin::plugin{};
   auto provider = forge::api::core::installer{environment.apis};
   environment.run(node.provide(provider));
   const auto events = environment.apis.get<node_plugin::events_api>(node_plugin::events_api::ref());
   const auto reads = environment.apis.get<node_plugin::diagnostics_api>(node_plugin::diagnostics_api::ref());
   node.request_stop();
   BOOST_CHECK_THROW(environment.run(node.initialize(environment.context)), forge::app::exceptions::invalid_state);
   BOOST_CHECK_THROW(environment.run(node.startup()), forge::app::exceptions::invalid_state);
   const auto stopped = node.lifecycle().snapshot();
   BOOST_CHECK(stopped.phase == forge::app::lifecycle_phase::stopping);
   BOOST_TEST(stopped.stop_requested);
   BOOST_TEST(!stopped.admission_open);
   BOOST_TEST(!stopped.completed(forge::app::lifecycle_operation::initialize));
   BOOST_TEST(!stopped.completed(forge::app::lifecycle_operation::startup));
   BOOST_CHECK_THROW((void)events->reachability_status(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)events->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)reads->snapshot(), node_plugin::exceptions::plugin_not_initialized);
   environment.run(node.shutdown());
   BOOST_CHECK(node.lifecycle().snapshot().phase == forge::app::lifecycle_phase::stopped);
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_issued_subscription_survives_registry_and_facade_release) {
   auto fixture = host_fixture{};
   fixture.start();
   auto events = fixture.facade().shared();
   const auto weak_events = std::weak_ptr<node_plugin::events_api>{events};
   auto subscription = events->host_events();
   auto deferred = subscription.async_read();
   fixture.environment.apis.clear();
   events.reset();
   BOOST_TEST(weak_events.expired());
   BOOST_TEST(subscription.active());
   auto io = boost::asio::io_context{};
   auto initial = boost::asio::co_spawn(io, std::move(deferred), boost::asio::use_future);
   const auto state = read_result(io, subscription, initial);
   BOOST_REQUIRE(state.has_value());
   BOOST_CHECK(state->phase == p2p::lifecycle_phase::maintenance);
   auto pending = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(pending.wait_for(0ms) != std::future_status::ready);
   fixture.node.request_stop();
   fixture.environment.run(fixture.node.shutdown());
   BOOST_TEST(!read_result(io, subscription, pending).has_value());
   BOOST_TEST(!subscription.active());
}

BOOST_AUTO_TEST_CASE(p2p_host_event_api_late_handles_and_cold_native_read_survive_plugin_destruction) {
   auto events = std::shared_ptr<node_plugin::events_api>{};
   auto reads = std::shared_ptr<node_plugin::diagnostics_api>{};
   auto weak_owner = std::weak_ptr<lifetime_gater>{};
   auto subscription = p2p::host_event_subscription{};
   auto deferred = std::optional<boost::asio::awaitable<std::optional<p2p::host_event>>>{};
   {
      auto fixture = host_fixture{};
      auto contribution = std::make_shared<lifetime_gater>();
      weak_owner = contribution;
      fixture.environment.apis.get<node_plugin::configuration_api>(node_plugin::configuration_api::ref())
          ->set_connection_gater(contribution);
      contribution.reset();
      fixture.start();
      events = fixture.facade().shared();
      reads = fixture.snapshots().shared();
      subscription = events->host_events();
      deferred.emplace(subscription.async_read());
   }
   BOOST_TEST(!weak_owner.expired());
   BOOST_CHECK_THROW((void)events->host_events(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)events->reachability_status(), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)reads->snapshot(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)reads->routing_status(routing_profile), node_plugin::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)reads->pubsub_scores(), node_plugin::exceptions::plugin_not_initialized);
   BOOST_TEST(!subscription.active());
   events.reset();
   BOOST_TEST(!weak_owner.expired());
   reads.reset();
   subscription = p2p::host_event_subscription{};
   auto io = boost::asio::io_context{};
   auto result = boost::asio::co_spawn(io, std::move(*deferred), boost::asio::use_future);
   BOOST_TEST(!read_result(io, subscription, result).has_value());
   deferred.reset();
   BOOST_TEST(weak_owner.expired());
}
