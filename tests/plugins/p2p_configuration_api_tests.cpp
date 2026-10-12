#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.descriptor;
import forge.api.core.registry;
import forge.api.core.types;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.exceptions;
import forge.net.p2p.connection_gater;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;

namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;
using namespace std::chrono_literals;

template<class T> T joined(std::future<T>& result) {
   if (result.wait_for(10s) != std::future_status::ready) {
      std::cerr << "configuration fixture failed its owned join budget\n";
      std::terminate();
   }
   return result.get();
}

void bounded(std::function<void()> operation) {
   auto task = std::packaged_task<void()>{std::move(operation)};
   auto result = task.get_future();
   auto worker = std::thread{std::move(task)};
   auto join = boost::scope::scope_exit{[&] { worker.join(); }};
   joined(result);
}

class gater final : public p2p::connection_gater {};

class dial_gater final : public p2p::connection_gater {
 public:
   dial_gater(p2p::peer_id target, bool allow) : _target(std::move(target)), _allow(allow) {}
   std::atomic_uint expected_calls{0};
   std::atomic_uint foreign_calls{0};
   bool intercept_peer_dial(const p2p::peer_id& peer) noexcept override {
      if (peer != _target) { ++foreign_calls; return false; }
      ++expected_calls;
      return _allow;
   }
 private:
   const p2p::peer_id _target;
   const bool _allow;
};

class identity_source final : public secrets::api {
 public:
   forge::tests::p2p::identity_fixture identity = forge::tests::p2p::make_identity_fixture("configuration-api");
   std::atomic_uint reads{0};
   std::atomic_uint psk_reads{0};
   std::string last_certificate;
   std::string last_psk;
   std::string held_purpose = "p2p.identity.certificate";
   std::size_t psk_size = 32;
   std::atomic_bool hold{false};
   std::promise<void> entered;
   forge::asio::notification released;
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override { co_return secrets::snapshot{}; }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      ++reads;
      if (request.purpose == "p2p.identity.certificate") {
         last_certificate = request.secret_id;
      }
      if (request.purpose == "p2p.private-network.psk") {
         ++psk_reads;
         last_psk = request.secret_id;
      }
      if (request.purpose == held_purpose && hold.load()) {
         entered.set_value();
         while (hold.load()) {
            const auto epoch = released.epoch();
            if (!hold.load()) { break; }
            co_await released.async_wait(epoch);
         }
      }
      if (request.purpose == "p2p.private-network.psk") {
         co_return secrets::get_result{.secret_id = request.secret_id, .bytes = std::vector<std::uint8_t>(psk_size, 0x71)};
      }
      const auto& material = request.purpose == "p2p.identity.certificate" ? identity.certificate_pem : identity.private_key_pem;
      co_return secrets::get_result{.secret_id = request.secret_id, .bytes = {material.begin(), material.end()}};
   }
   boost::asio::awaitable<secrets::derive_result> derive_hkdf_sha256(secrets::derive_request) override {
      throw std::logic_error{"not used"}; co_return secrets::derive_result{};
   }
   boost::asio::awaitable<secrets::aead_encrypt_result> encrypt_aes_gcm(secrets::aead_encrypt_request) override {
      throw std::logic_error{"not used"}; co_return secrets::aead_encrypt_result{};
   }
   boost::asio::awaitable<secrets::aead_decrypt_result> decrypt_aes_gcm(secrets::aead_decrypt_request) override {
      throw std::logic_error{"not used"}; co_return secrets::aead_decrypt_result{};
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
   std::shared_ptr<identity_source> identity = std::make_shared<identity_source>();
   std::shared_ptr<np::configuration_api> configuration;
   std::shared_ptr<np::protocol_api> raw;
   forge::config::core::document document;

   explicit fixture(bool with_secrets = true) {
      document.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      document.set("plugins.net.p2p.node.topology.mode", "static-only");
      document.set("plugins.net.p2p.node.pubsub.enabled", true);
      if (with_secrets) {
         document.set("plugins.net.p2p.node.identity.certificate-secret", "configuration/cert");
         document.set("plugins.net.p2p.node.identity.private-key-secret", "configuration/key");
      } else {
         document.set("plugins.net.p2p.node.peer-id", p2p::make_peer_id_from_certificate_pem(identity->identity.certificate_pem).value);
      }
      configure();
      apis.install<secrets::api>(secrets::api::describe(), identity);
      auto installer = forge::api::core::installer{apis};
      run(node.provide(installer));
      configuration = apis.get<np::configuration_api>(np::configuration_api::ref()).shared();
      raw = apis.get<np::protocol_api>(np::protocol_api::ref()).shared();
   }
   ~fixture() {
      identity->hold.store(false); identity->released.notify();
      node.request_stop();
      run(node.shutdown());
      run(scheduler.shutdown());
   }
   template<class T> T run(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return joined(result);
   }
   void configure() { run(node.configure({document, "plugins.net.p2p.node"})); }
   void initialize(bool after = true) { run(node.initialize(context)); if (after) { run(node.after_initialize()); } }
   void declare_custom(bool values = true) {
      using value = forge::config::core::value;
      document.set("plugins.net.p2p.node.dht.profiles", value::array_type{
          value::object_type{{"kind", "custom"}, {"mode", "server"}, {"protocol", "/configuration/kad/1"},
                              {"peers", true}, {"providers", false}, {"values", values}}});
      configure();
   }
};

