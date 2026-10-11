#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.handle;
import forge.api.core.descriptor;
import forge.api.core.registry;
import forge.api.core.types;
import forge.app.application_shell;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.chrono.timestamp;
import forge.config.core.document;
import forge.config.core.value;
import forge.multiformats.multihash;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.ipns;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.net.p2p.stream;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.plugin;

namespace {

namespace p2p = forge::net::p2p;
namespace node_plugin = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;

const auto amino = p2p::protocol_id{.value = "/ipfs/kad/1.0.0"};
const auto providers = p2p::protocol_id{.value = "/forge/test/providers/1.0.0"};

class identity_api final : public secrets::api {
 public:
   explicit identity_api(forge::tests::p2p::identity_fixture identity) : identity_{std::move(identity)} {}

   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override {
      co_return secrets::snapshot{.configured_secrets = 2};
   }

   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      const auto* value = request.secret_id == "dht/certificate" ? &identity_.certificate_pem
                          : request.secret_id == "dht/private-key" ? &identity_.private_key_pem : nullptr;
      if (!value) {
         throw std::invalid_argument{"unknown DHT test secret"};
      }
      co_return secrets::get_result{.secret_id = std::move(request.secret_id),
                                     .bytes = {value->begin(), value->end()}};
   }

   boost::asio::awaitable<secrets::derive_result> derive_hkdf_sha256(secrets::derive_request) override {
      throw std::logic_error{"DHT test identity cannot derive secrets"};
      co_return secrets::derive_result{};
   }
   boost::asio::awaitable<secrets::aead_encrypt_result> encrypt_aes_gcm(secrets::aead_encrypt_request) override {
      throw std::logic_error{"DHT test identity cannot encrypt"};
      co_return secrets::aead_encrypt_result{};
   }
   boost::asio::awaitable<secrets::aead_decrypt_result> decrypt_aes_gcm(secrets::aead_decrypt_request) override {
      throw std::logic_error{"DHT test identity cannot decrypt"};
      co_return secrets::aead_decrypt_result{};
   }

 private:
   forge::tests::p2p::identity_fixture identity_;
};

// Only the dependency slots are test doubles; the node, identity and DHT are native.
class dependency_plugin final : public forge::app::plugin {
 public:
   explicit dependency_plugin(std::string id, std::shared_ptr<identity_api> identity = {})
       : id_{std::move(id)}, identity_{std::move(identity)} {}
   forge::app::plugin_id id() const override { return {.value = id_}; }
   std::string version() const override { return "test"; }
   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override {
      if (identity_) {
         provider.install<secrets::api>(identity_);
      }
      co_return;
   }
   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override { co_return; }
   boost::asio::awaitable<void> startup() override { co_return; }
   boost::asio::awaitable<void> shutdown() override { co_return; }

 private:
   std::string id_;
   std::shared_ptr<identity_api> identity_;
};

class dht_application final : public forge::app::application_shell {
 public:
   explicit dht_application(std::string name)
       : identity_{std::make_shared<identity_api>(forge::tests::p2p::make_identity_fixture(name))} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin({
          .id = {.value = "forge.plugins.db.store"},
          .factory = [] { return std::make_unique<dependency_plugin>("forge.plugins.db.store"); },
      });
      registry.register_plugin({
          .id = {.value = "forge.plugins.crypto.secrets"},
          .factory = [identity = identity_] {
             return std::make_unique<dependency_plugin>("forge.plugins.crypto.secrets", identity);
          },
      });
      registry.register_plugin(node_plugin::descriptor());
   }

 private:
   std::shared_ptr<identity_api> identity_;
};

forge::config::core::document configuration(std::string mode = "server") {
   using value = forge::config::core::value;
   auto config = forge::config::core::document{};
   // Memory persistence is test-only; TCP still authenticates the real certificate identity.
   config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
   config.set("plugins.net.p2p.node.identity.certificate-secret", "dht/certificate");
   config.set("plugins.net.p2p.node.identity.private-key-secret", "dht/private-key");
   config.set("plugins.net.p2p.node.topology.mode", "static-only");
   config.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/127.0.0.1/tcp/0"}});
   config.set("plugins.net.p2p.node.dht.profiles", value::array_type{
       value{value::object_type{{"kind", "amino-v1"}, {"mode", mode}, {"protocol", amino.value},
                                {"peers", true}, {"providers", true}, {"values", true}}},
       value{value::object_type{{"kind", "custom"}, {"mode", mode}, {"protocol", providers.value},
                                {"peers", true}, {"providers", true}, {"values", false}}},
   });
   return config;
}

