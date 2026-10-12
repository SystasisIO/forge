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
import forge.app.plugin_context;
import forge.app.signals;
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
   forge::tests::p2p::identity_fixture identity = forge::tests::p2p::make_identity_fixture("raw-plugin");
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override { co_return secrets::snapshot{.configured_secrets = 2}; }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
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
   forge::app::signal_bus signals;
   forge::app::event_bus events{};
   forge::app::plugin_context context{scheduler, apis, signals, events};
   np::plugin node;
   std::shared_ptr<np::protocol_api> raw;
   std::shared_ptr<np::api> api;
   forge::config::core::document configuration;

   explicit fixture(std::optional<p2p::endpoint> bootstrap = {}, unsigned limit = 32) {
      auto& config = configuration;
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.identity.certificate-secret", "raw/cert");
      config.set("plugins.net.p2p.node.identity.private-key-secret", "raw/key");
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{"/ip4/127.0.0.1/tcp/0"});
      config.set("plugins.net.p2p.node.max-protocol-handlers", static_cast<std::uint64_t>(limit));
      if (bootstrap) {
         config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{bootstrap->to_string()});
         config.set("plugins.net.p2p.node.bootstrap-policy.requirement", "require-connection");
      }
      run(node.configure({config, "plugins.net.p2p.node"}));
      apis.install<secrets::api>(secrets::api::describe(), std::make_shared<identity_source>());
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

BOOST_AUTO_TEST_CASE(p2p_protocol_all_configured_dht_names_are_reserved_and_reconfigure_replaces_snapshot) {
   fixture f;
   using value = forge::config::core::value;
   auto config = forge::config::core::document{};
   config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
   config.set("plugins.net.p2p.node.dht.profiles", value::array_type{
       value{value::object_type{{"kind", "custom"}, {"mode", "client"}, {"protocol", "/raw/dht/client/1"},
                               {"peers", true}, {"providers", false}, {"values", false}}},
       value{value::object_type{{"kind", "custom"}, {"mode", "server"}, {"protocol", "/raw/dht/server/1"},
                               {"peers", true}, {"providers", false}, {"values", false}}},
   });
   // Client-only and provider/value-disabled profiles still own their whole name.
   f.run(f.node.configure({config, "plugins.net.p2p.node"}));
   f.run(f.node.configure({config, "plugins.net.p2p.node"}));
   for (const auto& protocol : {"/raw/dht/client/1", "/raw/dht/server/1"}) {
      BOOST_CHECK_THROW((void)f.raw->register_protocol({protocol}, noop()), np::exceptions::route_conflict);
      BOOST_CHECK_THROW((void)f.publish({protocol}), np::exceptions::route_conflict);
   }
   config.set("plugins.net.p2p.node.dht.profiles", value::array_type{});
   f.run(f.node.configure({config, "plugins.net.p2p.node"}));
   auto freed = f.raw->register_protocol({"/raw/dht/client/1"}, noop());
   BOOST_TEST(freed.active());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_rejected_dht_collision_preserves_configuration_and_live_raw_route) {
   fixture server;
   using value = forge::config::core::value;
   const auto original_dht = p2p::protocol_id{"/raw/config/original-dht/1"};
   const auto candidate_dht = p2p::protocol_id{"/raw/config/rejected-dht/1"};
   const auto raw_name = p2p::protocol_id{"/raw/config/echo/1"};
   auto good = server.configuration;
   good.set("plugins.net.p2p.node.dht.profiles", value::array_type{custom_dht(original_dht)});
   good.set("plugins.net.p2p.node.max-sessions", std::uint64_t{7});
   good.set("plugins.net.p2p.node.topology.peers.low", std::uint64_t{1});
   good.set("plugins.net.p2p.node.topology.peers.target", std::uint64_t{2});
   good.set("plugins.net.p2p.node.topology.peers.high", std::uint64_t{3});
   good.set("plugins.net.p2p.node.peer-exchange.enabled", false);
   server.run(server.node.configure({good, "plugins.net.p2p.node"}));
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
   BOOST_CHECK_THROW(server.run(server.node.configure({rejected, "plugins.net.p2p.node"})),
                     np::exceptions::route_conflict);
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

BOOST_AUTO_TEST_CASE(p2p_protocol_invalid_config_after_dht_candidate_preserves_reservations_and_quota) {
   fixture f{{}, 2};
   using value = forge::config::core::value;
   const auto original_dht = p2p::protocol_id{"/raw/config/retained-dht/1"};
   const auto candidate_dht = p2p::protocol_id{"/raw/config/uncommitted-dht/1"};
   auto good = f.configuration;
   good.set("plugins.net.p2p.node.dht.profiles", value::array_type{custom_dht(original_dht)});
   f.run(f.node.configure({good, "plugins.net.p2p.node"}));
   auto raw = f.raw->register_protocol({"/raw/config/retained/1"}, noop());

   auto rejected = good;
   rejected.set("plugins.net.p2p.node.dht.profiles", value::array_type{custom_dht(candidate_dht)});
   rejected.set("plugins.net.p2p.node.max-protocol-handlers", std::uint64_t{4});
   // This validation follows DHT/policy parsing, but must precede all publication.
   rejected.set("plugins.net.p2p.node.identity.private-key-secret", "");
   BOOST_CHECK_THROW(f.run(f.node.configure({rejected, "plugins.net.p2p.node"})), np::exceptions::invalid_config);
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
   // Live diagnostics updates allow an unchanged frozen node policy, not a
   // new protocol quota or another native configuration change.
   BOOST_CHECK_NO_THROW(f.run(f.node.configure({good, "plugins.net.p2p.node"})));
   good.set("plugins.net.p2p.node.max-protocol-handlers", std::uint64_t{4});
   BOOST_CHECK_THROW(f.run(f.node.configure({good, "plugins.net.p2p.node"})), np::exceptions::route_conflict);
   BOOST_TEST(raw.active());
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
