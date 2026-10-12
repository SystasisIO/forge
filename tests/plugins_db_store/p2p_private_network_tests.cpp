#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.registry;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.document;
import forge.config.core.value;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.connection_gater;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.stream;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.db.store.plugin;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;

namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;
using value = forge::config::core::value;
using namespace std::chrono_literals;

std::vector<std::string> backends() {
   auto result = std::vector<std::string>{};
#if FORGE_HAS_MDBX
   result.emplace_back("mdbx");
#endif
#if FORGE_HAS_ROCKSDB
   result.emplace_back("rocksdb");
#endif
   return result;
}

struct test_directory {
   const std::filesystem::path path = std::filesystem::temp_directory_path() /
       ("forge-private-plugin-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   test_directory() {
      if (!std::filesystem::create_directory(path)) { throw std::runtime_error{"private fixture directory collision"}; }
   }
   ~test_directory() {
      auto error = std::error_code{};
      std::filesystem::remove_all(path, error);
      if (error) { BOOST_ERROR("private fixture cleanup: " << error.message()); }
   }
};

value secret(std::string id, std::string material, std::string purpose, std::string encoding = "raw",
             bool exportable = true) {
   return value::object_type{
       {"id", std::move(id)}, {"kind", "bytes"},
       {"source", value::object_type{{"type", "value"}, {"encoding", std::move(encoding)}, {"value", std::move(material)}}},
       {"purposes", value::array_type{value{std::move(purpose)}}},
       {"operations", value::array_type{value{"get_bytes"}}}, {"allow-raw-export", exportable}};
}

struct secret_policy {
   std::string hex = std::string(64, '1');
   std::string purpose = "p2p.private-network.psk";
   bool exportable = true;
   bool include = true;
   bool private_profile = true;
};

forge::config::core::document document(const std::filesystem::path& path, std::string backend,
                                       const forge::tests::p2p::identity_fixture& identity,
                                       const secret_policy& policy, std::string security) {
   auto config = forge::config::core::document{};
   auto entries = value::array_type{
       secret("identity/cert", identity.certificate_pem, "p2p.identity.certificate"),
       secret("identity/key", identity.private_key_pem, "p2p.identity.private-key")};
   if (policy.include) {
      entries.push_back(secret("network/psk", policy.hex, policy.purpose, "hex", policy.exportable));
   }
   config.set("plugins.crypto.secrets.secrets", std::move(entries));
   config.set("plugins.db.store.stores", value::array_type{value::object_type{
       {"name", "peers"}, {"driver", std::move(backend)}, {"path", path.string()},
       {"object", value::object_type{{"family", "objectdb"}, {"write-policy", "single-writer"}}}}});
   config.set("plugins.net.p2p.node.peer-store.store", "peers");
   config.set("plugins.net.p2p.node.identity.certificate-secret", "identity/cert");
   config.set("plugins.net.p2p.node.identity.private-key-secret", "identity/key");
   config.set("plugins.net.p2p.node.allow-insecure-test-mode", false);
   config.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/127.0.0.1/tcp/0"}});
   config.set("plugins.net.p2p.node.topology.mode", "static-only");
   config.set("plugins.net.p2p.node.tcp.security", std::move(security));
   config.set("plugins.net.p2p.node.path.policy", "direct-only");
   config.set("plugins.net.p2p.node.relay.client-enabled", false);
   config.set("plugins.net.p2p.node.relay.server-enabled", false);
   config.set("plugins.net.p2p.node.relay.auto-discovery-enabled", false);
   if (policy.private_profile) { config.set("plugins.net.p2p.node.private-network.psk-secret", "network/psk"); }
   return config;
}

struct application {
   test_directory directory;
   const forge::tests::p2p::identity_fixture identity = forge::tests::p2p::make_identity_fixture("private-plugin");
   std::unique_ptr<forge::app::application_shell> shell;