auto dht_handle(dht_application& app) {
   return app.apis().get<node_plugin::dht_api>(
       {.id = {"forge.plugins.net.p2p.node.dht"}, .major = 1, .min_revision = 0});
}

auto node_handle(dht_application& app) {
   return app.apis().get<node_plugin::api>(
       {.id = {"forge.plugins.net.p2p.node"}, .major = 2, .min_revision = 0});
}

boost::asio::awaitable<bool> wait_for_dht_server(std::shared_ptr<node_plugin::diagnostics_source> diagnostics,
                                                p2p::peer_id remote, p2p::protocol_id protocol = providers) {
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   for (;;) {
      const auto snapshot = diagnostics->snapshot();
      const auto identified = std::ranges::any_of(snapshot.sessions, [&](const auto& session) {
         return session.remote_peer == remote && !session.closed &&
                session.identify_state == p2p::identify::state::identified &&
                session.authentication == p2p::peer_authentication::libp2p_tls &&
                session.muxer.value == "/yamux/1.0.0";
      });
      const auto advertised = std::ranges::any_of(snapshot.peers, [&](const auto& peer) {
         return peer.peer == remote && std::ranges::find(peer.protocols, protocol) != peer.protocols.end();
      });
      if (identified && advertised) {
         co_return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         co_return false;
      }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{10}));
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
}

template <typename Exception>
void check_closed_admission(forge::asio::runtime& runtime, node_plugin::dht_api& api) {
   const auto peer = p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
                                       .data = std::vector<std::uint8_t>(32, 42)});
   const auto key = p2p::make_dht_key(std::vector<std::uint8_t>{1, 2, 3});
   const auto value = std::vector<std::uint8_t>{'/', 'i', 'p', 'f', 's', '/', 'b', 'a', 'f', 'k', 'q', 'a', 'a', 'a'};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api.find_peer(amino, peer)), Exception);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api.provide(amino, key)), Exception);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api.find_providers(amino, key)), Exception);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api.put_value(amino, {.key_value = key, .value = value})),
                     Exception);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api.get_value(amino, key)), Exception);
   BOOST_CHECK_THROW((void)api.create_ipns_record(value, 1, forge::chrono::timestamp{}, std::chrono::seconds{1}), Exception);
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_dht_api_contract_is_local_and_rejects_before_startup) {
   auto runtime = forge::asio::runtime{};
   auto plugin = node_plugin::plugin{};
   auto apis = forge::api::core::registry{};
   auto installer = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(installer));
   auto dht = apis.get<node_plugin::dht_api>(
       {.id = {"forge.plugins.net.p2p.node.dht"}, .major = 1, .min_revision = 0});
   BOOST_CHECK(apis.describe({.id = {"forge.plugins.net.p2p.node"}, .major = 2, .min_revision = 0}) != nullptr);
   BOOST_CHECK(apis.describe({.id = {"forge.plugins.net.p2p.node.diagnostics_source"}, .major = 2,
                              .min_revision = 0}) != nullptr);
   BOOST_CHECK(apis.describe({.id = {"forge.plugins.net.p2p.node.pubsub_source"}, .major = 2,
                              .min_revision = 0}) != nullptr);
   BOOST_TEST(node_plugin::dht_api::describe().methods.empty());
   BOOST_CHECK(node_plugin::dht_api::describe().supported_surfaces == forge::api::core::surface::local);
   check_closed_admission<node_plugin::exceptions::plugin_not_initialized>(runtime, *dht.shared());
}

BOOST_AUTO_TEST_CASE(p2p_dht_api_rejects_at_request_stop_and_after_shutdown) {
   auto app = dht_application{"dht-plugin-admission"};
   app.configure(configuration());
   forge::asio::blocking::run(app.runtime(), app.initialize());
   auto cleanup = boost::scope::scope_exit{[&] { forge::asio::blocking::run(app.runtime(), app.shutdown()); }};
   auto dht = dht_handle(app);
   check_closed_admission<node_plugin::exceptions::plugin_not_initialized>(app.runtime(), *dht.shared());
   forge::asio::blocking::run(app.runtime(), app.startup());
   const auto key = p2p::make_dht_key(std::vector<std::uint8_t>{4, 5, 6});
   auto deferred = dht->find_providers(amino, key);
   app.request_stop();
   // The facade still exists here. Admission must not depend on node reset by shutdown().
   BOOST_CHECK_NO_THROW((void)node_handle(app)->local_peer());
   check_closed_admission<p2p::exceptions::canceled>(app.runtime(), *dht.shared());
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), std::move(deferred)), p2p::exceptions::canceled);
   forge::asio::blocking::run(app.runtime(), app.shutdown());
   cleanup.set_active(false);
   check_closed_admission<p2p::exceptions::canceled>(app.runtime(), *dht.shared());
}

