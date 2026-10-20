#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <functional>
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

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.api.p2p.publication;
import forge.api.transport.connection;
import forge.app.events;
import forge.app.exceptions;
import forge.app.plugin_context;
import forge.app.plugin_lifecycle;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.stream;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.pubsub_api;

namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;
using namespace std::chrono_literals;

template<class T> T joined(std::future<T>& value) {
   if (value.wait_for(10s) != std::future_status::ready) {
      std::cerr << "protocol fixture did not join its native operation\n";
      std::terminate();
   }
   return value.get();
}

class identity_source final : public secrets::api {
 public:
   bool reject_reads = false;
   forge::tests::p2p::identity_fixture identity = forge::tests::p2p::make_identity_fixture("raw-plugin");
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override { co_return secrets::snapshot{.configured_secrets = 2}; }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      if (reject_reads) { throw std::runtime_error{"raw fixture identity read failed"}; }
      const auto* bytes = request.secret_id == "raw/cert" ? &identity.certificate_pem
          : request.secret_id == "raw/key" ? &identity.private_key_pem : nullptr;
      if (!bytes) { throw std::invalid_argument{"unknown raw fixture secret"}; }
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
   forge::asio::runtime runtime{};
   forge::asio::task::scheduler scheduler{runtime};
   forge::api::core::registry apis;
   forge::app::event_journal events{};
   forge::app::plugin_context context{scheduler, apis, events};
   np::plugin node;
   std::shared_ptr<identity_source> identity = std::make_shared<identity_source>();
   std::shared_ptr<np::protocol_api> raw;
   std::shared_ptr<np::api> api;
   forge::config::core::document configuration;

   explicit fixture(std::optional<p2p::endpoint> bootstrap = {}, unsigned limit = 32, bool pubsub = false,
                    std::function<void(forge::config::core::document&)> prepare_configuration = {}) {
      auto& config = configuration;
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.identity.certificate-secret", "raw/cert");
      config.set("plugins.net.p2p.node.identity.private-key-secret", "raw/key");
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{"/ip4/127.0.0.1/tcp/0"});
      config.set("plugins.net.p2p.node.max-protocol-handlers", static_cast<std::uint64_t>(limit));
      config.set("plugins.net.p2p.node.pubsub.enabled", pubsub);
      if (bootstrap) {
         config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{bootstrap->to_string()});
         config.set("plugins.net.p2p.node.bootstrap-policy.requirement", "require-connection");
      }
      if (prepare_configuration) { prepare_configuration(config); }
      run(node.configure({config, "plugins.net.p2p.node"}));
      apis.install<secrets::api>(secrets::api::describe(), identity);
      auto installer = forge::api::core::installer{apis};
      run(node.provide(installer));
      run(node.initialize(context));
      run(node.after_initialize());
      raw = apis.get<np::protocol_api>({.id = {"forge.plugins.net.p2p.node.protocol"}, .major = 1}).shared();
      api = apis.get<np::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 3}).shared();
   }
   ~fixture() {
      node.request_stop();
      run(node.shutdown());
      run(scheduler.shutdown());
   }
   template<class T> T run(boost::asio::awaitable<T> operation) {
      auto value = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return joined(value);
   }
   auto publish(p2p::protocol_id protocol) {
      return api->publish_api(forge::api::core::binding().serve(apis).build(), std::move(protocol));
   }
};

void check_repeated_config(fixture& owner, const forge::config::core::document& document) {
   const auto before = owner.node.lifecycle().snapshot();
   BOOST_REQUIRE(before.completed(forge::app::lifecycle_operation::configure));
   BOOST_CHECK_THROW(owner.run(owner.node.configure({document, "plugins.net.p2p.node"})),
                     forge::app::exceptions::invalid_state);
   const auto after = owner.node.lifecycle().snapshot();
   BOOST_CHECK(after.phase == before.phase);
   BOOST_TEST(after.epoch == before.epoch);
   BOOST_TEST(after.generation == before.generation);
   BOOST_TEST(after.completed_operations == before.completed_operations);
   BOOST_TEST(after.admission_open == before.admission_open);
   BOOST_TEST(after.failure.has_value() == before.failure.has_value());
}