   application(const std::string& backend, secret_policy policy = {}, std::string security = "noise") {
      auto builder = forge::app::application_builder{};
      builder.name("private-plugin-test")
          .runtime(forge::asio::runtime_options{.worker_threads = 2})
          .plugin(forge::plugins::db::store::plugin::descriptor())
          .plugin(secrets::plugin::descriptor())
          .plugin(np::plugin::descriptor());
      shell = std::move(builder).build();
      shell->configure(document(directory.path / "peers", backend, identity, policy, std::move(security)));
   }
   template<class T> T run(boost::asio::awaitable<T> operation) {
      auto future = boost::asio::co_spawn(shell->runtime().context(), std::move(operation), boost::asio::use_future);
      if (future.wait_for(10s) != std::future_status::ready) {
         std::cerr << "private plugin fixture exceeded its join budget\n";
         std::terminate();
      }
      return future.get();
   }
   void start() { run(shell->startup()); }
   template<class T> auto api() { return shell->apis().get<T>(T::ref()).shared(); }
   ~application() {
      shell->request_stop();
      // Always join the scheduler even if plugin shutdown reports a failure.
      auto failure = std::exception_ptr{};
      for (auto step = 0; step != 2; ++step) {
         try {
            if (step == 0) { run(shell->shutdown()); }
            else { run(shell->scheduler().shutdown()); }
         } catch (...) { if (!failure) { failure = std::current_exception(); } }
      }
      if (failure) { BOOST_ERROR("private plugin shutdown failed after joining the scheduler"); }
   }
};

class accepting_gater final : public p2p::connection_gater {
 public:
   std::atomic_size_t accepted{0};
   std::atomic_size_t secured{0};
   bool intercept_accept(const p2p::connection_endpoints&) noexcept override {
      ++accepted;
      return true;
   }
   bool intercept_secured(p2p::connection_direction direction, const p2p::peer_id&,
                           const p2p::connection_endpoints&) noexcept override {
      if (direction == p2p::connection_direction::inbound) { ++secured; }
      return true;
   }
};

boost::asio::awaitable<bool> await_rejection(std::shared_ptr<np::diagnostics_api> diagnostics,
                                             std::shared_ptr<accepting_gater> witness, std::size_t previous,
                                             std::uint64_t failures) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   do {
      if (witness->accepted.load() > previous && diagnostics->resources().system.inbound_connections == 0 &&
          diagnostics->snapshot().metrics.handshakes_failed > failures) {
         co_return true;
      }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   co_return false;
}

void exchange(application& client, application& server, const p2p::protocol_id& protocol,
              p2p::peer_authentication authentication) {
   auto remote = server.api<np::api>();
   const auto endpoint = remote->local_endpoint();
   BOOST_REQUIRE(endpoint);
   const auto session = client.run(client.api<np::api>()->connect(*endpoint, {
       .expected_peer = remote->local_peer(), .allow_relay = false, .timeout = 2s, .allow_hole_punch = false}));
   BOOST_CHECK(session.identify_state == p2p::identify::state::identified);
   BOOST_CHECK(session.remote_peer == remote->local_peer());
   BOOST_TEST(session.muxer.value == "/yamux/1.0.0");
   auto stream = client.run(client.api<np::protocol_api>()->open(remote->local_peer(), protocol,
       {.allow_relay = false, .timeout = 2s, .allow_hole_punch = false}));
   const auto payload = std::vector<std::uint8_t>{0, 0xff, 2, 0, 4, 5};
   client.run(stream.async_write_frame(payload));
   BOOST_CHECK(client.run(stream.async_read_frame()) == payload);
   client.run(stream.async_close());
   const auto state = client.api<np::diagnostics_api>()->snapshot();
   BOOST_REQUIRE_EQUAL(state.sessions.size(), 1U);
   BOOST_CHECK(state.sessions.front().identify_state == p2p::identify::state::identified);
   BOOST_CHECK(state.sessions.front().authentication == authentication);
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_private_plugin_test_suite,
                      *boost::unit_test::enable_if<FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB>())