BOOST_AUTO_TEST_CASE(p2p_dht_api_saved_awaitables_survive_api_and_host_destruction) {
   auto find = std::optional<boost::asio::awaitable<p2p::dht::query_result>>{};
   auto provide = std::optional<boost::asio::awaitable<p2p::provider_registration>>{};
   auto providers_found = std::optional<boost::asio::awaitable<std::vector<p2p::dht::peer>>>{};
   auto put = std::optional<boost::asio::awaitable<p2p::dht::value_put_result>>{};
   auto get = std::optional<boost::asio::awaitable<p2p::dht::value_get_result>>{};
   auto released_api = std::weak_ptr<node_plugin::dht_api>{};
   {
      auto app = dht_application{"dht-plugin-saved-awaitables"};
      app.configure(configuration());
      forge::asio::blocking::run(app.runtime(), app.startup());
      auto cleanup = boost::scope::scope_exit{[&] { forge::asio::blocking::run(app.runtime(), app.shutdown()); }};
      const auto dht = dht_handle(app);
      released_api = dht.shared();
      const auto peer = node_handle(app)->local_peer();
      const auto key = p2p::make_dht_key(std::vector<std::uint8_t>{1, 2, 3});
      find.emplace(dht->find_peer(amino, peer));
      provide.emplace(dht->provide(amino, key));
      providers_found.emplace(dht->find_providers(amino, key));
      put.emplace(dht->put_value(amino, {.key_value = key, .value = {1}}));
      get.emplace(dht->get_value(amino, key));
      forge::asio::blocking::run(app.runtime(), app.shutdown());
      cleanup.set_active(false);
   }
   BOOST_REQUIRE(released_api.expired());
   // These operations have never resumed; even their original runtime is gone.
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(*find)), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(*provide)), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(*providers_found)), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(*put)), p2p::exceptions::canceled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(*get)), p2p::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(p2p_dht_api_amino_pk_storage_and_ipns_use_the_host_identity) {
   auto app = dht_application{"dht-plugin-local-records"};
   app.configure(configuration());
   forge::asio::blocking::run(app.runtime(), app.startup());
   auto cleanup = boost::scope::scope_exit{[&] { forge::asio::blocking::run(app.runtime(), app.shutdown()); }};
   const auto dht = dht_handle(app);
   const auto peer = node_handle(app)->local_peer();
   const auto value = std::vector<std::uint8_t>{'/', 'i', 'p', 'f', 's', '/', 'b', 'a', 'f', 'k', 'q', 'a', 'a', 'a'};
   const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
   const auto signed_record = dht->create_ipns_record(value, 7, forge::chrono::timestamp{now + std::chrono::hours{1}},
                                                     std::chrono::minutes{1}, {.embed_public_key = true});
   BOOST_CHECK_NO_THROW(p2p::ipns::validate(signed_record, peer, std::optional<p2p::public_key>{},
                                            forge::chrono::timestamp{now}));
   BOOST_TEST(signed_record.sequence() == 7U);
   BOOST_CHECK(std::ranges::equal(signed_record.value(), value));
   const auto public_key = signed_record.embedded_public_key();
   BOOST_REQUIRE(public_key.has_value());
   BOOST_CHECK(p2p::make_peer_id(*public_key) == peer);
   auto key_bytes = std::vector<std::uint8_t>{'/', 'p', 'k', '/'};
   const auto peer_bytes = peer.to_bytes();
   key_bytes.insert(key_bytes.end(), peer_bytes.begin(), peer_bytes.end());
   const auto key = p2p::make_dht_key(key_bytes);
   const auto encoded_key = p2p::encode_public_key(*public_key);
   const auto put = forge::asio::blocking::run(app.runtime(), dht->put_value(amino, {.key_value = key,
                                                                                   .value = encoded_key}));
   BOOST_TEST(!put.quorum_reached);
   BOOST_TEST(put.accepted == 0U);
   BOOST_TEST(put.attempted == 0U);
   BOOST_CHECK(put.selected.value == encoded_key);
   const auto get = forge::asio::blocking::run(app.runtime(), dht->get_value(amino, key));
   BOOST_REQUIRE(get.selected.has_value());
   BOOST_CHECK(get.selected->value == encoded_key);
   BOOST_TEST(get.quorum_reached);
   BOOST_TEST(get.responses == 0U);
   BOOST_TEST(get.valid_records == 1U);

   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         app.runtime(), dht->put_value(amino, {.key_value = key, .value = {0}})),
                     p2p::exceptions::record_rejected);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), dht->get_value(providers, key)),
                     p2p::exceptions::unsupported_protocol);
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         app.runtime(), dht->find_peer({.value = "/forge/not-configured/1"}, peer)),
                     p2p::exceptions::unsupported_protocol);
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         app.runtime(), dht->find_providers(amino, key, {.timeout = std::chrono::milliseconds{0}})),
                     p2p::exceptions::invalid_options);
   // No peers means no remote quorum, not a successful local-only registration.
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), dht->provide(amino, key)),
                     p2p::exceptions::peer_not_found);
   BOOST_TEST(forge::asio::blocking::run(app.runtime(), dht->find_providers(amino, key)).empty());
}