p2p::node::protocol_handler noop() {
   return [](p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; };
}
forge::config::core::value custom_dht(const p2p::protocol_id& protocol) {
   using value = forge::config::core::value;
   return value{value::object_type{{"kind", "custom"}, {"mode", "server"}, {"protocol", protocol.value},
                                   {"peers", true}, {"providers", false}, {"values", false}}};
}
p2p::node::protocol_handler echo(std::uint8_t tag) {
   return [tag](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
      for (;;) {
         auto bytes = co_await incoming.stream.async_read_frame();
         if (bytes.empty()) { break; }
         bytes.push_back(tag);
         co_await incoming.stream.async_write_frame(bytes);
      }
      co_await incoming.stream.async_close();
   };
}
boost::asio::awaitable<std::vector<std::uint8_t>> exchange(p2p::stream& stream) {
   const auto bytes = std::vector<std::uint8_t>{42};
   co_await stream.async_write_frame(bytes);
   co_return co_await stream.async_read_frame();
}
struct capture {
   std::function<void()> retired;
   ~capture() { retired(); }
};
}

BOOST_AUTO_TEST_CASE(p2p_protocol_contract_and_exact_generation_ownership) {
   fixture first, foreign;
   BOOST_CHECK(np::protocol_api::describe().supported_surfaces == forge::api::core::surface::local);
   BOOST_TEST(np::protocol_api::describe().methods.empty());
   BOOST_TEST(np::api::describe().version.major == 3U);
   const auto name = p2p::protocol_id{"/raw/owner/1"};
   auto old = first.raw->register_protocol(name, noop());
   auto elsewhere = foreign.raw->register_protocol(name, noop());
   BOOST_CHECK_THROW((void)first.raw->register_protocol(name, noop()), np::exceptions::route_conflict);
   BOOST_CHECK_THROW((void)first.raw->replace_protocol(elsewhere, name, noop()), np::exceptions::route_conflict);
   auto newer = first.raw->replace_protocol(old, name, noop());
   BOOST_TEST(!old.active());
   BOOST_TEST(newer.active());
   old.close();
   BOOST_TEST(newer.active());
   BOOST_CHECK_THROW((void)first.raw->replace_protocol(old, name, noop()), np::exceptions::route_conflict);
   auto moved = std::move(newer);
   BOOST_TEST(!newer.active());
   moved.close();
   BOOST_TEST(!moved.active());
   BOOST_CHECK_THROW((void)first.raw->replace_protocol(moved, name, noop()), np::exceptions::route_conflict);
   auto last = first.raw->register_protocol(name, noop());
   moved.close();
   BOOST_TEST(last.active());
}