BOOST_AUTO_TEST_CASE(real_secrets_permissions_and_decoded_key_size_reject_startup) {
   for (const auto& backend : backends()) {
      BOOST_TEST_CONTEXT(backend) {
         for (const auto size : {31U, 33U}) {
            application app{backend, {.hex = std::string(size * 2, '1')}};
            BOOST_CHECK_THROW(app.start(), np::exceptions::invalid_config);
            BOOST_CHECK_THROW(static_cast<void>(app.api<np::api>()->local_endpoint()), np::exceptions::plugin_not_initialized);
         }
         application missing{backend, {.include = false}};
         BOOST_CHECK_THROW(missing.start(), secrets::exceptions::secret_not_found);
         BOOST_CHECK_THROW(static_cast<void>(missing.api<np::api>()->local_endpoint()), np::exceptions::plugin_not_initialized);
         application purpose{backend, {.purpose = "another-purpose"}};
         BOOST_CHECK_THROW(purpose.start(), secrets::exceptions::purpose_denied);
         BOOST_CHECK_THROW(static_cast<void>(purpose.api<np::api>()->local_endpoint()), np::exceptions::plugin_not_initialized);
         application denied{backend, {.exportable = false}};
         BOOST_CHECK_THROW(denied.start(), secrets::exceptions::operation_denied);
         BOOST_CHECK_THROW(static_cast<void>(denied.api<np::api>()->local_endpoint()), np::exceptions::plugin_not_initialized);
      }
   }
}

BOOST_AUTO_TEST_CASE(secure_tcp_same_key_authenticates_and_wrong_or_missing_key_does_not_poison_listener) {
   for (const auto& backend : backends()) {
      for (const auto* security : {"noise", "tls"}) {
         BOOST_TEST_CONTEXT(backend << "/" << security) {
            application server{backend, {}, security};
            server.run(server.shell->initialize());
            auto witness = std::make_shared<accepting_gater>();
            server.api<np::configuration_api>()->set_connection_gater(witness);
            server.start();
            const auto protocol = p2p::protocol_id{"/stage7/private-plugin/echo/1"};
            auto registration = server.api<np::protocol_api>()->register_protocol(protocol,
                [](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                   const auto bytes = co_await incoming.stream.async_read_frame();
                   co_await incoming.stream.async_write_frame(bytes);
                   co_await incoming.stream.async_close();
                });
            const auto remote = server.api<np::api>();
            const auto endpoint = remote->local_endpoint();
            BOOST_REQUIRE(endpoint);
            for (const auto& rejected : {secret_policy{.hex = std::string(64, '2')},
                                          secret_policy{.private_profile = false}}) {
               application client{backend, rejected, security};
               client.start();
               const auto accepted_before = witness->accepted.load();
               const auto server_before = server.api<np::diagnostics_api>()->snapshot().metrics;
               BOOST_CHECK_THROW(client.run(client.api<np::api>()->connect(*endpoint,
                   {.expected_peer = remote->local_peer(), .allow_relay = false, .timeout = 500ms,
                    .direct_attempt_timeout = 500ms, .allow_hole_punch = false})), forge::exceptions::base);
               BOOST_REQUIRE(server.run(await_rejection(server.api<np::diagnostics_api>(), witness, accepted_before,
                                                        server_before.handshakes_failed)));
               BOOST_TEST(witness->secured.load() == 0U);
               const auto server_after = server.api<np::diagnostics_api>()->snapshot().metrics;
               BOOST_TEST(server_after.handshakes_completed == server_before.handshakes_completed);
               BOOST_TEST(server_after.sessions_opened == server_before.sessions_opened);
               const auto client_after = client.api<np::diagnostics_api>()->snapshot().metrics;
               BOOST_TEST(client_after.handshakes_completed == 0U);
               BOOST_TEST(client_after.sessions_opened == 0U);
               BOOST_TEST(client.api<np::diagnostics_api>()->snapshot().sessions.empty());
               BOOST_TEST(server.api<np::diagnostics_api>()->snapshot().sessions.empty());
            }
            application accepted{backend, {}, security};
            accepted.start();
            exchange(accepted, server, protocol, std::string_view{security} == "noise"
                ? p2p::peer_authentication::noise : p2p::peer_authentication::libp2p_tls);
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
