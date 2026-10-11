module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.runtime;
import forge.asio.task;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;

#include "../../plugins/net/p2p/node/details/resource_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
namespace secrets = forge::plugins::crypto::secrets;
using namespace std::chrono_literals;
using limits = p2p::resource_manager::limits;
using scope_limits = p2p::resource_manager::scope_limits;
using patch = np::resource_config::scope_patch;
using value = forge::config::core::value;
constexpr std::string_view section = "plugins.net.p2p.node";

struct scope_field {
   const char* name;
   patch np::resource_config::* input;
   scope_limits limits::* output;
};
constexpr auto scopes = std::array{
   scope_field{"system", &np::resource_config::system, &limits::system},
   scope_field{"transient", &np::resource_config::transient, &limits::transient},
   scope_field{"peer", &np::resource_config::peer, &limits::peer},
   scope_field{"protocol", &np::resource_config::protocol, &limits::protocol},
   scope_field{"service", &np::resource_config::service, &limits::service},
   scope_field{"protocol-peer", &np::resource_config::protocol_peer, &limits::protocol_peer},
   scope_field{"service-peer", &np::resource_config::service_peer, &limits::service_peer},
   scope_field{"connection", &np::resource_config::connection, &limits::connection},
   scope_field{"stream", &np::resource_config::stream, &limits::stream},
};
struct budget_field {
   const char* name;
   std::optional<std::uint64_t> patch::* input;
   std::uint64_t (*read)(const scope_limits&);
};
constexpr auto budgets = std::array{
   budget_field{"max-memory", &patch::max_memory, [](const scope_limits& v) -> std::uint64_t { return v.max_memory; }},
   budget_field{"max-file-descriptors", &patch::max_file_descriptors, [](const scope_limits& v) -> std::uint64_t { return v.max_file_descriptors; }},
   budget_field{"max-inbound-connections", &patch::max_inbound_connections, [](const scope_limits& v) -> std::uint64_t { return v.max_inbound_connections; }},
   budget_field{"max-outbound-connections", &patch::max_outbound_connections, [](const scope_limits& v) -> std::uint64_t { return v.max_outbound_connections; }},
   budget_field{"max-connections", &patch::max_connections, [](const scope_limits& v) -> std::uint64_t { return v.max_connections; }},
   budget_field{"max-inbound-streams", &patch::max_inbound_streams, [](const scope_limits& v) -> std::uint64_t { return v.max_inbound_streams; }},
   budget_field{"max-outbound-streams", &patch::max_outbound_streams, [](const scope_limits& v) -> std::uint64_t { return v.max_outbound_streams; }},
   budget_field{"max-streams", &patch::max_streams, [](const scope_limits& v) -> std::uint64_t { return v.max_streams; }},
};
struct counter_field {
   const char* name;
   std::optional<std::uint64_t> np::resource_config::* input;
   std::size_t limits::* output;
};
constexpr auto counters = std::array{
   counter_field{"max-dial-attempts", &np::resource_config::max_dial_attempts, &limits::max_dial_attempts},
   counter_field{"max-dial-attempts-per-peer", &np::resource_config::max_dial_attempts_per_peer, &limits::max_dial_attempts_per_peer},
   counter_field{"max-relay-reservations", &np::resource_config::max_relay_reservations, &limits::max_relay_reservations},
   counter_field{"max-malformed-messages-per-peer", &np::resource_config::max_malformed_messages_per_peer, &limits::max_malformed_messages_per_peer},
};

void check_limits(const limits& actual, const limits& expected) {
   for (const auto& scope : scopes) {
      for (const auto& field : budgets) {
         BOOST_TEST_CONTEXT(scope.name << "." << field.name) {
            BOOST_TEST(field.read(actual.*scope.output) == field.read(expected.*scope.output));
         }
      }
   }
   for (const auto& field : counters) { BOOST_TEST((actual.*field.output) == (expected.*field.output)); }
}

template<class T> T joined(std::future<T>& result) {
   if (result.wait_for(10s) != std::future_status::ready) {
      std::cerr << "resource fixture exceeded its owned join budget\n";
      std::terminate();
   }
   return result.get();
}