BOOST_AUTO_TEST_CASE(p2p_failed_startup_closes_contribution_admission) {
   fixture f{std::nullopt, 32, true};
   auto previous = f.raw->register_protocol({"/raw/before-failure/1"}, noop());
   auto pubsub = f.apis.get<np::pubsub_api>(np::pubsub_api::ref()).shared();
   f.identity->reject_reads = true;
   BOOST_CHECK_THROW(f.run(f.node.startup()), std::runtime_error);
   const auto state = f.node.lifecycle().snapshot();
   BOOST_CHECK(state.failure.has_value());
   BOOST_CHECK(!state.stop_requested);
   BOOST_CHECK(!state.admission_open);
   BOOST_CHECK_THROW(static_cast<void>(f.raw->register_protocol({"/raw/after-failure/1"}, noop())),
                     np::exceptions::route_conflict);
   BOOST_CHECK_THROW(static_cast<void>(f.raw->replace_protocol(previous, {"/raw/before-failure/1"}, noop())),
                     np::exceptions::route_conflict);
   BOOST_CHECK_THROW(static_cast<void>(f.publish({"/api/after-failure/1"})), np::exceptions::route_conflict);
   BOOST_CHECK_THROW(f.run(pubsub->publish({"after-failure"}, {1})), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(static_cast<void>(pubsub->snapshot()), p2p::exceptions::canceled);
   previous.close();
   BOOST_CHECK(!previous.active());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_raw_api_collision_both_orders_and_concurrency) {
   fixture f;
   const auto name = p2p::protocol_id{"/raw/api/collision/1"};
   auto raw = f.raw->register_protocol(name, noop());
   BOOST_CHECK_THROW((void)f.publish(name), np::exceptions::route_conflict);
   raw.close();
   auto api = f.publish(name);
   BOOST_CHECK_THROW((void)f.raw->register_protocol(name, noop()), np::exceptions::route_conflict);
   api.close();
   for (unsigned iteration = 0; iteration < 16; ++iteration) {
      std::barrier start{3};
      std::atomic_uint success{0}, conflicts{0};
      np::protocol_registration raw_owner;
      forge::api::p2p::publication api_owner;
      std::exception_ptr raw_error, api_error;
      std::promise<void> raw_done, api_done;
      auto raw_join = raw_done.get_future();
      auto api_join = api_done.get_future();
      auto raw_thread = std::thread{[&] {
         start.arrive_and_wait();
         try { raw_owner = f.raw->register_protocol(name, noop()); ++success; }
         catch (const np::exceptions::route_conflict&) { ++conflicts; }
         catch (...) { raw_error = std::current_exception(); }
         raw_done.set_value();
      }};
      auto api_thread = std::thread{[&] {
         start.arrive_and_wait();
         try { api_owner = f.publish(name); ++success; }
         catch (const np::exceptions::route_conflict&) { ++conflicts; }
         catch (...) { api_error = std::current_exception(); }
         api_done.set_value();
      }};
      start.arrive_and_wait();
      joined(raw_join); joined(api_join);
      raw_thread.join(); api_thread.join();
      if (raw_error) { std::rethrow_exception(raw_error); }
      if (api_error) { std::rethrow_exception(api_error); }
      BOOST_TEST(success.load() == 1U);
      BOOST_TEST(conflicts.load() == 1U);
   }
}

BOOST_AUTO_TEST_CASE(p2p_protocol_builtin_names_and_prestart_quota) {
   fixture f{{}, 1};
   for (const auto& name : std::vector<p2p::protocol_id>{p2p::builtins::echo, p2p::builtins::ping,
       p2p::builtins::identify, p2p::builtins::identify_push, p2p::builtins::peer_exchange,
       p2p::builtins::autonat_v1, p2p::builtins::autonat_v2_dial_request, p2p::builtins::autonat_v2_dial_back,
       p2p::builtins::relay_hop, p2p::builtins::relay_stop, p2p::builtins::dcutr, p2p::builtins::kad_dht,
       p2p::builtins::rendezvous, p2p::builtins::meshsub_v10, p2p::builtins::meshsub_v11,
       p2p::builtins::meshsub_v12, p2p::builtins::meshsub_v13}) {
      BOOST_CHECK_THROW((void)f.raw->register_protocol(name, noop()), np::exceptions::route_conflict);
      BOOST_CHECK_THROW((void)f.publish(name), np::exceptions::route_conflict);
   }
   auto owner = f.raw->register_protocol({"/raw/one/1"}, noop());
   BOOST_CHECK_THROW((void)f.raw->register_protocol({"/raw/two/1"}, noop()), np::exceptions::route_conflict);
   BOOST_CHECK_THROW((void)f.publish({"/raw/two/1"}), np::exceptions::route_conflict);
   auto replacement = f.raw->replace_protocol(owner, {"/raw/one/1"}, noop());
   replacement.close();
   auto next = f.publish({"/raw/two/1"});
   BOOST_TEST(next.active());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_prestart_close_live_replacement_and_admitted_stream_survival) {
   fixture server;
   auto absent = server.raw->register_protocol({"/raw/preclosed/1"}, noop());
   absent.close();
   auto owner = server.raw->register_protocol({"/raw/echo/1"}, echo(1));
   server.run(server.node.startup());
   const auto endpoint = server.api->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   fixture client{endpoint};
   client.run(client.node.startup());
   const auto remote = server.api->local_peer();
   BOOST_CHECK_THROW((void)client.run(client.raw->open(remote, {"/raw/preclosed/1"}, {.timeout = 2s})),
                     p2p::exceptions::unsupported_protocol);
   auto first = client.run(client.raw->open(remote, {"/raw/echo/1"}, {.timeout = 2s}));
   BOOST_CHECK(first.authentication() == p2p::peer_authentication::libp2p_tls);
   BOOST_CHECK(client.run(exchange(first)) == (std::vector<std::uint8_t>{42, 1}));
   auto replacement = server.raw->replace_protocol(owner, {"/raw/echo/1"}, echo(2));
   owner.close();
   auto second = client.run(client.raw->open(remote, {"/raw/echo/1"}, {.timeout = 2s}));
   BOOST_CHECK(client.run(exchange(second)) == (std::vector<std::uint8_t>{42, 2}));
   replacement.close();
   BOOST_CHECK_THROW((void)client.run(client.raw->open(remote, {"/raw/echo/1"}, {.timeout = 2s})),
                     p2p::exceptions::unsupported_protocol);
   BOOST_CHECK(client.run(exchange(first)) == (std::vector<std::uint8_t>{42, 1}));
   BOOST_CHECK(client.run(exchange(second)) == (std::vector<std::uint8_t>{42, 2}));
   first.request_cancel(); second.request_cancel();
}

BOOST_AUTO_TEST_CASE(p2p_api_authenticated_open_accepts_native_verified_peer) {
   fixture server;
   auto raw = server.raw->register_protocol({"/raw/auth-evidence/1"}, echo(1));
   auto publication = server.publish({"/api/authenticated-open/1"});
   server.run(server.node.startup());
   const auto endpoint = server.api->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   fixture client{endpoint};
   client.run(client.node.startup());
   auto evidence = client.run(client.raw->open(server.api->local_peer(), {"/raw/auth-evidence/1"}, {.timeout = 2s}));
   BOOST_CHECK(evidence.authentication() == p2p::peer_authentication::libp2p_tls);
   client.run(evidence.async_close());
   auto connection = client.run(client.api->open_api_connection(server.api->local_peer(),
      {"/api/authenticated-open/1"}, {.require_authenticated_peer = true}));
   BOOST_CHECK(connection.valid());
   client.run(connection.async_close());
   server.run(publication.async_close());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_configured_dht_names_stay_reserved_and_fresh_owners_use_new_settings) {
   using value = forge::config::core::value;
   fixture f{std::nullopt, 32, false, [](auto& config) {
      config.set("plugins.net.p2p.node.dht.profiles", value::array_type{
          value{value::object_type{{"kind", "custom"}, {"mode", "client"}, {"protocol", "/raw/dht/client/1"},
                                  {"peers", true}, {"providers", false}, {"values", false}}},
          value{value::object_type{{"kind", "custom"}, {"mode", "server"}, {"protocol", "/raw/dht/server/1"},
                                  {"peers", true}, {"providers", false}, {"values", false}}},
      });
   }};
   // Client-only and provider/value-disabled profiles still own their whole name.
   check_repeated_config(f, f.configuration);
   for (const auto& protocol : {"/raw/dht/client/1", "/raw/dht/server/1"}) {
      BOOST_CHECK_THROW((void)f.raw->register_protocol({protocol}, noop()), np::exceptions::route_conflict);
      BOOST_CHECK_THROW((void)f.publish({protocol}), np::exceptions::route_conflict);
   }
   auto cleared = f.configuration;
   cleared.set("plugins.net.p2p.node.dht.profiles", value::array_type{});
   check_repeated_config(f, cleared);
   for (const auto& protocol : {"/raw/dht/client/1", "/raw/dht/server/1"}) {
      BOOST_CHECK_THROW((void)f.raw->register_protocol({protocol}, noop()), np::exceptions::route_conflict);
      BOOST_CHECK_THROW((void)f.publish({protocol}), np::exceptions::route_conflict);
   }
   fixture fresh{std::nullopt, 32, false, [cleared](auto& config) { config = cleared; }};
   auto freed_client = fresh.raw->register_protocol({"/raw/dht/client/1"}, noop());
   auto freed_server = fresh.raw->register_protocol({"/raw/dht/server/1"}, noop());
   BOOST_TEST(freed_client.active());
   BOOST_TEST(freed_server.active());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_fresh_dht_collision_and_repeated_input_preserve_the_live_raw_route) {
   using value = forge::config::core::value;
   const auto original_dht = p2p::protocol_id{"/raw/config/original-dht/1"};
   const auto candidate_dht = p2p::protocol_id{"/raw/config/rejected-dht/1"};
   const auto raw_name = p2p::protocol_id{"/raw/config/echo/1"};
   fixture server{std::nullopt, 32, false, [original_dht](auto& config) {
      config.set("plugins.net.p2p.node.dht.profiles", value::array_type{custom_dht(original_dht)});
      config.set("plugins.net.p2p.node.max-sessions", std::uint64_t{7});
      config.set("plugins.net.p2p.node.topology.peers.low", std::uint64_t{1});
      config.set("plugins.net.p2p.node.topology.peers.target", std::uint64_t{2});
      config.set("plugins.net.p2p.node.topology.peers.high", std::uint64_t{3});
      config.set("plugins.net.p2p.node.peer-exchange.enabled", false);
   }};
   const auto good = server.configuration;
   auto raw = server.raw->register_protocol(raw_name, echo(23));

   auto rejected = good;
   rejected.set("plugins.net.p2p.node.dht.profiles",
                value::array_type{custom_dht(candidate_dht), custom_dht(raw_name)});
   rejected.set("plugins.net.p2p.node.listen", value::array_type{"/ip4/127.0.0.1/udp/0/quic-v1"});
   rejected.set("plugins.net.p2p.node.identity.certificate-secret", "rejected/cert");
   rejected.set("plugins.net.p2p.node.identity.private-key-secret", "rejected/key");
   rejected.set("plugins.net.p2p.node.max-sessions", std::uint64_t{11});
   rejected.set("plugins.net.p2p.node.topology.mode", "managed");
   rejected.set("plugins.net.p2p.node.topology.peers.target", std::uint64_t{3});
   rejected.set("plugins.net.p2p.node.topology.peers.high", std::uint64_t{4});
   rejected.set("plugins.net.p2p.node.peer-exchange.enabled", true);
   rejected.set("plugins.net.p2p.node.api.deadline-ms", std::uint64_t{25});
   rejected.set("plugins.net.p2p.node.max-inflight-per-peer", std::uint64_t{1});
   rejected.set("plugins.net.p2p.node.peer-store.schema-policy", "reject");
   {
      // A fresh input still reaches the actual DHT/raw claim collision validator.
      auto probe = np::plugin{};
      auto registry = forge::api::core::registry{};
      auto installer = forge::api::core::installer{registry};
      auto cleanup = boost::scope::scope_exit{[&] {
         try { server.run(probe.shutdown()); }
         catch (const std::exception& error) { BOOST_ERROR("DHT collision probe cleanup: " << error.what()); }
         catch (...) { BOOST_ERROR("DHT collision probe cleanup: non-standard exception"); }
      }};
      server.run(probe.provide(installer));
      auto protocols = registry.get<np::protocol_api>(np::protocol_api::ref());
      auto claimed = protocols->register_protocol(raw_name, noop());
      BOOST_REQUIRE(claimed.active());
      BOOST_CHECK_THROW(server.run(probe.configure({rejected, "plugins.net.p2p.node"})),
                        np::exceptions::route_conflict);
   }
   check_repeated_config(server, rejected);
   BOOST_TEST(raw.active());
   BOOST_CHECK_THROW((void)server.raw->register_protocol(original_dht, noop()), np::exceptions::route_conflict);
   BOOST_CHECK_THROW((void)server.publish(original_dht), np::exceptions::route_conflict);
   auto unreserved = server.raw->register_protocol(candidate_dht, noop());
   BOOST_TEST(unreserved.active());
   unreserved.close();

   server.run(server.node.startup());
   const auto snapshot = server.apis.get<np::diagnostics_api>(
       {.id = {"forge.plugins.net.p2p.node.diagnostics"}, .major = 1})->snapshot();
   BOOST_REQUIRE_EQUAL(snapshot.dht_profiles.size(), 1U);
   BOOST_TEST(snapshot.dht_profiles.front().protocol.value == original_dht.value);
   BOOST_TEST(snapshot.dht_profiles.front().kind == "custom");
   BOOST_TEST(snapshot.dht_profiles.front().mode == "server");
   BOOST_TEST(snapshot.dht_profiles.front().peers);
   BOOST_TEST(!snapshot.dht_profiles.front().providers);
   BOOST_TEST(!snapshot.dht_profiles.front().values);
   BOOST_TEST(snapshot.effective_limits.system.max_connections == 8U);
   BOOST_TEST(snapshot.effective_limits.system.max_inbound_connections == 8U);
   BOOST_TEST(snapshot.effective_limits.system.max_outbound_connections == 8U);
   BOOST_TEST(snapshot.topology.mode == "static-only");
   BOOST_TEST(snapshot.topology.low_watermark == 1U);
   BOOST_TEST(snapshot.topology.target_watermark == 2U);
   BOOST_TEST(snapshot.topology.high_watermark == 3U);
   BOOST_TEST(!snapshot.topology.peer_exchange_enabled);
   BOOST_TEST(snapshot.topology.dht_enabled);

   const auto endpoint = server.api->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   BOOST_TEST(endpoint->to_string().find("/tcp/") != std::string::npos);
   fixture client{endpoint};
   client.run(client.node.startup());
   auto stream = client.run(client.raw->open(server.api->local_peer(), raw_name, {.timeout = 2s}));
   BOOST_CHECK(stream.authentication() == p2p::peer_authentication::libp2p_tls);
   BOOST_CHECK(client.run(exchange(stream)) == (std::vector<std::uint8_t>{42, 23}));
   stream.request_cancel();
}

BOOST_AUTO_TEST_CASE(p2p_protocol_invalid_fresh_input_and_repeated_config_preserve_reservations_and_quota) {
   using value = forge::config::core::value;
   const auto original_dht = p2p::protocol_id{"/raw/config/retained-dht/1"};
   const auto candidate_dht = p2p::protocol_id{"/raw/config/uncommitted-dht/1"};
   fixture f{std::nullopt, 2, false, [original_dht](auto& config) {
      config.set("plugins.net.p2p.node.dht.profiles", value::array_type{custom_dht(original_dht)});
   }};
   auto good = f.configuration;
   auto raw = f.raw->register_protocol({"/raw/config/retained/1"}, noop());

   auto rejected = good;
   rejected.set("plugins.net.p2p.node.dht.profiles", value::array_type{custom_dht(candidate_dht)});
   rejected.set("plugins.net.p2p.node.max-protocol-handlers", std::uint64_t{4});
   // This validation follows DHT/policy parsing, but must precede all publication.
   rejected.set("plugins.net.p2p.node.identity.private-key-secret", "");
   {
      auto probe = np::plugin{};
      auto cleanup = boost::scope::scope_exit{[&] {
         try { f.run(probe.shutdown()); }
         catch (const std::exception& error) { BOOST_ERROR("invalid config probe cleanup: " << error.what()); }
         catch (...) { BOOST_ERROR("invalid config probe cleanup: non-standard exception"); }
      }};
      BOOST_CHECK_THROW(f.run(probe.configure({rejected, "plugins.net.p2p.node"})), np::exceptions::invalid_config);
   }
   check_repeated_config(f, rejected);
   BOOST_TEST(raw.active());
   BOOST_CHECK_THROW((void)f.raw->register_protocol(original_dht, noop()), np::exceptions::route_conflict);
   auto second = f.raw->register_protocol(candidate_dht, noop());
   BOOST_TEST(second.active());
   BOOST_CHECK_THROW((void)f.raw->register_protocol({"/raw/config/over-quota/1"}, noop()),
                     np::exceptions::route_conflict);
   BOOST_CHECK_THROW((void)f.publish({"/raw/config/over-quota/1"}), np::exceptions::route_conflict);
   second.close();

   f.run(f.node.startup());
   const auto snapshot = f.apis.get<np::diagnostics_api>(
       {.id = {"forge.plugins.net.p2p.node.diagnostics"}, .major = 1})->snapshot();
   BOOST_REQUIRE_EQUAL(snapshot.dht_profiles.size(), 1U);
   BOOST_TEST(snapshot.dht_profiles.front().protocol.value == original_dht.value);
   BOOST_TEST(raw.active());
   // Even identical input is one-shot; neither it nor a changed quota replaces the live policy.
   check_repeated_config(f, good);
   good.set("plugins.net.p2p.node.max-protocol-handlers", std::uint64_t{4});
   check_repeated_config(f, good);
   BOOST_TEST(raw.active());
   auto live_second = f.raw->register_protocol(candidate_dht, noop());
   BOOST_TEST(live_second.active());
   BOOST_CHECK_THROW((void)f.raw->register_protocol({"/raw/config/live-over-quota/1"}, noop()),
                     np::exceptions::route_conflict);
   BOOST_CHECK_THROW((void)f.publish({"/raw/config/live-over-quota/1"}), np::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_protocol_capture_retirement_reenters_outside_registry_and_configuration_locks) {
   fixture f;
   auto count = 0U;
   np::protocol_registration successor;
   auto sentinel = std::make_shared<capture>();
   sentinel->retired = [&] { successor = f.raw->register_protocol({"/raw/reenter/1"}, noop()); ++count; };
   auto owner = f.raw->register_protocol({"/raw/reenter/1"},
       [sentinel](p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; });
   sentinel.reset();
   owner.close();
   BOOST_TEST(count == 1U);
   BOOST_TEST(successor.active());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_native_attach_failure_retires_captures_outside_configuration_lock) {
   fixture f;
   auto configuration = f.apis.get<np::configuration_api>(np::configuration_api::ref());
   std::atomic_bool retired{false};
   auto sentinel = std::make_shared<capture>();
   sentinel->retired = [&] {
      try { configuration->set_connection_gater({}); }
      catch (const np::exceptions::route_conflict&) { retired = true; }
   };
   auto owner = f.raw->register_protocol({"/" + std::string(4096, 'x')},
       [sentinel](p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; });
   sentinel.reset();
   BOOST_CHECK_THROW(f.run(f.node.startup()), p2p::exceptions::invalid_options);
   BOOST_TEST(retired.load());
   BOOST_TEST(!owner.active());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_saved_open_and_late_registration_handles_fail_after_stop) {
   std::optional<boost::asio::awaitable<p2p::stream>> saved;
   std::optional<boost::asio::awaitable<forge::api::transport::connection>> saved_api;
   std::shared_ptr<np::protocol_api> late;
   np::protocol_registration token;
   {
      fixture f;
      late = f.raw;
      BOOST_CHECK_THROW((void)f.run(late->open({}, {"/raw/test/1"})), np::exceptions::plugin_not_initialized);
      token = late->register_protocol({"/raw/test/1"}, noop());
      f.run(f.node.startup());
      saved.emplace(late->open(f.api->local_peer(), {"/raw/test/1"}));
      saved_api.emplace(f.api->open_api_connection(f.api->local_peer(), {"/raw/test/1"}));
      f.node.request_stop();
      BOOST_TEST(!token.active());
      BOOST_CHECK_THROW((void)late->register_protocol({"/raw/late/1"}, noop()), np::exceptions::route_conflict);
   }
   late.reset();
   forge::asio::runtime runtime{};
   auto raw_result = boost::asio::co_spawn(runtime.context(), std::move(*saved), boost::asio::use_future);
   auto api_result = boost::asio::co_spawn(runtime.context(), std::move(*saved_api), boost::asio::use_future);
   BOOST_CHECK_THROW((void)joined(raw_result), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)joined(api_result), p2p::exceptions::canceled);
   BOOST_CHECK_NO_THROW(token.close());
}