BOOST_AUTO_TEST_CASE(p2p_dht_api_provider_registration_preserves_profile_and_lifecycle) {
   auto server = dht_application{"dht-plugin-server"};
   server.configure(configuration());
   forge::asio::blocking::run(server.runtime(), server.startup());
   auto server_cleanup = boost::scope::scope_exit{[&] {
      forge::asio::blocking::run(server.runtime(), server.shutdown());
   }};
   const auto server_node = node_handle(server);
   const auto address = server_node->local_endpoint();
   BOOST_REQUIRE(address.has_value());
   BOOST_CHECK(address->is_direct_tcp());

   auto client = dht_application{"dht-plugin-provider"};
   auto config = configuration();
   config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                   forge::config::core::value{address->to_string()}});
   config.set("plugins.net.p2p.node.bootstrap-requirement", "require-connection");
   client.configure(config);
   forge::asio::blocking::run(client.runtime(), client.startup());
   auto client_cleanup = boost::scope::scope_exit{[&] {
      forge::asio::blocking::run(client.runtime(), client.shutdown());
   }};
   const auto client_node = node_handle(client);
   const auto dht = dht_handle(client);
   const auto local_peer = client_node->local_peer();
   BOOST_CHECK(local_peer != server_node->local_peer());
   const auto diagnostics = client.apis().get<node_plugin::diagnostics_source>(
       {.id = {"forge.plugins.net.p2p.node.diagnostics_source"}, .major = 2, .min_revision = 0});
   BOOST_REQUIRE_MESSAGE(forge::asio::blocking::run(
                             client.runtime(), wait_for_dht_server(diagnostics.shared(), server_node->local_peer())),
                         "bootstrap did not authenticate and identify the configured DHT server");
   const auto found = forge::asio::blocking::run(client.runtime(), dht->find_peer(providers, server_node->local_peer()));
   BOOST_TEST(found.complete);
   BOOST_CHECK(std::ranges::any_of(found.closest_peers, [&](const auto& peer) {
      return peer.id == server_node->local_peer();
   }));

   const auto key = p2p::make_dht_key(forge::multiformats::multihash::sha2_256(
       std::vector<std::uint8_t>{'p', 'r', 'o', 'v', 'i', 'd', 'e'}).encode());
   auto registration = forge::asio::blocking::run(client.runtime(), dht->provide(providers, key));
   BOOST_TEST(registration.active());
   BOOST_CHECK(registration.profile() == providers);
   BOOST_CHECK(registration.key().bytes == key.bytes);
   const auto local = forge::asio::blocking::run(
       client.runtime(), dht->find_providers(providers, key, {.requested_count = 1}));
   BOOST_REQUIRE_EQUAL(local.size(), 1U);
   BOOST_CHECK(local.front().id == local_peer);
   BOOST_CHECK(!local.front().endpoints.empty());
   const auto server_dht = dht_handle(server);
   const auto remote = forge::asio::blocking::run(
       server.runtime(), server_dht->find_providers(providers, key, {.requested_count = 1}));
   BOOST_REQUIRE_EQUAL(remote.size(), 1U);
   BOOST_CHECK(remote.front().id == local_peer);
   BOOST_CHECK(!remote.front().endpoints.empty());
   BOOST_TEST(forge::asio::blocking::run(client.runtime(), dht->find_providers(amino, key)).empty());

   const auto ipns_value = std::vector<std::uint8_t>{'/', 'i', 'p', 'f', 's', '/', 'b', 'a', 'f', 'k', 'q', 'a', 'a', 'a'};
   const auto eol = forge::chrono::timestamp{std::chrono::time_point_cast<std::chrono::nanoseconds>(
       std::chrono::system_clock::now() + std::chrono::hours{1})};
   const auto signed_record = dht->create_ipns_record(ipns_value, 1, eol, std::chrono::minutes{1},
                                                     {.embed_public_key = true});
   const auto public_key = signed_record.embedded_public_key();
   BOOST_REQUIRE(public_key.has_value());
   BOOST_CHECK(p2p::make_peer_id(*public_key) == local_peer);
   auto pk_bytes = std::vector<std::uint8_t>{'/', 'p', 'k', '/'};
   const auto peer_bytes = local_peer.to_bytes();
   pk_bytes.insert(pk_bytes.end(), peer_bytes.begin(), peer_bytes.end());
   const auto pk = p2p::make_dht_key(pk_bytes);
   const auto encoded_key = p2p::encode_public_key(*public_key);
   const auto put = forge::asio::blocking::run(
       client.runtime(), dht->put_value(amino, {.key_value = pk, .value = encoded_key}));
   BOOST_TEST(put.quorum_reached);
   BOOST_TEST(put.accepted == 1U);
   BOOST_TEST(put.attempted == 1U);
   const auto stored = forge::asio::blocking::run(server.runtime(), server_dht->get_value(amino, pk));
   BOOST_REQUIRE(stored.selected.has_value());
   BOOST_CHECK(stored.selected->value == encoded_key);
   BOOST_TEST(stored.responses == 0U);

   auto reader = dht_application{"dht-plugin-independent-reader"};
   auto reader_config = configuration("client");
   reader_config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                          forge::config::core::value{address->to_string()}});
   reader_config.set("plugins.net.p2p.node.bootstrap-requirement", "require-connection");
   reader.configure(reader_config);
   forge::asio::blocking::run(reader.runtime(), reader.startup());
   auto reader_cleanup = boost::scope::scope_exit{[&] {
      forge::asio::blocking::run(reader.runtime(), reader.shutdown());
   }};
   const auto reader_diagnostics = reader.apis().get<node_plugin::diagnostics_source>(
       {.id = {"forge.plugins.net.p2p.node.diagnostics_source"}, .major = 2, .min_revision = 0});
   BOOST_REQUIRE(forge::asio::blocking::run(reader.runtime(),
       wait_for_dht_server(reader_diagnostics.shared(), server_node->local_peer(), amino)));
   const auto discovered_providers = forge::asio::blocking::run(
       reader.runtime(), dht_handle(reader)->find_providers(providers, key, {.requested_count = 1}));
   BOOST_REQUIRE_EQUAL(discovered_providers.size(), 1U);
   BOOST_CHECK(discovered_providers.front().id == local_peer);
   BOOST_CHECK(!discovered_providers.front().endpoints.empty());
   const auto fetched = forge::asio::blocking::run(reader.runtime(), dht_handle(reader)->get_value(amino, pk));
   BOOST_REQUIRE(fetched.selected.has_value());
   BOOST_CHECK(fetched.selected->value == encoded_key);
   BOOST_TEST(fetched.quorum_reached);
   BOOST_TEST(fetched.responses >= 1U);
   BOOST_TEST(fetched.valid_records >= 1U);
   forge::asio::blocking::run(reader.runtime(), reader.shutdown());
   reader_cleanup.set_active(false);

   auto coalesced = forge::asio::blocking::run(client.runtime(), dht->provide(providers, key));
   forge::asio::blocking::run(client.runtime(), registration.async_withdraw());
   BOOST_TEST(!registration.active());
   BOOST_TEST(coalesced.active());
   forge::asio::blocking::run(client.runtime(), coalesced.async_withdraw());
   BOOST_TEST(!coalesced.active());
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(client.runtime(), coalesced.async_withdraw()));
   // Remote provider advertisements have their own TTL; withdrawal is local ownership release.
   auto retained = forge::asio::blocking::run(client.runtime(), dht->provide(providers, key));
   forge::asio::blocking::run(client.runtime(), client.shutdown());
   client_cleanup.set_active(false);
   BOOST_TEST(!retained.active());
   // Both other hosts are stopped: this result must come from the server's stored advertisement.
   const auto stored_provider = forge::asio::blocking::run(
       server.runtime(), server_dht->find_providers(providers, key, {.requested_count = 1}));
   BOOST_REQUIRE_EQUAL(stored_provider.size(), 1U);
   BOOST_CHECK(stored_provider.front().id == local_peer);
}