void select_private_network(fixture& f, std::string reference = "configuration/psk") {
   f.document.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
       forge::config::core::value{"/ip4/127.0.0.1/tcp/0"}});
   f.document.set("plugins.net.p2p.node.path.policy", "direct-only");
   f.document.set("plugins.net.p2p.node.relay.client-enabled", false);
   f.document.set("plugins.net.p2p.node.relay.server-enabled", false);
   f.document.set("plugins.net.p2p.node.relay.auto-discovery-enabled", false);
   f.document.set("plugins.net.p2p.node.private-network.psk-secret", std::move(reference));
}

std::vector<p2p::dht::value_policy> policies() {
   return {{.key_prefix = {'/', 't', '/'},
            .validate = [](const p2p::dht::record&, p2p::dht::value_validation_context) {},
            .select = [](std::span<const p2p::dht::record>) { return std::size_t{0}; },
            .expiry = [](const p2p::dht::record&, p2p::dht::value_expiry_context context) { return context.supplied_expires_at; }}};
}

struct copy_control {
   std::atomic_bool armed{false};
   std::atomic_uint copied{0};
   std::function<void()> on_copy;
};
struct traced_copy {
   std::shared_ptr<copy_control> control;
   explicit traced_copy(std::shared_ptr<copy_control> value) : control(std::move(value)) {}
   traced_copy(const traced_copy& other) : control(other.control) {
      if (control->armed.exchange(false)) { ++control->copied; control->on_copy(); }
   }
   traced_copy(traced_copy&&) noexcept = default;
   void operator()(const p2p::pubsub::trace_event&) const {}
};
void install_copy_probe(fixture& f, const std::shared_ptr<copy_control>& control) {
   auto options = p2p::pubsub::options{};
   options.tracer = traced_copy{control};
   f.configuration->set_pubsub_options(std::move(options));
}
struct retirement_result {
   std::atomic_bool completed{false};
   std::exception_ptr failure;
};
struct retired_capture {
   std::function<void()> on_destroy;
   std::shared_ptr<retirement_result> result;
   ~retired_capture() noexcept {
      try { on_destroy(); }
      catch (...) { result->failure = std::current_exception(); }
      result->completed.store(true, std::memory_order_release);
   }
};
} // namespace