class identity_source final : public secrets::api {
 public:
   forge::tests::p2p::identity_fixture identity = forge::tests::p2p::make_identity_fixture("resource-plugin");
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override { co_return secrets::snapshot{.configured_secrets = 2}; }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      const auto* bytes = request.secret_id == "resources/cert" ? &identity.certificate_pem
          : request.secret_id == "resources/key" ? &identity.private_key_pem : nullptr;
      if (!bytes) { throw std::invalid_argument{"unknown resource fixture secret"}; }
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

struct fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry apis;
   forge::app::signal_bus signals;
   forge::app::event_bus events{};
   forge::app::plugin_context context{scheduler, apis, signals, events};
   np::plugin node;
   forge::config::core::document document;
   std::shared_ptr<np::api> host;
   std::shared_ptr<np::diagnostics_api> diagnostics;

   fixture() {
      // Explicit memory persistence, but real Secrets identity and TLS/Identify.
      document.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      document.set("plugins.net.p2p.node.identity.certificate-secret", "resources/cert");
      document.set("plugins.net.p2p.node.identity.private-key-secret", "resources/key");
      document.set("plugins.net.p2p.node.topology.mode", "static-only");
      configure();
      apis.install<secrets::api>(secrets::api::describe(), std::make_shared<identity_source>());
      auto installer = forge::api::core::installer{apis};
      run(node.provide(installer));
      host = apis.get<np::api>(np::api::ref()).shared();
      diagnostics = apis.get<np::diagnostics_api>(np::diagnostics_api::ref()).shared();
   }
   ~fixture() {
      node.request_stop();
      auto node_failure = std::exception_ptr{};
      auto scheduler_failure = std::exception_ptr{};
      try { run(node.shutdown()); }
      catch (...) { node_failure = std::current_exception(); }
      // Always join the scheduler before reporting either cleanup failure.
      try { run(scheduler.shutdown()); }
      catch (...) { scheduler_failure = std::current_exception(); }
      for (const auto& failure : {node_failure, scheduler_failure}) {
         if (!failure) { continue; }
         try { std::rethrow_exception(failure); }
         catch (const std::exception& error) { BOOST_ERROR("resource fixture cleanup: " << error.what()); }
         catch (...) { BOOST_ERROR("resource fixture cleanup: non-standard exception"); }
      }
   }
   template<class T> T run(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return joined(result);
   }
   void configure() { run(node.configure({document, std::string{section}})); }
   void start() { run(node.initialize(context)); run(node.after_initialize()); run(node.startup()); }
};

boost::asio::awaitable<bool> await_peer_records(std::shared_ptr<np::diagnostics_api> reads,
                                              std::size_t count) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   do {
      // Explicit bounds inspect the native state independently of configured bounds.
      if (reads->snapshot({.max_peers = 16}).peers.size() >= count) { co_return true; }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   co_return false;
}

template<class Error> void check_error(int expected) {
   const auto error = Error{"category probe"};
   const auto original = np::exceptions::plugin_not_initialized{"original category probe"};
   BOOST_TEST(error.code().value() == expected);
   BOOST_TEST(std::string{error.code().category().name()} == "forge.plugins.net.p2p.node");
   BOOST_CHECK(&error.code().category() == &original.code().category());
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_resource_config_single_error_category_preserves_original_numbers) {
   check_error<np::exceptions::plugin_not_initialized>(1);
   check_error<np::exceptions::route_conflict>(2);
   check_error<np::exceptions::invalid_config>(3);
   check_error<np::exceptions::topic_not_allowed>(4);
   check_error<np::exceptions::subscription_not_found>(5);
   check_error<np::exceptions::handler_limit>(6);
   check_error<np::exceptions::message_too_large>(7);
   check_error<np::exceptions::not_found>(8);
}

