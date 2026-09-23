#include <boost/asio/awaitable.hpp>
#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libp2p_identity_fixture.hxx"

import forge.api.auth.authenticated_caller;
import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.p2p.binding;
import forge.api.transport.connection;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.chain.api.block_signer;
import forge.chain.api.exceptions;
import forge.chain.api.transaction_signer;
import forge.chain.protocol.block;
import forge.chain.protocol.block_signing;
import forge.chain.protocol.transaction;
import forge.chain.transaction.types;
import forge.crypto.asymmetric;
import forge.crypto.core.secret_string;
import forge.crypto.digest.sha256;
import forge.crypto.signer.configured_provider;
import forge.net.p2p.endpoint;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.stream;
import forge.plugins.chain.signer.plugin;
import forge.plugins.chain.signer.types;

namespace {

namespace chain_api = forge::chain::api;
namespace protocol = forge::chain::protocol;
namespace signer = forge::plugins::chain::signer;
namespace p2p = forge::net::p2p;

boost::asio::awaitable<void> exercise_signer_api(forge::api::transport::connection& connection,
                                                  forge::chain::transaction::unsigned_transaction transaction,
                                                  protocol::block_sign_request block,
                                                  forge::crypto::asymmetric::public_key expected_key) {
   auto transactions = co_await connection.get_remote_api<chain_api::transaction_signer>();
   auto blocks = co_await connection.get_remote_api<chain_api::block_signer>();
   const auto spoofed = forge::api::auth::authenticated_caller{
       forge::api::auth::caller_source::tls_certificate,
       forge::crypto::digest::sha256::hash(std::string{"spoofed-p2p-signer-caller"})};
   auto prepared = co_await transactions->sign(transaction, spoofed);
   BOOST_REQUIRE_EQUAL(prepared.packed.signatures.size(), 1U);
   BOOST_TEST(forge::crypto::asymmetric::recover(
                  prepared.packed.signatures.front(),
                  transaction.value.sig_digest(transaction.chain, transaction.context_free_data)) == expected_key);
   auto signatures = co_await blocks->sign(block, spoofed);
   BOOST_REQUIRE_EQUAL(signatures.size(), 1U);
   BOOST_TEST(forge::crypto::asymmetric::recover(signatures.front(), protocol::calculate_block_id(block.header)) ==
              expected_key);
   co_await connection.async_close();
}

boost::asio::awaitable<void> exercise_unlisted_signer_api(
    forge::api::transport::connection& connection,
    forge::chain::transaction::unsigned_transaction transaction,
    protocol::block_sign_request block) {
   auto transactions = co_await connection.get_remote_api<chain_api::transaction_signer>();
   auto blocks = co_await connection.get_remote_api<chain_api::block_signer>();
   auto transaction_denied = false;
   try {
      static_cast<void>(co_await transactions->sign(transaction, {}));
   } catch (const chain_api::exceptions::authorization_denied&) {
      transaction_denied = true;
   }
   BOOST_TEST(transaction_denied);
   auto block_denied = false;
   try {
      static_cast<void>(co_await blocks->sign(block, {}));
   } catch (const chain_api::exceptions::authorization_denied&) {
      block_denied = true;
   }
   BOOST_TEST(block_denied);
   co_await connection.async_close();
}

[[nodiscard]] p2p::node::options node_options(std::string_view label) {
   auto fixture = forge::tests::p2p::make_identity_fixture(label);
   return p2p::node::options{
       .certificate_pem = std::move(fixture.certificate_pem),
       .private_key_pem = std::move(fixture.private_key_pem),
       .peer_state = {.persistence = p2p::peer_store::make_memory_persistence()},
       .allow_insecure_test_mode = false,
   };
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_authenticated_quic_roundtrips_both_chain_signer_methods) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = p2p::node{runtime, node_options("signer-live-server")};
   auto client = p2p::node{runtime, node_options("signer-live-client")};
   auto unlisted = p2p::node{runtime, node_options("signer-live-unlisted")};
   const auto chain = forge::crypto::digest::sha256::hash(std::string{"p2p-live-signer-chain"});
   const auto private_key = forge::crypto::asymmetric::private_key::regenerate(
       forge::crypto::digest::sha256::hash(std::string{"p2p-live-signer-key"}));
   const auto public_key = private_key.get_public_key();
   const auto provider = forge::crypto::signer::configured_provider::from_private_key(
       {.value = "producer-key"},
       forge::crypto::core::secret_string{forge::crypto::asymmetric::encoding::forge().format(private_key)});
   const auto fingerprint = forge::crypto::digest::sha256::hash(client.local_peer().to_bytes());
   auto settings = signer::config{};
   settings.transaction_profiles.push_back({
       .name = "writer-transaction",
       .chain_id = chain.str(),
       .signing = {.provider = "k1", .key_id = "producer-key",
                   .expected_public_key = forge::crypto::asymmetric::encoding::forge().format(public_key)},
       .authorization = signer::remote_authorization::profile_only,
       .callers = {{.source = forge::api::auth::caller_source::p2p_peer, .fingerprint = fingerprint.str()}},
       .actions = {{.account = "storage", .action = "write", .actor = "writer", .permission = "active"}},
   });
   settings.block_profiles.push_back({
       .name = "writer-block",
       .chain_id = chain.str(),
       .producer = "writer",
       .signing = {{.provider = "k1", .key_id = "producer-key",
                    .expected_public_key = forge::crypto::asymmetric::encoding::forge().format(public_key)}},
       .callers = {{.source = forge::api::auth::caller_source::p2p_peer, .fingerprint = fingerprint.str()}},
   });
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto registry = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = signer::plugin{signer::plugin_options{
       .providers = {{.name = "k1", .value = provider}},
       .initial_config = std::move(settings),
       .now = [] { return protocol::time_point_sec{1'700'000'000U}; },
   }};
   auto installer = forge::api::core::installer{registry};
   forge::asio::blocking::run(runtime, plugin.provide(installer));
   auto context = forge::app::plugin_context{scheduler, registry, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));
   forge::asio::blocking::run(runtime, plugin.startup());
   auto binding = forge::api::p2p::api(server).use(forge::api::core::binding().serve(registry).build()).build();
   server.register_protocol_handler(binding.protocol(), binding.handler());

   forge::asio::blocking::run(runtime, server.async_listen(p2p::endpoint{
                                           .transport = {.host_type = p2p::endpoint::host_kind::ip4,
                                                         .protocol = p2p::endpoint::protocol_kind::quic_v1,
                                                         .host = "127.0.0.1", .port = 0}}));
   const auto endpoint = server.local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   static_cast<void>(forge::asio::blocking::run(
       runtime, client.async_connect(*endpoint, p2p::node::connect_options{
                                                    .expected_peer = server.local_peer(),
                                                    .allow_relay = false,
                                                })));
   auto stream = forge::asio::blocking::run(runtime, client.async_open_protocol_stream(
                                                       server.local_peer(), binding.protocol(),
                                                       p2p::node::open_options{.allow_relay = false}));
   BOOST_TEST(static_cast<std::uint8_t>(stream.authentication()) ==
              static_cast<std::uint8_t>(p2p::peer_authentication::quic_tls));
   auto connection = forge::api::transport::connection{std::move(stream).into_transport_stream(), binding.options()};
   auto transaction = forge::chain::transaction::unsigned_transaction{};
   transaction.chain = chain;
   transaction.value.expiration = protocol::time_point_sec{1'700'000'030U};
   auto action = protocol::action{};
   action.account = protocol::account_name{"storage"};
   action.name = protocol::action_name{"write"};
   action.authorization = {{.actor = protocol::account_name{"writer"},
                            .permission = protocol::permission_name{"active"}}};
   transaction.value.actions.push_back(std::move(action));
   auto header = protocol::block_header{};
   header.producer = protocol::account_name{"writer"};
   auto block = protocol::block_sign_request{.chain = chain, .header = std::move(header), .keys = {public_key}};
   forge::asio::blocking::run(runtime, exercise_signer_api(connection, transaction, block, public_key));
   static_cast<void>(forge::asio::blocking::run(
       runtime, unlisted.async_connect(*endpoint, p2p::node::connect_options{
                                                        .expected_peer = server.local_peer(),
                                                        .allow_relay = false,
                                                    })));
   auto unlisted_stream = forge::asio::blocking::run(runtime, unlisted.async_open_protocol_stream(
                                                            server.local_peer(), binding.protocol(),
                                                            p2p::node::open_options{.allow_relay = false}));
   BOOST_TEST(static_cast<std::uint8_t>(unlisted_stream.authentication()) ==
              static_cast<std::uint8_t>(p2p::peer_authentication::quic_tls));
   auto unlisted_connection = forge::api::transport::connection{
       std::move(unlisted_stream).into_transport_stream(), binding.options()};
   forge::asio::blocking::run(runtime, exercise_unlisted_signer_api(unlisted_connection, transaction, block));
   forge::asio::blocking::run(runtime, unlisted.async_stop());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   plugin.request_stop();
   forge::asio::blocking::run(runtime, plugin.shutdown());
}
