#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
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

import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin_context;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.document;
import forge.config.core.value;
import forge.multiformats.multiaddr;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.stream;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.net.p2p.node.plugin;

namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;
using namespace std::chrono_literals;

template<class T> T joined(std::future<T>& value) {
   if (value.wait_for(10s) != std::future_status::ready) {
      std::cerr << "peer API fixture did not join its native operation\n";
      std::terminate();
   }
   return value.get();
}

class identity_source final : public secrets::api {
 public:
   forge::tests::p2p::identity_fixture identity = forge::tests::p2p::make_identity_fixture("peer-api");
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override { co_return secrets::snapshot{.configured_secrets = 2}; }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      const auto* bytes = request.secret_id == "peer/cert" ? &identity.certificate_pem
          : request.secret_id == "peer/key" ? &identity.private_key_pem : nullptr;
      if (!bytes) { throw std::invalid_argument{"unknown peer fixture secret"}; }
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
   std::shared_ptr<np::api> api;
   std::shared_ptr<np::diagnostics_api> diagnostics;

   explicit fixture(std::optional<p2p::endpoint> bootstrap = {}) {
      forge::config::core::document config;
      // Insecure in-memory lifecycle fixture; not an expected-peer verification fixture.
      config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
      config.set("plugins.net.p2p.node.identity.certificate-secret", "peer/cert");
      config.set("plugins.net.p2p.node.identity.private-key-secret", "peer/key");
      config.set("plugins.net.p2p.node.topology.mode", "static-only");
      config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{"/ip4/127.0.0.1/tcp/0"});
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
      api = apis.get<np::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 3}).shared();
      diagnostics = apis.get<np::diagnostics_api>(
          {.id = {"forge.plugins.net.p2p.node.diagnostics"}, .major = 1}).shared();
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
};

void canceled_native_dial(bool stop_owner) {
   fixture f;
   f.run(f.node.startup());
   auto strand = boost::asio::make_strand(f.runtime.context());
   auto listener = boost::asio::ip::tcp::acceptor{strand, {boost::asio::ip::address_v4::loopback(), 0}};
   auto accepted = listener.async_accept(boost::asio::use_future);
   auto endpoint = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/" + std::to_string(listener.local_endpoint().port()));
   boost::asio::cancellation_signal cancel;
   auto result = boost::asio::co_spawn(strand, f.api->connect(endpoint,
       {.allow_relay = false, .timeout = 5s, .direct_attempt_timeout = 5s, .allow_hole_punch = false}),
       boost::asio::bind_cancellation_slot(cancel.slot(), boost::asio::use_future));
   std::optional<boost::asio::ip::tcp::socket> remote;
   auto cleanup = boost::scope::scope_exit{[&] {
      f.node.request_stop();
      // No accepted-side IO was started. All client work must join before its slot dies.
      auto close_done = std::promise<void>{};
      auto closed = close_done.get_future();
      boost::asio::post(strand, [&] {
         boost::system::error_code ignored;
         listener.close(ignored);
         if (remote) { remote->close(ignored); }
         close_done.set_value();
      });
      joined(closed);
      if (accepted.valid()) {
         try { static_cast<void>(joined(accepted)); }
         catch (const std::exception& error) { std::cerr << "pending accept cleanup: " << error.what() << '\n'; }
      }
      if (result.valid()) {
         try { static_cast<void>(joined(result)); }
         catch (const std::exception& error) { std::cerr << "pending dial cleanup: " << error.what() << '\n'; }
      }
   }};
   remote.emplace(joined(accepted));
   BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::timeout);
   // The real TCP socket was accepted, but the remote never answers security negotiation.
   if (stop_owner) { f.node.request_stop(); }
   else {
      auto signaled = std::promise<void>{};
      auto signal_done = signaled.get_future();
      boost::asio::post(strand, [&] { cancel.emit(boost::asio::cancellation_type::all); signaled.set_value(); });
      joined(signal_done);
   }
   BOOST_CHECK_EXCEPTION(joined(result), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::is(error, p2p::exceptions::code::canceled) ||
             p2p::exceptions::is(error, p2p::exceptions::code::closed);
   });
   BOOST_TEST(f.diagnostics->snapshot().connections.active_sessions == 0U);
}
}

BOOST_AUTO_TEST_CASE(p2p_peer_api_connect_endpoint_multiaddr_and_ping) {
   fixture server, client;
   server.run(server.node.startup());
   client.run(client.node.startup());
   BOOST_REQUIRE(server.api->local_endpoint());
   const auto endpoint = *server.api->local_endpoint();
   const auto peer = server.api->local_peer();
   BOOST_REQUIRE(client.api->local_peer() != peer);
   const auto connected = client.run(client.api->connect(endpoint, {.allow_relay = false}));
   BOOST_CHECK(connected.remote_peer == peer);
   const auto again = client.run(client.api->connect(endpoint.to_multiaddr(),
                                                    {.allow_relay = false}));
   BOOST_CHECK(again.remote_peer == peer);
   BOOST_TEST(client.run(client.api->ping(peer, {.allow_relay = false})).count() >= 0);
   const auto state = client.diagnostics->snapshot();
   BOOST_CHECK(std::ranges::any_of(state.sessions, [&](const auto& value) {
      return value.id == connected.id && value.remote_peer == peer && !value.closed &&
             value.muxer.value == "/yamux/1.0.0";
   }));
}