BOOST_AUTO_TEST_CASE(p2p_configuration_disabled_pubsub_keeps_consumer_admission_closed) {
   fixture f{false};
   f.document.set("plugins.net.p2p.node.pubsub.enabled", false);
   f.configure();
   BOOST_CHECK_THROW(f.configuration->set_pubsub_options({}), np::exceptions::invalid_config);
   f.initialize();
   f.run(f.node.startup());
   const auto pubsub = f.apis.get<np::pubsub_api>(np::pubsub_api::ref());
   BOOST_CHECK_THROW(f.run(pubsub->publish({"disabled"}, {1})), p2p::exceptions::unsupported_protocol);
   BOOST_CHECK_THROW(f.run(pubsub->subscribe({"disabled"},
       [](np::message) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
          co_return p2p::pubsub::validation_result::accept;
       })), p2p::exceptions::unsupported_protocol);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_contract_gater_and_late_handle) {
   std::shared_ptr<np::configuration_api> late;
   {
      fixture f;
      const auto descriptor = np::configuration_api::describe();
      BOOST_TEST(descriptor.version.major == 1U);
      BOOST_TEST(descriptor.version.revision == 0U);
      BOOST_CHECK(descriptor.supported_surfaces == forge::api::core::surface::local);
      BOOST_TEST(descriptor.methods.empty());
      BOOST_CHECK_THROW(f.configuration->set_connection_gater({}), np::exceptions::invalid_config);
      f.configuration->set_connection_gater(std::make_shared<gater>());
      BOOST_CHECK_THROW(f.configuration->set_connection_gater(std::make_shared<gater>()), np::exceptions::route_conflict);
      f.initialize(false); // Empty Store does not require after_initialize for startup.
      f.run(f.node.startup());
      BOOST_CHECK_THROW(f.configuration->set_dht_value_policies({"/unknown"}, policies()), np::exceptions::route_conflict);
      late = f.configuration;
   }
   BOOST_CHECK_THROW(late->set_connection_gater(std::make_shared<gater>()), np::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_gater_denies_native_peer_dial_and_allow_control_connects) {
   // Explicit in-memory test mode proves gater delegation, not identity policy.
   fixture server;
   server.document.set("plugins.net.p2p.node.listen",
                       forge::config::core::value::array_type{"/ip4/127.0.0.1/tcp/0"});
   server.configure(); server.initialize(); server.run(server.node.startup());
   const auto server_api = server.apis.get<np::api>(np::api::ref()).shared();
   const auto endpoint = server_api->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   const auto peer = server_api->local_peer();
   const auto options = p2p::node::connect_options{
       .expected_peer = peer, .allow_relay = false, .timeout = 3s,
       .direct_attempt_timeout = 2s, .allow_hole_punch = false};

   fixture denied;
   const auto deny = std::make_shared<dial_gater>(peer, false);
   denied.configuration->set_connection_gater(deny);
   denied.initialize(); denied.run(denied.node.startup());
   const auto denied_api = denied.apis.get<np::api>(np::api::ref()).shared();
   const auto denied_diagnostics = denied.apis.get<np::diagnostics_api>(np::diagnostics_api::ref()).shared();
   BOOST_CHECK_EXCEPTION(denied.run(denied_api->connect(*endpoint, options)), forge::exceptions::base,
                         [](const auto& error) {
      return p2p::exceptions::is(error, p2p::exceptions::code::connection_rejected);
   });
   BOOST_TEST(deny->expected_calls.load() > 0U);
   BOOST_TEST(deny->foreign_calls.load() == 0U);
   const auto rejected = denied_diagnostics->snapshot();
   BOOST_TEST(rejected.metrics.gater_peer_dial_rejections > 0U);
   BOOST_TEST(rejected.connections.active_sessions == 0U);

   fixture allowed;
   const auto allow = std::make_shared<dial_gater>(peer, true);
   allowed.configuration->set_connection_gater(allow);
   allowed.initialize(); allowed.run(allowed.node.startup());
   const auto allowed_api = allowed.apis.get<np::api>(np::api::ref()).shared();
   const auto connected = allowed.run(allowed_api->connect(*endpoint, options));
   BOOST_CHECK(connected.remote_peer == peer);
   BOOST_TEST(allow->expected_calls.load() > 0U);
   BOOST_TEST(allow->foreign_calls.load() == 0U);
   BOOST_TEST(allowed.run(allowed_api->ping(peer, {.allow_relay = false, .timeout = 3s})).count() >= 0);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_native_declarations_are_not_complete_profiles) {
   auto profile = p2p::dht::profile{};
   profile.protocol = {"/configuration/kad/1"};
   profile.capabilities.values = true;
   BOOST_CHECK_NO_THROW(p2p::validate_declaration(profile));
   BOOST_CHECK_THROW(p2p::validate(profile), p2p::exceptions::invalid_options);
   profile.protocol = {"relative"};
   BOOST_CHECK_THROW(p2p::validate_declaration(profile), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(p2p::validate(profile), p2p::exceptions::invalid_options);
   profile.protocol = {"/configuration/kad/1"};
   profile.limits.alpha = 0;
   BOOST_CHECK_THROW(p2p::validate_declaration(profile), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(p2p::validate(profile), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_custom_policy_is_complete_unique_and_reserved) {
   fixture f;
   f.declare_custom();
   const auto name = p2p::protocol_id{"/configuration/kad/1"};
   BOOST_CHECK_THROW(static_cast<void>(f.raw->register_protocol(name, [](p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; })), np::exceptions::route_conflict);
   BOOST_CHECK_THROW(f.configuration->set_dht_value_policies({"/unknown"}, policies()), np::exceptions::invalid_config);
   BOOST_CHECK_THROW(f.configuration->set_dht_value_policies(p2p::builtins::kad_dht, policies()), np::exceptions::invalid_config);
   for (unsigned missing = 0; missing != 3; ++missing) {
      auto values = policies();
      if (missing == 0) { values.front().validate = {}; }
      if (missing == 1) { values.front().select = {}; }
      if (missing == 2) { values.front().expiry = {}; }
      BOOST_CHECK_THROW(f.configuration->set_dht_value_policies(name, std::move(values)), np::exceptions::invalid_config);
   }
   BOOST_CHECK_THROW(f.configuration->set_dht_value_policies(name, {}), np::exceptions::invalid_config);
   auto overlap = policies(); overlap.push_back(overlap.front());
   BOOST_CHECK_THROW(f.configuration->set_dht_value_policies(name, std::move(overlap)), np::exceptions::invalid_config);
   f.configuration->set_dht_value_policies(name, policies());
   BOOST_CHECK_THROW(f.configuration->set_dht_value_policies(name, policies()), np::exceptions::route_conflict);
   f.initialize(); f.run(f.node.startup());
}

BOOST_AUTO_TEST_CASE(p2p_configuration_values_disabled_and_amino_cannot_be_overridden) {
   fixture f;
   f.declare_custom(false);
   BOOST_CHECK_THROW(f.configuration->set_dht_value_policies({"/configuration/kad/1"}, policies()), np::exceptions::invalid_config);
   using value = forge::config::core::value;
   f.document.set("plugins.net.p2p.node.dht.profiles", value::array_type{value::object_type{{"kind", "amino-v1"}}});
   f.configure();
   BOOST_CHECK_THROW(f.configuration->set_dht_value_policies(p2p::builtins::kad_dht, policies()), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_incomplete_profile_fails_before_secrets_and_can_be_completed) {
   fixture f;
   f.declare_custom(); f.initialize();
   BOOST_CHECK_THROW(f.run(f.node.startup()), np::exceptions::invalid_config);
   BOOST_TEST(f.identity->reads.load() == 0U);
   f.configuration->set_dht_value_policies({"/configuration/kad/1"}, policies());
   f.run(f.node.startup());
   BOOST_TEST(f.identity->reads.load() == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_startup_requires_initialized_dependencies) {
   fixture f;
   BOOST_CHECK_THROW(f.run(f.node.startup()), np::exceptions::plugin_not_initialized);
   BOOST_TEST(f.identity->reads.load() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_rejected_initialize_preserves_original_binding) {
   fixture f;
   f.initialize(false);
   {
      auto runtime = forge::asio::runtime{};
      auto scheduler = forge::asio::task::scheduler{runtime};
      auto context = forge::app::plugin_context{scheduler, f.apis, f.signals, f.events};
      BOOST_CHECK_THROW(f.run(f.node.initialize(context)), np::exceptions::route_conflict);
      f.run(scheduler.shutdown());
   }
   // The rejected provider binding no longer exists. Startup still uses the
   // first initialized runtime and does not require an empty Store binding.
   f.run(f.node.startup());
   BOOST_TEST(f.identity->reads.load() == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_actual_internal_copy_failure_preserves_previous) {
   fixture f;
   auto probe = std::make_shared<copy_control>();
   install_copy_probe(f, probe);
   auto token = f.raw->register_protocol({"/configuration/live/1"}, [](p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; });
   probe->on_copy = [] { throw std::runtime_error{"actual candidate copy failed"}; };
   probe->armed.store(true);
   BOOST_CHECK_THROW(f.configuration->set_connection_gater(std::make_shared<gater>()), std::runtime_error);
   BOOST_TEST(probe->copied.load() == 1U);
   BOOST_TEST(token.active());
   f.configuration->set_connection_gater(std::make_shared<gater>());
}

BOOST_AUTO_TEST_CASE(p2p_configuration_internal_copy_reentry_keeps_nested_change) {
   fixture f;
   f.declare_custom();
   auto probe = std::make_shared<copy_control>();
   install_copy_probe(f, probe);
   probe->on_copy = [&] { f.configuration->set_connection_gater(std::make_shared<gater>()); };
   probe->armed.store(true);
   bounded([&] {
      BOOST_CHECK_THROW(f.configuration->set_dht_value_policies({"/configuration/kad/1"}, policies()), np::exceptions::route_conflict);
   });
   BOOST_TEST(probe->copied.load() == 1U);
   BOOST_CHECK_THROW(f.configuration->set_connection_gater(std::make_shared<gater>()), np::exceptions::route_conflict);
   f.configuration->set_dht_value_policies({"/configuration/kad/1"}, policies());
}

BOOST_AUTO_TEST_CASE(p2p_configuration_retired_callback_destructor_reenters_before_api_return) {
   fixture f;
   const auto result = std::make_shared<retirement_result>();
   auto capture = std::make_shared<retired_capture>();
   capture->result = result;
   capture->on_destroy = [weak = std::weak_ptr{f.configuration}] {
      const auto api = weak.lock();
      if (!api) { throw std::logic_error{"configuration owner died before callback retirement"}; }
      api->set_connection_gater(std::make_shared<gater>());
   };
   {
      // A moved-from std::function may retain its small callable. End producer
      // ownership before measuring release by the configuration API.
      auto options = p2p::pubsub::options{};
      options.tracer = [capture](const p2p::pubsub::trace_event&) {};
      f.configuration->set_pubsub_options(std::move(options));
   }
   const auto retired = std::weak_ptr{capture};
   capture.reset();
   BOOST_REQUIRE_EQUAL(retired.use_count(), 1);
   auto completed_at_return = false;
   bounded([&] {
      f.configuration->set_pubsub_options({});
      completed_at_return = result->completed.load(std::memory_order_acquire);
   });
   BOOST_REQUIRE(completed_at_return);
   BOOST_CHECK(!result->failure);
   BOOST_TEST(retired.expired());
   BOOST_CHECK_THROW(f.configuration->set_connection_gater(std::make_shared<gater>()), np::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_stop_during_startup_copy_prevents_secrets_and_native_start) {
   fixture f;
   auto probe = std::make_shared<copy_control>();
   install_copy_probe(f, probe);
   f.initialize();
   std::promise<void> entered, release;
   auto arrived = entered.get_future();
   auto resume = release.get_future().share();
   probe->on_copy = [&] {
      entered.set_value();
      if (resume.wait_for(10s) != std::future_status::ready) { std::terminate(); }
   };
   probe->armed.store(true);
   auto startup = boost::asio::co_spawn(f.runtime.context(), f.node.startup(), boost::asio::use_future);
   auto cleanup = boost::scope::scope_exit{[&] {
      release.set_value();
      if (startup.valid()) {
         try { joined(startup); }
         catch (const std::exception& error) { std::cerr << "startup copy cleanup: " << error.what() << '\n'; }
      }
   }};
   joined(arrived);
   BOOST_REQUIRE(startup.wait_for(0ms) == std::future_status::timeout);
   BOOST_TEST(f.identity->reads.load() == 0U);
   bounded([&] { f.node.request_stop(); });
   BOOST_CHECK_THROW(f.configuration->set_connection_gater(std::make_shared<gater>()), np::exceptions::route_conflict);
   cleanup.set_active(false);
   release.set_value();
   joined(startup);
   BOOST_TEST(probe->copied.load() == 1U);
   BOOST_TEST(f.identity->reads.load() == 0U);
   const auto api = f.apis.get<np::api>(np::api::ref()).shared();
   BOOST_CHECK_THROW(static_cast<void>(api->local_endpoint()), np::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_stop_during_internal_copy_is_bounded_and_rejects_commit) {
   fixture f;
   auto probe = std::make_shared<copy_control>();
   install_copy_probe(f, probe);
   std::promise<void> entered, release;
   auto arrived = entered.get_future();
   auto resume = release.get_future().share();
   probe->on_copy = [&] { entered.set_value(); if (resume.wait_for(10s) != std::future_status::ready) { std::terminate(); } };
   probe->armed.store(true);
   auto task = std::packaged_task<void()>{[&] { f.configuration->set_connection_gater(std::make_shared<gater>()); }};
   auto result = task.get_future();
   auto worker = std::thread{std::move(task)};
   auto cleanup = boost::scope::scope_exit{[&] { release.set_value(); if (result.wait_for(10s) != std::future_status::ready) { std::terminate(); } worker.join(); }};
   joined(arrived);
   bounded([&] { f.node.request_stop(); });
   cleanup.set_active(false); release.set_value();
   auto join = boost::scope::scope_exit{[&] { worker.join(); }};
   BOOST_CHECK_THROW(joined(result), np::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_unbound_providers_and_empty_store_binding_cannot_change) {
   fixture f{false}; f.initialize();
   f.document.set("plugins.net.p2p.node.identity.certificate-secret", "new/cert");
   f.document.set("plugins.net.p2p.node.identity.private-key-secret", "new/key");
   BOOST_CHECK_THROW(f.configure(), np::exceptions::invalid_config);
   f.document.set("plugins.net.p2p.node.identity.certificate-secret", "");
   f.document.set("plugins.net.p2p.node.identity.private-key-secret", "");
   f.document.set("plugins.net.p2p.node.peer-store.store", "unbound");
   BOOST_CHECK_THROW(f.configure(), np::exceptions::invalid_config);
   f.document.set("plugins.net.p2p.node.peer-store.store", "");
   f.document.set("plugins.net.p2p.node.max-sessions", std::uint64_t{9});
   BOOST_CHECK_NO_THROW(f.configure());
}

BOOST_AUTO_TEST_CASE(p2p_configuration_bound_secret_ids_can_change_or_clear_before_startup) {
   fixture f; f.initialize();
   f.document.set("plugins.net.p2p.node.identity.certificate-secret", "changed/cert");
   f.document.set("plugins.net.p2p.node.identity.private-key-secret", "changed/key");
   f.configure(); f.run(f.node.startup());
   BOOST_TEST(f.identity->last_certificate == "changed/cert");
   fixture cleared; cleared.initialize();
   cleared.document.set("plugins.net.p2p.node.identity.certificate-secret", "");
   cleared.document.set("plugins.net.p2p.node.identity.private-key-secret", "");
   cleared.document.set("plugins.net.p2p.node.peer-id", p2p::make_peer_id_from_certificate_pem(cleared.identity->identity.certificate_pem).value);
   cleared.configure(); cleared.run(cleared.node.startup());
   BOOST_TEST(cleared.identity->reads.load() == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_shutdown_joins_startup_secret_read_before_cleanup) {
   fixture f; f.initialize();
   f.identity->hold.store(true);
   auto entered = f.identity->entered.get_future();
   auto startup = boost::asio::co_spawn(f.runtime.context(), f.node.startup(), boost::asio::use_future);
   auto cleanup = boost::scope::scope_exit{[&] {
      f.identity->hold.store(false); f.identity->released.notify();
      if (startup.valid()) { joined(startup); }
   }};
   joined(entered);
   f.node.request_stop();
   auto shutdown = boost::asio::co_spawn(f.runtime.context(), f.node.shutdown(), boost::asio::use_future);
   BOOST_CHECK(shutdown.wait_for(30ms) != std::future_status::ready);
   f.identity->hold.store(false); f.identity->released.notify();
   joined(startup); joined(shutdown);
   BOOST_TEST(f.identity->reads.load() == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_cancelled_shutdown_still_joins_setup_and_preserves_caller_policy) {
   fixture f; f.initialize();
   f.identity->hold.store(true);
   auto entered = f.identity->entered.get_future();
   auto startup = boost::asio::co_spawn(f.runtime.context(), f.node.startup(), boost::asio::use_future);
   auto cleanup = boost::scope::scope_exit{[&] {
      f.identity->hold.store(false); f.identity->released.notify();
      if (startup.valid()) { joined(startup); }
   }};
   joined(entered);
   const auto executor = boost::asio::make_strand(f.runtime.context());
   boost::asio::cancellation_signal cancellation;
   auto shutdown = boost::asio::co_spawn(executor,
       [](np::plugin* node) -> boost::asio::awaitable<bool> {
          co_await node->shutdown();
          const auto state = co_await boost::asio::this_coro::cancellation_state;
          co_return state.cancelled() != boost::asio::cancellation_type::none;
       }(&f.node), boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   BOOST_REQUIRE(shutdown.wait_for(30ms) == std::future_status::timeout);
   auto cancelled = boost::asio::post(executor, boost::asio::use_future([&] {
      cancellation.emit(boost::asio::cancellation_type::terminal);
   }));
   joined(cancelled);
   BOOST_REQUIRE(shutdown.wait_for(30ms) == std::future_status::timeout);
   BOOST_TEST(f.identity->reads.load() == 1U);
   f.identity->hold.store(false); f.identity->released.notify();
   joined(startup);
   BOOST_TEST(joined(shutdown));
   BOOST_TEST(f.identity->reads.load() == 1U);
   const auto api = f.apis.get<np::api>(np::api::ref()).shared();
   BOOST_CHECK_THROW(static_cast<void>(api->local_endpoint()), np::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_private_network_mixed_rejection_preserves_frozen_secret_and_policy) {
   fixture f;
   select_private_network(f, "accepted/psk");
   f.configure();
   const auto accepted = f.document;
   f.document.set("plugins.net.p2p.node.private-network.psk-secret", "rejected/psk");
   f.document.set("plugins.net.p2p.node.quic.max-connections", std::uint64_t{1});
   BOOST_CHECK_THROW(f.configure(), np::exceptions::invalid_config);
   f.initialize();
   f.run(f.node.startup());
   BOOST_TEST(f.identity->reads.load() == 3U);
   BOOST_TEST(f.identity->psk_reads.load() == 1U);
   BOOST_TEST(f.identity->last_psk == "accepted/psk");
   const auto api = f.apis.get<np::api>(np::api::ref()).shared();
   const auto endpoint = api->local_endpoint();
   BOOST_REQUIRE(endpoint);
   BOOST_TEST(endpoint->is_direct_tcp());
   f.document = accepted;
   f.document.set("plugins.net.p2p.node.private-network.psk-secret", "too-late/psk");
   BOOST_CHECK_THROW(f.configure(), np::exceptions::route_conflict);
   f.document = accepted;
   f.document.set("plugins.net.p2p.node.private-network.internet-egress", "allow-internet");
   BOOST_CHECK_THROW(f.configure(), np::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_private_network_rejects_empty_short_and_long_export) {
   for (const auto size : {0U, 31U, 33U}) {
      fixture f;
      select_private_network(f);
      f.configure();
      f.initialize();
      f.identity->psk_size = size;
      BOOST_CHECK_THROW(f.run(f.node.startup()), np::exceptions::invalid_config);
      BOOST_TEST(f.identity->psk_reads.load() == 1U);
      const auto api = f.apis.get<np::api>(np::api::ref()).shared();
      BOOST_CHECK_THROW(static_cast<void>(api->local_endpoint()), np::exceptions::plugin_not_initialized);
   }
}

BOOST_AUTO_TEST_CASE(p2p_configuration_private_network_can_clear_or_replace_before_startup_and_bind_psk_only) {
   fixture cleared;
   select_private_network(cleared);
   cleared.configure();
   cleared.initialize();
   cleared.document.set("plugins.net.p2p.node.private-network", forge::config::core::value{});
   cleared.configure();
   cleared.run(cleared.node.startup());
   BOOST_TEST(cleared.identity->psk_reads.load() == 0U);

   // This is dependency-binding evidence only: no secure wire claim for the
   // explicit test identity / in-memory persistence fixture.
   fixture psk_only{false};
   select_private_network(psk_only, "initial/psk");
   psk_only.configure();
   psk_only.initialize();
   psk_only.document.set("plugins.net.p2p.node.private-network", forge::config::core::value{});
   psk_only.configure();
   select_private_network(psk_only, "replacement/psk");
   psk_only.configure();
   psk_only.run(psk_only.node.startup());
   BOOST_TEST(psk_only.identity->reads.load() == 1U);
   BOOST_TEST(psk_only.identity->psk_reads.load() == 1U);
   BOOST_TEST(psk_only.identity->last_psk == "replacement/psk");
}

BOOST_AUTO_TEST_CASE(p2p_configuration_held_psk_stop_cancelled_shutdown_and_retry_join_setup) {
   fixture f;
   select_private_network(f);
   f.configure();
   f.initialize();
   f.identity->held_purpose = "p2p.private-network.psk";
   f.identity->hold.store(true);
   auto entered = f.identity->entered.get_future();
   const auto executor = boost::asio::make_strand(f.runtime.context());
   boost::asio::cancellation_signal cancellation;
   auto shutdown = std::future<bool>{};
   auto retry = std::future<void>{};
   auto cancelled = std::future<void>{};
   auto startup = boost::asio::co_spawn(f.runtime.context(), f.node.startup(), boost::asio::use_future);
   auto cleanup = boost::scope::scope_exit{[&] {
      f.identity->hold.store(false); f.identity->released.notify();
      auto failure = std::exception_ptr{};
      const auto drain = [&](auto& pending) {
         if (!pending.valid()) { return; }
         try { static_cast<void>(joined(pending)); }
         catch (...) { if (!failure) { failure = std::current_exception(); } }
      };
      drain(cancelled);
      drain(startup);
      drain(shutdown);
      drain(retry);
      if (failure) { BOOST_ERROR("held PSK fixture cleanup reported an error after joining all operations"); }
   }};
   joined(entered);
   BOOST_TEST(f.identity->reads.load() == 3U);
   BOOST_TEST(f.identity->psk_reads.load() == 1U);
   f.document.set("plugins.net.p2p.node.private-network.psk-secret", "too-late/psk");
   BOOST_CHECK_THROW(f.configure(), np::exceptions::route_conflict);
   f.node.request_stop();
   shutdown = boost::asio::co_spawn(executor,
       [](np::plugin* node) -> boost::asio::awaitable<bool> {
          co_await node->shutdown();
          const auto state = co_await boost::asio::this_coro::cancellation_state;
          co_return state.cancelled() != boost::asio::cancellation_type::none;
       }(&f.node), boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   BOOST_REQUIRE(shutdown.wait_for(30ms) == std::future_status::timeout);
   cancelled = boost::asio::post(executor, boost::asio::use_future([&] {
      cancellation.emit(boost::asio::cancellation_type::terminal);
   }));
   joined(cancelled);
   retry = boost::asio::co_spawn(f.runtime.context(), f.node.shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(shutdown.wait_for(30ms) == std::future_status::timeout);
   BOOST_REQUIRE(retry.wait_for(30ms) == std::future_status::timeout);
   f.identity->hold.store(false); f.identity->released.notify();
   joined(startup);
   BOOST_TEST(joined(shutdown));
   joined(retry);
   BOOST_TEST(f.identity->reads.load() == 3U);
   const auto api = f.apis.get<np::api>(np::api::ref()).shared();
   BOOST_CHECK_THROW(static_cast<void>(api->local_endpoint()), np::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_configuration_shutdown_capture_retirement_cancels_only_after_join_is_protected) {
   fixture f;
   const auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
   const auto retired = std::make_shared<retirement_result>();
   std::promise<void> stop_entered;
   auto stopping = stop_entered.get_future();
   auto capture = std::make_shared<retired_capture>();
   capture->result = retired;
   capture->on_destroy = [cancellation, &stop_entered] {
      cancellation->emit(boost::asio::cancellation_type::terminal);
      stop_entered.set_value();
   };
   auto registration = f.raw->register_protocol({"/configuration/cancel-on-retirement/1"},
       [capture](p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; });
   const auto owned = std::weak_ptr{capture};
   capture.reset();
   BOOST_REQUIRE_EQUAL(owned.use_count(), 1);
   f.initialize();
   f.identity->hold.store(true);
   auto entered = f.identity->entered.get_future();
   auto startup = boost::asio::co_spawn(f.runtime.context(), f.node.startup(), boost::asio::use_future);
   auto cleanup = boost::scope::scope_exit{[&] {
      f.identity->hold.store(false); f.identity->released.notify();
      if (startup.valid()) { joined(startup); }
   }};
   joined(entered);
   auto shutdown = boost::asio::co_spawn(boost::asio::make_strand(f.runtime.context()),
       [](np::plugin* node) -> boost::asio::awaitable<bool> {
          co_await node->shutdown();
          const auto state = co_await boost::asio::this_coro::cancellation_state;
          co_return state.cancelled() != boost::asio::cancellation_type::none;
       }(&f.node), boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_future));
   joined(stopping);
   BOOST_REQUIRE(shutdown.wait_for(30ms) == std::future_status::timeout);
   BOOST_TEST(retired->completed.load(std::memory_order_acquire));
   BOOST_CHECK(!retired->failure);
   BOOST_TEST(owned.expired());
   BOOST_TEST(!registration.active());
   BOOST_TEST(f.identity->reads.load() == 1U);
   f.identity->hold.store(false); f.identity->released.notify();
   joined(startup);
   BOOST_TEST(joined(shutdown));
}