BOOST_AUTO_TEST_CASE(p2p_resource_config_all_scopes_and_dimensions_preserve_sparse_defaults_and_zero) {
   auto defaults = limits{};
   np::detail::apply_resource_config(defaults, {});
   check_limits(defaults, limits{});
   for (const auto& scope : scopes) {
      for (const auto& field : budgets) {
         for (const auto number : {std::uint64_t{0}, std::uint64_t{123}}) {
            BOOST_TEST_CONTEXT(scope.name << "." << field.name << "=" << number) {
               auto document = forge::config::core::document{};
               document.set(std::string{scope.name} + "." + field.name, number);
               const auto decoded = forge::config::core::decode<np::resource_config>(document);
               BOOST_REQUIRE(decoded.ok());
               const auto& input = (decoded.value.*scope.input).*field.input;
               BOOST_REQUIRE(input.has_value());
               BOOST_TEST(*input == number);
               auto actual = limits{};
               np::detail::apply_resource_config(actual, decoded.value);
               const auto native = limits{};
               for (const auto& other_scope : scopes) {
                  for (const auto& other_field : budgets) {
                     const auto selected = scope.input == other_scope.input && field.input == other_field.input;
                     BOOST_TEST(other_field.read(actual.*other_scope.output) ==
                                (selected ? number : other_field.read(native.*other_scope.output)));
                  }
               }
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_resource_config_operational_counters_are_sparse_and_checked) {
   for (const auto& field : counters) {
      auto document = forge::config::core::document{};
      document.set(field.name, std::uint64_t{17});
      const auto decoded = forge::config::core::decode<np::resource_config>(document);
      BOOST_REQUIRE(decoded.ok());
      auto actual = limits{};
      np::detail::apply_resource_config(actual, decoded.value);
      auto expected = limits{};
      expected.*field.output = 17;
      check_limits(actual, expected);
      fixture f;
      const auto prior = f.document;
      f.document.set("plugins.net.p2p.node.resources." + std::string{field.name}, std::uint64_t{0});
      BOOST_CHECK_THROW(f.configure(), np::exceptions::invalid_config);
      f.document = prior;
      f.start();
      auto native = limits{};
      native.system.max_connections = native.system.max_inbound_connections = native.system.max_outbound_connections = 1025;
      check_limits(f.diagnostics->snapshot().effective_limits, native);
   }
}

BOOST_AUTO_TEST_CASE(p2p_resource_config_yaml_sparse_empty_and_null_budget_use_the_plugin_decode_path) {
   fixture f;
   const auto parsed = forge::codec::yaml::read_document(R"(
plugins:
  net:
    p2p:
      node:
        enabled: true
        max-sessions: 7
        resources:
          system:
            max-outbound-connections: 0
            max-memory: null
          peer: {}
          max-dial-attempts: 17
outside-node:
  resorces: ignored
)");
   BOOST_REQUIRE(parsed.ok());
   for (const auto& [name, setting] : parsed.value.root) { f.document.set(name, setting); }
   // Restore the fixture's identity/persistence declarations overwritten by plugins.
   f.document.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
   f.document.set("plugins.net.p2p.node.identity.certificate-secret", "resources/cert");
   f.document.set("plugins.net.p2p.node.identity.private-key-secret", "resources/key");
   f.document.set("plugins.net.p2p.node.topology.mode", "static-only");
   f.configure();
   f.start();
   auto expected = limits{};
   expected.system.max_connections = expected.system.max_inbound_connections = 8;
   expected.system.max_outbound_connections = 0;
   expected.max_dial_attempts = 17;
   check_limits(f.diagnostics->snapshot().effective_limits, expected);
}

BOOST_AUTO_TEST_CASE(p2p_resource_config_invalid_shapes_unknowns_and_overflow_rollback_before_mutation) {
   fixture f;
   f.document.set("plugins.net.p2p.node.max-sessions", std::uint64_t{7});
   f.document.set("plugins.net.p2p.node.resources.system.max-connections", std::uint64_t{3});
   f.configure();
   const auto valid = f.document;
   const auto reject = [&](const std::string& path, value input) {
      f.document = valid;
      f.document.set(path, std::move(input));
      BOOST_TEST_CONTEXT(path) { BOOST_CHECK_THROW(f.configure(), np::exceptions::invalid_config); }
   };
   for (const auto& scope : scopes) {
      for (const auto& field : budgets) {
         const auto path = "plugins.net.p2p.node.resources." + std::string{scope.name} + "." + field.name;
         reject(path, std::int64_t{-1});
         reject(path, 1.5);
         reject(path, "18446744073709551616");
      }
   }
   for (const auto& field : counters) {
      const auto path = "plugins.net.p2p.node.resources." + std::string{field.name};
      reject(path, std::int64_t{-1}); reject(path, 1.5); reject(path, "18446744073709551616");
   }
   reject("plugins.net.p2p.node.resorces", value::object_type{});
   reject("plugins.net.p2p.node.resources.system.bad-limit", std::uint64_t{1});
   reject("plugins.net.p2p.node.enabled-extra", true);
   reject("plugins.net.p2p.node.resources.system", value{});
   reject("plugins.net.p2p.node.resources.peer", "not-a-scope");
   const auto bad_yaml = forge::codec::yaml::read_document("plugins:\n  net:\n    p2p:\n      node:\n        resorces: {}\n");
   BOOST_REQUIRE(bad_yaml.ok());
   const auto* typo = bad_yaml.value.object_at(section);
   BOOST_REQUIRE(typo);
   BOOST_REQUIRE_EQUAL(typo->size(), 1U);
   f.document = valid;
   for (const auto& [name, setting] : *typo) {
      f.document.set(std::string{section} + "." + name, setting);
   }
   // The valid identity, persistence and policy remain intact; only the typo differs.
   BOOST_CHECK_THROW(f.configure(), np::exceptions::invalid_config);
   f.start(); // No successful configure after the rejected candidates.
   auto expected = limits{};
   expected.system.max_connections = 3;
   expected.system.max_inbound_connections = expected.system.max_outbound_connections = 8;
   check_limits(f.diagnostics->snapshot().effective_limits, expected);
}

BOOST_AUTO_TEST_CASE(p2p_resource_config_largest_representable_values_and_native_size_overflow) {
   auto config = np::resource_config{};
   const auto largest = (std::numeric_limits<std::uint64_t>::max)();
   config.system.max_memory = largest;
   config.system.max_connections = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
   auto actual = limits{};
   np::detail::apply_resource_config(actual, config);
   BOOST_TEST(actual.system.max_memory == largest);
   BOOST_TEST(actual.system.max_connections == (std::numeric_limits<std::size_t>::max)());
   // This overflow branch is relevant only to a narrower native size_t target.
   if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
      const auto previous = actual;
      config.system.max_memory = 11;
      config.system.max_connections = largest;
      BOOST_CHECK_THROW(np::detail::apply_resource_config(actual, config), np::exceptions::invalid_config);
      check_limits(actual, previous);
   }
}

BOOST_AUTO_TEST_CASE(p2p_resource_config_frozen_updates_and_mixed_diagnostics_rollback) {
   fixture f;
   f.document.set("plugins.net.p2p.node.resources.system.max-connections", std::uint64_t{3});
   f.document.set("plugins.net.p2p.node.diagnostics.max-peers", std::uint64_t{1});
   f.document.set("plugins.net.p2p.node.listen", value::array_type{"/ip4/127.0.0.1/tcp/0"});
   f.configure(); f.start();
   const auto endpoint = f.host->local_endpoint();
   BOOST_REQUIRE(endpoint);
   auto clients = std::vector<std::unique_ptr<fixture>>{};
   for (auto index = 0; index != 3; ++index) {
      auto peer = std::make_unique<fixture>();
      peer->start();
      const auto connected = peer->run(peer->host->connect(*endpoint,
          {.expected_peer = f.host->local_peer(), .allow_relay = false, .timeout = 3s,
           .direct_attempt_timeout = 2s, .allow_hole_punch = false}));
      BOOST_REQUIRE(connected.remote_peer == f.host->local_peer());
      BOOST_REQUIRE(connected.identify_state == p2p::identify::state::identified);
      clients.push_back(std::move(peer));
   }
   BOOST_REQUIRE(f.run(await_peer_records(f.diagnostics, 3)));
   BOOST_REQUIRE_EQUAL(f.diagnostics->snapshot().peers.size(), 1U);
   const auto running = f.document;
   f.document.set("plugins.net.p2p.node.resources.system.max-connections", std::uint64_t{4});
   f.document.set("plugins.net.p2p.node.diagnostics.max-peers", std::uint64_t{2});
   BOOST_CHECK_THROW(f.configure(), np::exceptions::route_conflict);
   BOOST_TEST(f.diagnostics->snapshot().effective_limits.system.max_connections == 3U);
   BOOST_TEST(f.diagnostics->snapshot().peers.size() == 1U);
   BOOST_TEST(f.diagnostics->snapshot({.max_peers = 16}).peers.size() >= 3U);
   f.document = running;
   f.document.set("plugins.net.p2p.node.diagnostics.max-peers", std::uint64_t{2});
   BOOST_CHECK_NO_THROW(f.configure());
   BOOST_TEST(f.diagnostics->snapshot().peers.size() == 2U);
   f.node.request_stop();
   BOOST_CHECK_THROW(f.configure(), np::exceptions::route_conflict);
   f.run(f.node.shutdown());
   BOOST_CHECK_THROW(f.configure(), np::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_resource_config_real_connection_denial_keeps_native_code_and_releases_reservations) {
   fixture server;
   server.document.set("plugins.net.p2p.node.listen", value::array_type{"/ip4/127.0.0.1/tcp/0"});
   server.configure(); server.start();
   const auto endpoint = server.host->local_endpoint();
   BOOST_REQUIRE(endpoint);
   const auto options = p2p::node::connect_options{
       .expected_peer = server.host->local_peer(), .allow_relay = false, .timeout = 3s,
       .direct_attempt_timeout = 2s, .allow_hole_punch = false};
   fixture allowed;
   allowed.start();
   const auto connected = allowed.run(allowed.host->connect(*endpoint, options));
   BOOST_REQUIRE(connected.remote_peer == server.host->local_peer());
   BOOST_REQUIRE(connected.identify_state == p2p::identify::state::identified);
   BOOST_REQUIRE(connected.muxer.value == "/yamux/1.0.0");
   const auto authenticated = allowed.diagnostics->snapshot();
   BOOST_REQUIRE(std::ranges::any_of(authenticated.sessions, [&](const auto& session) {
      return session.id == connected.id && session.authentication == p2p::peer_authentication::libp2p_tls;
   }));
   fixture denied;
   denied.document.set("plugins.net.p2p.node.resources.system.max-outbound-connections", std::uint64_t{0});
   denied.configure(); denied.start();
   const auto before = denied.diagnostics->snapshot();
   BOOST_CHECK_EXCEPTION(denied.run(denied.host->connect(*endpoint, options)), forge::exceptions::base,
       [](const auto& error) { return p2p::exceptions::is(error, p2p::exceptions::code::backpressure_rejected); });
   const auto after = denied.diagnostics->snapshot();
   BOOST_TEST(after.resources.denied_connections == before.resources.denied_connections + 1);
   BOOST_TEST(after.connections.active_sessions == 0U);
   BOOST_TEST(after.resources.active_dials == before.resources.active_dials);
   BOOST_TEST(after.resources.dial_attempt_scopes == before.resources.dial_attempt_scopes);
   BOOST_TEST(after.resources.active_peer_scopes == before.resources.active_peer_scopes);
   BOOST_TEST(after.resources.system.inbound_connections == before.resources.system.inbound_connections);
   BOOST_TEST(after.resources.system.outbound_connections == before.resources.system.outbound_connections);
   BOOST_TEST(after.resources.system.file_descriptors == before.resources.system.file_descriptors);
   BOOST_TEST(after.resources.system.memory == before.resources.system.memory);
   BOOST_TEST(after.resources.transient.outbound_connections == before.resources.transient.outbound_connections);
   BOOST_TEST(after.resources.transient.file_descriptors == before.resources.transient.file_descriptors);
   BOOST_TEST(after.resources.transient.memory == before.resources.transient.memory);
}