BOOST_AUTO_TEST_CASE(p2p_peer_api_admission_and_deferred_awaitables_retain_their_owner) {
   fixture executor;
   const auto endpoint = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/1");
   const auto peer = p2p::peer_id{"not-running"};
   BOOST_CHECK_THROW(executor.run(executor.api->connect(endpoint)), np::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW(executor.run(executor.api->ping(peer)), np::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)executor.api->protect_peer(peer), np::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)executor.api->tag_peer(peer, 0), np::exceptions::plugin_not_initialized);
   auto owner = std::make_unique<fixture>();
   owner->run(owner->node.startup());
   auto api = owner->api;
   auto by_endpoint = api->connect(endpoint);
   auto by_address = api->connect(endpoint.to_multiaddr());
   auto ping = api->ping(peer);
   auto protection = api->protect_peer(peer);
   owner.reset();
   BOOST_TEST(!protection.active());
   BOOST_CHECK_THROW((void)api->tag_peer(peer, 1), p2p::exceptions::canceled);
   api.reset();
   BOOST_CHECK_THROW(executor.run(std::move(by_endpoint)), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(executor.run(std::move(by_address)), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(executor.run(std::move(ping)), p2p::exceptions::canceled);
   protection.close();
}

BOOST_AUTO_TEST_CASE(p2p_peer_api_two_owners_moves_bounds_and_bootstrap_protection) {
   fixture server;
   server.run(server.node.startup());
   BOOST_REQUIRE(server.api->local_endpoint());
   fixture client{*server.api->local_endpoint()};
   client.run(client.node.startup());
   const auto peer = server.api->local_peer();
   auto first = client.api->protect_peer(peer);
   auto second = client.api->protect_peer(peer);
   first.close();
   BOOST_TEST(second.active());
   auto moved = std::move(second);
   BOOST_TEST(!second.active());
   moved.close();
   moved.close();
   BOOST_TEST(client.api->is_peer_protected(peer)); // Bootstrap is an independent native owner.
   const auto other = client.api->local_peer();
   std::vector<np::peer_contribution> tokens;
   for (unsigned i = 0; i < 16; ++i) {
      tokens.push_back(i % 2 ? client.api->protect_peer(other) : client.api->tag_peer(other, i));
   }
   BOOST_CHECK_THROW((void)client.api->protect_peer(other), p2p::exceptions::backpressure_rejected);
   tokens.front().close();
   auto replacement = client.api->tag_peer(other, -1);
   client.node.request_stop();
   for (const auto& token : tokens) { BOOST_TEST(!token.active()); }
   BOOST_TEST(!replacement.active());
   BOOST_CHECK_THROW((void)client.api->is_peer_protected(peer), p2p::exceptions::canceled);
   BOOST_CHECK_THROW((void)client.api->protect_peer(peer), p2p::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(p2p_peer_api_facade_destruction_closes_late_handle_admission) {
   fixture executor;
   forge::api::core::registry apis;
   auto facade = std::make_unique<np::plugin>();
   auto installer = forge::api::core::installer{apis};
   executor.run(facade->provide(installer));
   auto api = apis.get<np::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 3}).shared();
   auto saved = api->connect(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/1"));
   facade.reset(); // No explicit request_stop; the facade closes its own admission.
   BOOST_CHECK_THROW((void)api->protect_peer({"later"}), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(executor.run(std::move(saved)), p2p::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(p2p_peer_api_contribution_admission_and_stop_are_serialized) {
   fixture f;
   f.run(f.node.startup());
   auto start = std::barrier{2};
   std::vector<np::peer_contribution> tokens;
   std::exception_ptr unexpected;
   std::promise<void> done;
   auto finished = done.get_future();
   std::thread worker{[&] {
      start.arrive_and_wait();
      try {
         for (unsigned i = 0; i < 16; ++i) {
            tokens.push_back(f.api->protect_peer({"concurrent-" + std::to_string(i)}));
         }
      } catch (const p2p::exceptions::canceled&) {}
      catch (...) { unexpected = std::current_exception(); }
      done.set_value();
   }};
   start.arrive_and_wait();
   f.node.request_stop();
   joined(finished);
   worker.join();
   if (unexpected) { std::rethrow_exception(unexpected); }
   for (const auto& token : tokens) { BOOST_TEST(!token.active()); }
   BOOST_CHECK_THROW((void)f.api->tag_peer({"later"}, 0), p2p::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(p2p_peer_api_caller_cancellation_joins_an_actual_pending_native_dial) {
   canceled_native_dial(false);
}
BOOST_AUTO_TEST_CASE(p2p_peer_api_stop_joins_an_actual_pending_native_dial) {
   canceled_native_dial(true);
}
