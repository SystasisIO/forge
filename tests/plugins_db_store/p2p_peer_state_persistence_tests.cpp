module;

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/db/object/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

module forge.plugins.net.p2p.node.plugin;

import forge.app.application_builder;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.config.core.document;
import forge.config.core.value;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.exceptions;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.transaction;
import forge.multiformats.multiaddr;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.rendezvous;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.db.store.api;
import forge.plugins.db.store.plugin;

#include "details/object_dht_record_store_adapter.hxx"
#include "details/object_peer_state_adapter.hxx"
#include "details/p2p_state_schema.hxx"

namespace {

namespace p2p = forge::net::p2p;
namespace p2p_node = forge::plugins::net::p2p::node;
namespace secrets_plugin = forge::plugins::crypto::secrets;
namespace store_plugin = forge::plugins::db::store;
using p2p_state_schema = forge::plugins::net::p2p::node::detail::p2p_state_schema;

constexpr auto peer_store_name = std::string_view{"p2p-peer-state"};

struct root_guard {
   std::filesystem::path root =
       std::filesystem::temp_directory_path() /
       ("forge_p2p_peer_state_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

   root_guard() {
      std::filesystem::remove_all(root);
   }

   ~root_guard() {
      std::filesystem::remove_all(root);
   }
};

[[nodiscard]] forge::config::core::value configured_store(std::string driver, const std::filesystem::path& path) {
   auto object_layer = forge::config::core::value::object_type{};
   object_layer.emplace("family", forge::config::core::value{std::string{"objectdb"}});
   object_layer.emplace("write-policy", forge::config::core::value{std::string{"single-writer"}});

   auto store = forge::config::core::value::object_type{};
   store.emplace("name", forge::config::core::value{std::string{peer_store_name}});
   store.emplace("driver", forge::config::core::value{std::move(driver)});
   store.emplace("path", forge::config::core::value{path.string()});
   store.emplace("object", forge::config::core::value{std::move(object_layer)});
   return forge::config::core::value{std::move(store)};
}

[[nodiscard]] forge::config::core::document document_for(std::string driver, const std::filesystem::path& path) {
   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores", forge::config::core::value::array_type{
                                               configured_store(std::move(driver), path),
                                           });
   return document;
}

[[nodiscard]] forge::config::core::value secret(std::string id, std::string_view value, std::string purpose) {
   auto source = forge::config::core::value::object_type{};
   source.emplace("type", forge::config::core::value{"value"});
   source.emplace("encoding", forge::config::core::value{"raw"});
   source.emplace("value", forge::config::core::value{std::string{value}});

   auto configured = forge::config::core::value::object_type{};
   configured.emplace("id", forge::config::core::value{std::move(id)});
   configured.emplace("kind", forge::config::core::value{"bytes"});
   configured.emplace("source", forge::config::core::value{std::move(source)});
   configured.emplace("purposes", forge::config::core::value::array_type{
                                      forge::config::core::value{std::move(purpose)},
                                  });
   configured.emplace("operations", forge::config::core::value::array_type{
                                        forge::config::core::value{"get_bytes"},
                                    });
   configured.emplace("allow-raw-export", forge::config::core::value{true});
   return forge::config::core::value{std::move(configured)};
}

[[nodiscard]] forge::config::core::document p2p_document_for(std::string driver, const std::filesystem::path& path,
                                                             const forge::tests::p2p::identity_fixture& identity) {
   constexpr auto certificate_id = std::string_view{"p2p/test-certificate"};
   constexpr auto private_key_id = std::string_view{"p2p/test-private-key"};
   auto document = document_for(std::move(driver), path);
   document.set("plugins.crypto.secrets.secrets",
                forge::config::core::value::array_type{
                    secret(std::string{certificate_id}, identity.certificate_pem, "p2p.identity.certificate"),
                    secret(std::string{private_key_id}, identity.private_key_pem, "p2p.identity.private-key"),
                });
   document.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                               forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"},
                                           });
   document.set("plugins.net.p2p.node.peer-store.store", std::string{peer_store_name});
   document.set("plugins.net.p2p.node.identity.certificate-secret", std::string{certificate_id});
   document.set("plugins.net.p2p.node.identity.private-key-secret", std::string{private_key_id});
   return document;
}

[[nodiscard]] std::unique_ptr<forge::app::application_shell> make_app(forge::config::core::document document) {
   auto builder = forge::app::application_builder{};
   builder.name("p2p-peer-state-persistence-test")
       .runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "p2p-peer-state-test"})
       .plugin(store_plugin::descriptor());
   auto app = std::move(builder).build();
   app->configure(document);
   forge::asio::blocking::run(app->runtime(), app->startup());
   return app;
}

[[nodiscard]] std::unique_ptr<forge::app::application_shell> build_p2p_app(forge::config::core::document document) {
   auto builder = forge::app::application_builder{};
   builder.name("p2p-peer-state-maintenance-test")
       .runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "p2p-maintenance-test"})
       .plugin(store_plugin::descriptor())
       .plugin(secrets_plugin::descriptor())
       .plugin(p2p_node::descriptor());
   auto app = std::move(builder).build();
   app->configure(document);
   return app;
}

[[nodiscard]] std::unique_ptr<forge::app::application_shell> make_p2p_app(forge::config::core::document document) {
   auto app = build_p2p_app(std::move(document));
   forge::asio::blocking::run(app->runtime(), app->startup());
   return app;
}

class tracking_store_api final : public store_plugin::api {
 public:
   explicit tracking_store_api(store_plugin::api* delegate) : delegate_{delegate} {}

   boost::asio::awaitable<void> add_store(std::string name, std::shared_ptr<forge::db::core::driver> driver,
                                          store_plugin::store_options options) override {
      co_await delegate_->add_store(std::move(name), std::move(driver), std::move(options));
   }

   boost::asio::awaitable<store_plugin::store_handle> store(std::string name) override {
      co_return co_await delegate_->store(std::move(name));
   }

   boost::asio::awaitable<void> flush(std::string name, bool sync) override {
      ++flush_calls;
      last_store = std::move(name);
      last_sync = sync;
      if (fail_flush) {
         throw std::runtime_error{"injected DB Store flush failure"};
      }
      co_await delegate_->flush(last_store, sync);
   }

   boost::asio::awaitable<void> flush_all(bool sync) override {
      co_await delegate_->flush_all(sync);
   }

   boost::asio::awaitable<store_plugin::status> status() override {
      co_return co_await delegate_->status();
   }

   std::size_t flush_calls = 0;
   std::string last_store;
   bool last_sync = false;
   bool fail_flush = false;

 private:
   store_plugin::api* delegate_;
};

[[nodiscard]] std::shared_ptr<p2p::peer_store::persistence>
open_peer_persistence(forge::app::application_shell& app, bool register_schema = true,
                      bool reset_incompatible_cache = false) {
   auto stores = app.apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app.runtime(), stores->store(std::string{peer_store_name}));
   if (register_schema) {
      p2p_state_schema::register_objects(handle);
   }
   return forge::asio::blocking::run(
       app.runtime(), p2p_node::object_peer_state_adapter::async_open(stores.operator->(), std::move(handle), {},
                                                                      reset_incompatible_cache));
}

[[nodiscard]] std::shared_ptr<p2p::dht::record_store::persistence>
open_dht_persistence(forge::app::application_shell& app, p2p::protocol_id profile,
                     p2p::dht::record_store::options limits = {}) {
   auto stores = app.apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app.runtime(), stores->store(std::string{peer_store_name}));
   return forge::asio::blocking::run(
       app.runtime(), p2p_node::object_dht_record_store_adapter::async_open(stores.operator->(), std::move(handle),
                                                                            std::move(profile), std::move(limits)));
}

[[nodiscard]] p2p::peer_store open_peer_state(forge::app::application_shell& app) {
   return p2p::peer_store{p2p::peer_store::options{
       .persistence = open_peer_persistence(app),
       .max_peers = 1,
       .max_pending = 4,
   }};
}

template <typename DocumentFactory> void check_object_peer_state_persistence(DocumentFactory&& make_document) {
   const auto low = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-peer-state-low").certificate_pem);
   const auto high = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-peer-state-high").certificate_pem);
   const auto registration_peer = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-peer-state-registration").certificate_pem);
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   auto highest_sequence = std::uint64_t{};

   {
      auto app = make_app(make_document());
      auto state = open_peer_state(*app);
      state.upsert(p2p::peer_store::record{
          .peer = low,
          .protocol_version = "/forge/persisted/low/1",
          .discovery_expires_at = expires_at,
          .failures = 10,
      });
      state.upsert(p2p::peer_store::record{
          .peer = high,
          .protocol_version = "/forge/persisted/high/1",
          .endpoints = std::vector<p2p::peer_store::endpoint_record>{p2p::peer_store::endpoint_record{
              .address = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4701/p2p/" + high.to_string()).to_multiaddr(),
              .sources =
                  p2p::peer_store::endpoint_sources{
                      .learned = false,
                      .identify_signed = true,
                  },
          }},
          .discovery_expires_at = expires_at,
          .successes = 10,
      });
      forge::asio::blocking::run(app->runtime(), state.async_flush());
      forge::asio::blocking::run(app->runtime(), state.async_upsert_rendezvous(p2p::rendezvous::registration{
                                                     .namespace_name = "forge.peer-state",
                                                     .peer = low,
                                                     .ttl = std::chrono::hours{1},
                                                     .expires_at = expires_at,
                                                 }));
      forge::asio::blocking::run(app->runtime(), state.async_upsert_rendezvous(p2p::rendezvous::registration{
                                                     .namespace_name = "forge.peer-state",
                                                     .peer = high,
                                                     .ttl = std::chrono::hours{1},
                                                     .expires_at = expires_at,
                                                 }));
      const auto registrations = state.discover_rendezvous("forge.peer-state", 0, 10);
      BOOST_REQUIRE_EQUAL(registrations.size(), 2U);
      highest_sequence = registrations.back().sequence;
      forge::asio::blocking::run(app->runtime(), state.async_remove_rendezvous(low, "forge.peer-state"));
      forge::asio::blocking::run(app->runtime(), state.async_remove_rendezvous(high, "forge.peer-state"));
      forge::asio::blocking::run(app->runtime(), state.async_close());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto app = make_app(make_document());
      auto state = open_peer_state(*app);
      forge::asio::blocking::run(app->runtime(), state.async_hydrate());
      BOOST_REQUIRE_EQUAL(state.snapshot(10).size(), 1U);
      BOOST_TEST(!state.find(low).has_value());
      const auto hydrated_high = state.find(high);
      BOOST_REQUIRE(hydrated_high.has_value());
      BOOST_REQUIRE_EQUAL(hydrated_high->endpoints.size(), 1U);
      // The private cache persists endpoint facts conservatively as learned
      // addresses; signed Identify provenance is rebuilt by live Identify.
      BOOST_TEST(hydrated_high->endpoints.front().sources.learned);
      BOOST_TEST(!hydrated_high->endpoints.front().sources.identify_unsigned);
      BOOST_TEST(!hydrated_high->endpoints.front().sources.identify_signed);
      BOOST_TEST(state.discover_rendezvous("forge.peer-state", 0, 10).empty());

      forge::asio::blocking::run(app->runtime(), state.async_upsert_rendezvous(p2p::rendezvous::registration{
                                                     .namespace_name = "forge.peer-state",
                                                     .peer = registration_peer,
                                                     .ttl = std::chrono::hours{1},
                                                     .expires_at = expires_at,
                                                 }));
      const auto added = state.discover_rendezvous("forge.peer-state", highest_sequence, 10);
      BOOST_REQUIRE_EQUAL(added.size(), 1U);
      BOOST_TEST(added.front().sequence > highest_sequence);
      forge::asio::blocking::run(app->runtime(), state.async_close());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}

void check_plugin_maintenance_prunes_expired_peer(std::string driver, const std::filesystem::path& path) {
   const auto identity = forge::tests::p2p::make_identity_fixture("p2p-maintenance-node");
   const auto expired = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-maintenance-expired").certificate_pem);
   auto app = make_p2p_app(p2p_document_for(std::move(driver), path, identity));
   auto persistence = open_peer_persistence(*app, false);
   auto state = p2p::peer_store{p2p::peer_store::options{.persistence = persistence}};
   state.upsert(p2p::peer_store::record{
       .peer = expired,
       .protocol_version = "/forge/expired/1",
       .discovery_expires_at = std::chrono::system_clock::now() - std::chrono::seconds{1},
   });
   forge::asio::blocking::run(app->runtime(), state.async_flush());

   const auto contains_expired = [&](const p2p::peer_store::hydration_page& page) {
      return std::ranges::any_of(page.peers, [&](const auto& value) { return value.peer == expired; });
   };
   auto page = forge::asio::blocking::run(app->runtime(), persistence->async_hydrate(p2p::peer_store::hydration_request{
                                                              .kind = p2p::peer_store::hydration_kind::peers,
                                                              .limit = 256,
                                                          }));
   BOOST_REQUIRE(contains_expired(page));

   for (auto attempt = 0; attempt < 80 && contains_expired(page); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
      page = forge::asio::blocking::run(app->runtime(), persistence->async_hydrate(p2p::peer_store::hydration_request{
                                                            .kind = p2p::peer_store::hydration_kind::peers,
                                                            .limit = 256,
                                                        }));
   }
   BOOST_TEST(!contains_expired(page));
   forge::asio::blocking::run(app->runtime(), state.async_close());
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

void check_object_peer_state_prune_rotates_categories(std::string driver, const std::filesystem::path& path) {
   auto app = make_app(document_for(std::move(driver), path));
   auto persistence = open_peer_persistence(*app);
   auto state = p2p::peer_store{p2p::peer_store::options{
       .persistence = persistence,
       .prune_page_limit = 1,
   }};
   const auto expired_at = std::chrono::system_clock::now() - std::chrono::seconds{1};
   const auto peer = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-prune-peer").certificate_pem);
   const auto registration = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-prune-rendezvous").certificate_pem);

   state.upsert(p2p::peer_store::record{.peer = peer, .discovery_expires_at = expired_at});
   forge::asio::blocking::run(app->runtime(), state.async_flush());
   forge::asio::blocking::run(app->runtime(), state.async_upsert_rendezvous(p2p::rendezvous::registration{
                                                  .namespace_name = "forge.prune.rotation",
                                                  .peer = registration,
                                                  .ttl = std::chrono::seconds{1},
                                                  .expires_at = expired_at,
                                              }));

   const auto first = forge::asio::blocking::run(app->runtime(), state.async_prune_expired());
   const auto second = forge::asio::blocking::run(app->runtime(), state.async_prune_expired());

   BOOST_TEST(first.peers.size() == 1U);
   BOOST_TEST(second.rendezvous_registrations.size() == 1U);
   BOOST_TEST(!second.may_have_more);
   BOOST_TEST(state.snapshot(1).empty());
   BOOST_TEST(state.discover_rendezvous("forge.prune.rotation", 0, 1).empty());

   forge::asio::blocking::run(app->runtime(), state.async_close());
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

void check_object_peer_state_prune_reports_exact_unhydrated_identity(std::string driver,
                                                                     const std::filesystem::path& path) {
   auto app = make_app(document_for(std::move(driver), path));
   auto persistence = open_peer_persistence(*app);
   auto first = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-prune-exact-first").certificate_pem);
   auto second = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-prune-exact-second").certificate_pem);
   if (second.value < first.value) {
      std::swap(first, second);
   }
   const auto now = std::chrono::system_clock::now();
   auto batch = p2p::peer_store::mutation_batch{};
   batch.peer_upserts = {
       p2p::peer_store::record{.peer = first, .discovery_expires_at = now - std::chrono::seconds{1}},
       p2p::peer_store::record{.peer = second, .discovery_expires_at = now - std::chrono::seconds{2}},
   };
   (void)forge::asio::blocking::run(app->runtime(), persistence->async_apply(std::move(batch)));

   auto state = p2p::peer_store{p2p::peer_store::options{
       .persistence = persistence,
       .max_peers = 1,
       .prune_page_limit = 1,
   }};
   forge::asio::blocking::run(app->runtime(), state.async_hydrate());
   BOOST_REQUIRE(state.find(first).has_value());
   BOOST_TEST(!state.find(second).has_value());

   const auto result = forge::asio::blocking::run(app->runtime(), state.async_prune_expired(now));
   BOOST_REQUIRE_EQUAL(result.peers.size(), 1U);
   BOOST_TEST(result.peers.front().value == second.value);
   BOOST_TEST(state.find(first).has_value());
   BOOST_TEST(!state.find(second).has_value());

   forge::asio::blocking::run(app->runtime(), state.async_close());
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

void check_object_peer_state_durable_acknowledgement(std::string driver, const std::filesystem::path& path) {
   auto app = make_app(document_for(std::move(driver), path));
   auto stores = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app->runtime(), stores->store(std::string{peer_store_name}));
   p2p_state_schema::register_objects(handle);
   auto tracking = tracking_store_api{stores.operator->()};
   auto persistence = forge::asio::blocking::run(
       app->runtime(), p2p_node::object_peer_state_adapter::async_open(&tracking, std::move(handle), {}));
   const auto schema_flushes = tracking.flush_calls;
   BOOST_TEST(schema_flushes == 1U);
   auto state = p2p::peer_store{p2p::peer_store::options{.persistence = persistence}};
   const auto first_registration = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-durable-first").certificate_pem);
   const auto registration = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-durable-rendezvous").certificate_pem);
   const auto expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};

   forge::asio::blocking::run(app->runtime(), state.async_upsert_rendezvous(p2p::rendezvous::registration{
                                                  .namespace_name = "forge.durable.first",
                                                  .peer = first_registration,
                                                  .ttl = std::chrono::hours{1},
                                                  .expires_at = expires_at,
                                              }));
   BOOST_TEST(tracking.flush_calls == schema_flushes + 1U);
   BOOST_TEST(tracking.last_store == peer_store_name);
   BOOST_TEST(tracking.last_sync);

   tracking.fail_flush = true;
   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(app->runtime(), state.async_upsert_rendezvous(p2p::rendezvous::registration{
                                                       .namespace_name = "forge.durable",
                                                       .peer = registration,
                                                       .ttl = std::chrono::hours{1},
                                                       .expires_at = expires_at,
                                                   }))),
       p2p::exceptions::durability_uncertain);
   BOOST_TEST(tracking.flush_calls == schema_flushes + 2U);
   BOOST_TEST(tracking.last_sync);
   BOOST_TEST(state.discover_rendezvous("forge.durable", 0, 1).size() == 1U);
   BOOST_TEST(state.persistence_state().degraded);

   tracking.fail_flush = false;
   forge::asio::blocking::run(app->runtime(), state.async_flush());
   BOOST_TEST(!state.persistence_state().degraded);

   (void)forge::asio::blocking::run(app->runtime(), state.async_prune_expired());
   BOOST_TEST(tracking.flush_calls == schema_flushes + 3U);
   forge::asio::blocking::run(app->runtime(), state.async_close());
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

void check_p2p_state_schema_flush_retries(std::string driver, const std::filesystem::path& path) {
   auto app = make_app(document_for(std::move(driver), path));
   auto stores = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app->runtime(), stores->store(std::string{peer_store_name}));
   p2p_state_schema::register_objects(handle);
   auto tracking = tracking_store_api{stores.operator->()};
   tracking.fail_flush = true;

   BOOST_CHECK_THROW((forge::asio::blocking::run(
                         app->runtime(), p2p_node::object_peer_state_adapter::async_open(&tracking, handle, {}))),
                     std::runtime_error);
   BOOST_TEST(tracking.flush_calls == 1U);
   BOOST_TEST(tracking.last_store == peer_store_name);
   BOOST_TEST(tracking.last_sync);

   tracking.fail_flush = false;
   auto persistence = forge::asio::blocking::run(
       app->runtime(), p2p_node::object_peer_state_adapter::async_open(&tracking, handle, {}));
   BOOST_TEST(tracking.flush_calls == 2U);
   BOOST_TEST(tracking.last_sync);

   forge::asio::blocking::run(app->runtime(), persistence->async_close());
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

void check_object_peer_state_rejects_oversized_hydration(std::string driver, const std::filesystem::path& path) {
   auto app = make_app(document_for(std::move(driver), path));
   auto stores = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app->runtime(), stores->store(std::string{peer_store_name}));
   p2p_state_schema::register_objects(handle);
   auto limits = p2p::peer_store::options{.max_protocols_per_peer = 1};
   auto persistence = forge::asio::blocking::run(
       app->runtime(), p2p_node::object_peer_state_adapter::async_open(stores.operator->(), handle, limits));
   const auto invalid_peer = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-oversized-hydration").certificate_pem);

   auto insert_invalid = [&]() -> boost::asio::awaitable<void> {
      auto transaction = co_await handle.begin_transaction();
      auto objects = co_await handle.objects().join(transaction);
      co_await objects.create<p2p_state_schema::peer_row>([&](p2p_state_schema::peer_row& row) {
         row.peer = invalid_peer.value;
         row.protocols = {"/forge/test/1", "/forge/test/2"};
         row.hydration_priority = std::uint64_t{1} << 63U;
      });
      co_await transaction.commit();
   };
   forge::asio::blocking::run(app->runtime(), insert_invalid());

   BOOST_CHECK_THROW(
       (forge::asio::blocking::run(app->runtime(), persistence->async_hydrate(p2p::peer_store::hydration_request{
                                                       .kind = p2p::peer_store::hydration_kind::peers,
                                                       .limit = 256,
                                                   }))),
       p2p::exceptions::codec_error);

   forge::asio::blocking::run(app->runtime(), persistence->async_close());
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

void check_object_peer_state_clears_terminal_hydration_cursor(std::string driver, const std::filesystem::path& path) {
   auto app = make_app(document_for(std::move(driver), path));
   auto persistence = open_peer_persistence(*app);
   auto state = p2p::peer_store{p2p::peer_store::options{.persistence = persistence}};
   const auto first = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-terminal-cursor-first").certificate_pem);
   const auto second = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-terminal-cursor-second").certificate_pem);
   state.upsert(p2p::peer_store::record{.peer = first, .protocol_version = "/forge/cursor/first/1"});
   state.upsert(p2p::peer_store::record{.peer = second, .protocol_version = "/forge/cursor/second/1"});
   forge::asio::blocking::run(app->runtime(), state.async_flush());

   const auto page =
       forge::asio::blocking::run(app->runtime(), persistence->async_hydrate(p2p::peer_store::hydration_request{
                                                      .kind = p2p::peer_store::hydration_kind::peers,
                                                      .limit = 256,
                                                  }));
   BOOST_REQUIRE_EQUAL(page.peers.size(), 2U);
   BOOST_TEST(!page.cursor.has_value());

   forge::asio::blocking::run(app->runtime(), state.async_close());
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

void check_object_dht_record_persistence(std::string driver, const std::filesystem::path& path) {
   const auto profile_a = p2p::protocol_id{.value = "/forge/test/dht/a/1"};
   const auto profile_b = p2p::protocol_id{.value = "/forge/test/dht/b/1"};
   const auto publisher = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-dht-publisher").certificate_pem);
   const auto address_provider = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-dht-address-provider").certificate_pem);
   const auto expired_provider = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-dht-expired-provider").certificate_pem);
   const auto fully_expired_provider = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-dht-fully-expired-provider").certificate_pem);
   const auto local_provider = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-dht-local-provider").certificate_pem);
   const auto active_key = p2p::make_dht_key(std::vector<std::uint8_t>{'a', 'c', 't', 'i', 'v', 'e'});
   const auto expired_key = p2p::make_dht_key(std::vector<std::uint8_t>{'e', 'x', 'p', 'i', 'r', 'e', 'd'});
   const auto near_limit_key =
       p2p::make_dht_key(std::vector<std::uint8_t>{'n', 'e', 'a', 'r', '-', 'l', 'i', 'm', 'i', 't'});
   const auto provider_key = p2p::make_dht_key(std::vector<std::uint8_t>{'p', 'r', 'o', 'v', 'i', 'd', 'e', 'r'});
   const auto now = std::chrono::system_clock::now();
   const auto future = now + std::chrono::hours{1};
   const auto past = now - std::chrono::seconds{1};
   const auto older_past = now - std::chrono::seconds{2};
   const auto address = forge::multiformats::multiaddr::parse(
       "/dnsaddr/object-provider.example/p2p/" + address_provider.to_string());
   const auto fully_expired_address =
       p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4782/p2p/" + fully_expired_provider.to_string()).to_multiaddr();

   {
      auto app = make_app(document_for(driver, path));
      auto peer_persistence = open_peer_persistence(*app);
      forge::asio::blocking::run(app->runtime(), peer_persistence->async_close());
      auto persistence_a = open_dht_persistence(*app, profile_a);
      auto persistence_b = open_dht_persistence(*app, profile_b);

      auto batch_a = p2p::dht::record_store::mutation_batch{};
      batch_a.value_upserts = {
          p2p::dht::record_store::value_record{
              .record =
                  p2p::dht::record{
                      .key_value = active_key,
                      .value = std::vector<std::uint8_t>{'a'},
                      .time_received = "2026-08-14T00:00:00Z",
                      .publisher = publisher,
                      .ttl = std::chrono::seconds{60},
                  },
              .expires_at = future,
          },
          p2p::dht::record_store::value_record{
              .record =
                  p2p::dht::record{
                      .key_value = expired_key,
                      .value = std::vector<std::uint8_t>{'x'},
                      .time_received = "2026-08-14T00:00:01Z",
                      .publisher = publisher,
                      .ttl = std::chrono::seconds{60},
                  },
              .expires_at = past,
          },
      };
      batch_a.provider_upserts = {
          p2p::dht::record_store::provider_record{
              .key = provider_key,
              .provider = address_provider,
              .endpoints = {address},
              .provider_expires_at = future,
              .addresses_expires_at = past,
          },
          p2p::dht::record_store::provider_record{
              .key = provider_key,
              .provider = expired_provider,
              .provider_expires_at = past,
          },
          p2p::dht::record_store::provider_record{
              .key = provider_key,
              .provider = fully_expired_provider,
              .endpoints = {fully_expired_address},
              .provider_expires_at = past,
              .addresses_expires_at = older_past,
          },
          p2p::dht::record_store::provider_record{
              .key = provider_key,
              .provider = local_provider,
              .provider_expires_at = future,
              .local_owned = true,
          },
      };
      const auto committed_a =
          forge::asio::blocking::run(app->runtime(), persistence_a->async_apply(std::move(batch_a)));
      BOOST_TEST(committed_a.durability_confirmed);

      auto batch_b = p2p::dht::record_store::mutation_batch{};
      batch_b.value_upserts.push_back(p2p::dht::record_store::value_record{
          .record =
              p2p::dht::record{
                  .key_value = expired_key,
                  .value = std::vector<std::uint8_t>{'b'},
                  .time_received = "2026-08-14T00:00:02Z",
                  .ttl = std::chrono::seconds{60},
              },
          .expires_at = past,
      });
      batch_b.provider_upserts.push_back(p2p::dht::record_store::provider_record{
          .key = provider_key,
          .provider = expired_provider,
          .provider_expires_at = past,
      });
      const auto committed_b =
          forge::asio::blocking::run(app->runtime(), persistence_b->async_apply(std::move(batch_b)));
      BOOST_TEST(committed_b.durability_confirmed);

      forge::asio::blocking::run(app->runtime(), persistence_a->async_close());
      forge::asio::blocking::run(app->runtime(), persistence_b->async_close());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto app = make_app(document_for(std::move(driver), path));
      auto peer_persistence = open_peer_persistence(*app);
      forge::asio::blocking::run(app->runtime(), peer_persistence->async_close());
      auto persistence_a = open_dht_persistence(*app, profile_a);
      auto persistence_b = open_dht_persistence(*app, profile_b);

      const auto first_values = forge::asio::blocking::run(
          app->runtime(), persistence_a->async_hydrate(p2p::dht::record_store::hydration_request{
                              .kind = p2p::dht::record_store::hydration_kind::values,
                              .limit = 1,
                          }));
      BOOST_REQUIRE_EQUAL(first_values.values.size(), 1U);
      BOOST_REQUIRE(first_values.cursor.has_value());
      const auto second_values = forge::asio::blocking::run(
          app->runtime(), persistence_a->async_hydrate(p2p::dht::record_store::hydration_request{
                              .kind = p2p::dht::record_store::hydration_kind::values,
                              .cursor = first_values.cursor,
                              .limit = 1,
                          }));
      BOOST_REQUIRE_EQUAL(second_values.values.size(), 1U);
      BOOST_TEST(!second_values.cursor.has_value());

      const auto providers = forge::asio::blocking::run(
          app->runtime(), persistence_a->async_hydrate(p2p::dht::record_store::hydration_request{
                              .kind = p2p::dht::record_store::hydration_kind::providers,
                              .limit = 10,
                          }));
      BOOST_REQUIRE_EQUAL(providers.providers.size(), 4U);
      BOOST_TEST(std::ranges::any_of(providers.providers, [&](const auto& value) {
         return value.provider == local_provider && value.local_owned;
      }));
      BOOST_TEST(std::ranges::any_of(providers.providers, [&](const auto& value) {
         return value.provider == address_provider && value.endpoints.size() == 1U &&
                value.endpoints.front().to_string() == address.to_string();
      }));

      const auto pruned = forge::asio::blocking::run(app->runtime(), persistence_a->async_prune_expired(now, 10));
      BOOST_REQUIRE_EQUAL(pruned.values.size(), 1U);
      BOOST_REQUIRE_EQUAL(pruned.providers.size(), 2U);
      BOOST_REQUIRE_EQUAL(pruned.provider_address_updates.size(), 1U);
      BOOST_TEST(!pruned.may_have_more);
      BOOST_TEST(pruned.durability.durability_confirmed);
      BOOST_TEST(pruned.provider_address_updates.front().provider.value == address_provider.value);
      BOOST_TEST(std::ranges::any_of(pruned.providers,
                                     [&](const auto& value) { return value.provider == fully_expired_provider; }));
      BOOST_TEST(std::ranges::none_of(pruned.provider_address_updates,
                                      [&](const auto& value) { return value.provider == fully_expired_provider; }));
      BOOST_TEST(pruned.provider_address_updates.front().endpoints.empty());
      BOOST_TEST(pruned.provider_address_updates.front().addresses_expires_at.time_since_epoch().count() == 0);

      const auto remaining_a_values = forge::asio::blocking::run(
          app->runtime(), persistence_a->async_hydrate(p2p::dht::record_store::hydration_request{
                              .kind = p2p::dht::record_store::hydration_kind::values,
                              .limit = 10,
                          }));
      BOOST_REQUIRE_EQUAL(remaining_a_values.values.size(), 1U);
      BOOST_TEST(remaining_a_values.values.front().record.key_value.bytes == active_key.bytes);
      const auto remaining_a_providers = forge::asio::blocking::run(
          app->runtime(), persistence_a->async_hydrate(p2p::dht::record_store::hydration_request{
                              .kind = p2p::dht::record_store::hydration_kind::providers,
                              .limit = 10,
                          }));
      BOOST_REQUIRE_EQUAL(remaining_a_providers.providers.size(), 2U);
      BOOST_TEST(std::ranges::any_of(remaining_a_providers.providers, [&](const auto& value) {
         return value.provider == local_provider && value.local_owned;
      }));

      constexpr auto near_limit_ns = (std::numeric_limits<std::int64_t>::max)() - 807;
      const auto near_limit = std::chrono::system_clock::time_point{
          std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{near_limit_ns})};
      auto near_limit_batch = p2p::dht::record_store::mutation_batch{};
      near_limit_batch.value_upserts.push_back(p2p::dht::record_store::value_record{
          .record =
              p2p::dht::record{
                  .key_value = near_limit_key,
                  .value = std::vector<std::uint8_t>{'z'},
                  .time_received = "2262-04-11T23:47:16Z",
                  .ttl = std::chrono::seconds{60},
              },
          .expires_at = near_limit,
      });
      const auto near_limit_commit =
          forge::asio::blocking::run(app->runtime(), persistence_a->async_apply(std::move(near_limit_batch)));
      BOOST_TEST(near_limit_commit.durability_confirmed);
      const auto near_limit_prune =
          forge::asio::blocking::run(app->runtime(), persistence_a->async_prune_expired(near_limit, 10));
      BOOST_TEST(std::ranges::any_of(near_limit_prune.values,
                                     [&](const auto& value) { return value.bytes == near_limit_key.bytes; }));

      const auto isolated_b_values = forge::asio::blocking::run(
          app->runtime(), persistence_b->async_hydrate(p2p::dht::record_store::hydration_request{
                              .kind = p2p::dht::record_store::hydration_kind::values,
                              .limit = 10,
                          }));
      const auto isolated_b_providers = forge::asio::blocking::run(
          app->runtime(), persistence_b->async_hydrate(p2p::dht::record_store::hydration_request{
                              .kind = p2p::dht::record_store::hydration_kind::providers,
                              .limit = 10,
                          }));
      BOOST_REQUIRE_EQUAL(isolated_b_values.values.size(), 1U);
      BOOST_REQUIRE_EQUAL(isolated_b_providers.providers.size(), 1U);
      BOOST_TEST(isolated_b_values.values.front().record.value == std::vector<std::uint8_t>{'b'});

      forge::asio::blocking::run(app->runtime(), persistence_a->async_flush());
      forge::asio::blocking::run(app->runtime(), persistence_a->async_close());
      forge::asio::blocking::run(app->runtime(), persistence_b->async_close());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}

void check_object_p2p_state_incompatible_and_missing_marker_policy(std::string driver, const std::filesystem::path& path) {
   const auto peer = p2p::make_peer_id_from_certificate_pem(
       forge::tests::p2p::make_identity_fixture("p2p-v1-reset-peer").certificate_pem);
   const auto profile = p2p::protocol_id{.value = "/forge/test/dht/reset/1"};

   {
      auto app = make_app(document_for(driver, path / "v1"));
      auto stores = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), stores->store(std::string{peer_store_name}));
      p2p_state_schema::register_objects(handle);
      auto seed_v1 = [&]() -> boost::asio::awaitable<void> {
         auto transaction = co_await handle.begin_transaction();
         auto objects = co_await handle.objects().join(transaction);
         auto state = p2p_state_schema::schema_state{};
         state.id = p2p_state_schema::schema_state_id;
         state.format_version = 1;
         co_await objects.insert(state);
         co_await objects.create<p2p_state_schema::peer_row>([&](p2p_state_schema::peer_row& row) {
            row.peer = peer.value;
            row.hydration_priority = std::uint64_t{1} << 63U;
         });
         co_await objects.create<p2p_state_schema::legacy_provider_v1_row>(
             [&](p2p_state_schema::legacy_provider_v1_row& row) {
                row.key = "legacy";
                row.peer = peer.value;
                row.expires_at_ns = 1;
             });
         co_await objects.create<p2p_state_schema::rendezvous_row>([&](p2p_state_schema::rendezvous_row& row) {
            row.namespace_name = "forge.reset";
            row.peer = peer.value;
            row.sequence = 1;
         });
         co_await objects.create<p2p_state_schema::dht_value_row>([&](p2p_state_schema::dht_value_row& row) {
            row.profile = profile.value;
            row.key = "value";
            row.expires_at_ns = 1;
         });
         co_await objects.create<p2p_state_schema::dht_provider_row>([&](p2p_state_schema::dht_provider_row& row) {
            row.profile = profile.value;
            row.key = "provider";
            row.peer = peer.value;
            row.provider_expires_at_ns = 1;
         });
         co_await transaction.commit();
      };
      forge::asio::blocking::run(app->runtime(), seed_v1());

      BOOST_CHECK_THROW((forge::asio::blocking::run(app->runtime(), p2p_node::object_peer_state_adapter::async_open(
                                                                        stores.operator->(), handle, {}, false))),
                        forge::db::object::exceptions::incompatible_version);
      auto reset = forge::asio::blocking::run(
          app->runtime(), p2p_node::object_peer_state_adapter::async_open(stores.operator->(), handle, {}, true));
      const auto peer_page =
          forge::asio::blocking::run(app->runtime(), reset->async_hydrate(p2p::peer_store::hydration_request{
                                                         .kind = p2p::peer_store::hydration_kind::peers,
                                                         .limit = 10,
                                                     }));
      const auto rendezvous_page =
          forge::asio::blocking::run(app->runtime(), reset->async_hydrate(p2p::peer_store::hydration_request{
                                                         .kind = p2p::peer_store::hydration_kind::rendezvous,
                                                         .limit = 10,
                                                     }));
      BOOST_TEST(peer_page.peers.empty());
      BOOST_TEST(rendezvous_page.rendezvous_registrations.empty());

      auto dht = open_dht_persistence(*app, profile);
      const auto values =
          forge::asio::blocking::run(app->runtime(), dht->async_hydrate(p2p::dht::record_store::hydration_request{
                                                         .kind = p2p::dht::record_store::hydration_kind::values,
                                                         .limit = 10,
                                                     }));
      const auto providers =
          forge::asio::blocking::run(app->runtime(), dht->async_hydrate(p2p::dht::record_store::hydration_request{
                                                         .kind = p2p::dht::record_store::hydration_kind::providers,
                                                         .limit = 10,
                                                     }));
      BOOST_TEST(values.values.empty());
      BOOST_TEST(providers.providers.empty());

      auto verify_reset = [&]() -> boost::asio::awaitable<void> {
         auto snapshot = co_await handle.begin_read();
         auto objects = snapshot.objects();
         const auto state = co_await objects.get(p2p_state_schema::schema_state_id);
         BOOST_TEST(state.format_version == p2p_state_schema::format_version);
         auto legacy =
             co_await objects
                 .index<p2p_state_schema::legacy_provider_v1_object, p2p_state_schema::by_legacy_provider_v1_row_id>()
                 .lower_bound(p2p_state_schema::legacy_provider_v1_row::id_t{})
                 .page(forge::db::core::page_request{.limit = 1});
         BOOST_TEST(legacy.items.empty());
      };
      forge::asio::blocking::run(app->runtime(), verify_reset());
      forge::asio::blocking::run(app->runtime(), dht->async_close());
      forge::asio::blocking::run(app->runtime(), reset->async_close());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto app = make_app(document_for(driver, path / "v2"));
      auto stores = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), stores->store(std::string{peer_store_name}));
      p2p_state_schema::register_objects(handle);
      auto seed_v2 = [&]() -> boost::asio::awaitable<void> {
         auto transaction = co_await handle.begin_transaction();
         auto objects = co_await handle.objects().join(transaction);
         auto state = p2p_state_schema::schema_state{};
         state.id = p2p_state_schema::schema_state_id;
         state.format_version = 2;
         co_await objects.insert(state);
         co_await objects.create<p2p_state_schema::peer_row>([&](p2p_state_schema::peer_row& row) {
            row.peer = peer.value;
            row.hydration_priority = std::uint64_t{1} << 63U;
            row.endpoints.push_back({.endpoint = "/dnsaddr/v2-cache.example/p2p/" + peer.to_string()});
         });
         co_await transaction.commit();
      };
      forge::asio::blocking::run(app->runtime(), seed_v2());

      BOOST_CHECK_THROW((forge::asio::blocking::run(app->runtime(), p2p_node::object_peer_state_adapter::async_open(
                                                                        stores.operator->(), handle, {}, false))),
                        forge::db::object::exceptions::incompatible_version);
      auto reset = forge::asio::blocking::run(
          app->runtime(), p2p_node::object_peer_state_adapter::async_open(stores.operator->(), handle, {}, true));
      const auto peer_page =
          forge::asio::blocking::run(app->runtime(), reset->async_hydrate(p2p::peer_store::hydration_request{
                                                         .kind = p2p::peer_store::hydration_kind::peers,
                                                         .limit = 10,
                                                     }));
      BOOST_TEST(peer_page.peers.empty());

      auto verify_reset = [&]() -> boost::asio::awaitable<void> {
         auto snapshot = co_await handle.begin_read();
         auto objects = snapshot.objects();
         const auto state = co_await objects.get(p2p_state_schema::schema_state_id);
         BOOST_TEST(state.format_version == p2p_state_schema::format_version);
      };
      forge::asio::blocking::run(app->runtime(), verify_reset());
      forge::asio::blocking::run(app->runtime(), reset->async_close());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto app = make_app(document_for(std::move(driver), path / "missing"));
      auto stores = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), stores->store(std::string{peer_store_name}));
      p2p_state_schema::register_objects(handle);
      auto seed_without_marker = [&]() -> boost::asio::awaitable<void> {
         auto transaction = co_await handle.begin_transaction();
         auto objects = co_await handle.objects().join(transaction);
         co_await objects.create<p2p_state_schema::peer_row>([&](p2p_state_schema::peer_row& row) {
            row.peer = peer.value;
            row.hydration_priority = std::uint64_t{1} << 63U;
         });
         co_await transaction.commit();
      };
      forge::asio::blocking::run(app->runtime(), seed_without_marker());

      BOOST_CHECK_THROW((forge::asio::blocking::run(app->runtime(), p2p_node::object_peer_state_adapter::async_open(
                                                                        stores.operator->(), handle, {}, false))),
                        forge::db::object::exceptions::incompatible_version);
      auto reset = forge::asio::blocking::run(
          app->runtime(), p2p_node::object_peer_state_adapter::async_open(stores.operator->(), handle, {}, true));
      const auto page =
          forge::asio::blocking::run(app->runtime(), reset->async_hydrate(p2p::peer_store::hydration_request{
                                                         .kind = p2p::peer_store::hydration_kind::peers,
                                                         .limit = 10,
                                                     }));
      BOOST_TEST(page.peers.empty());
      forge::asio::blocking::run(app->runtime(), reset->async_close());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}

void check_plugin_startup_rolls_back_open_peer_state(std::string driver, const std::filesystem::path& path) {
   const auto identity = forge::tests::p2p::make_identity_fixture("p2p-startup-rollback");
   const auto foreign = forge::tests::p2p::make_identity_fixture("p2p-startup-rollback-foreign");
   auto document = p2p_document_for(driver, path, identity);
   document.set("plugins.net.p2p.node.peer-id",
                p2p::make_peer_id_from_certificate_pem(foreign.certificate_pem).to_string());

   {
      auto app = build_p2p_app(std::move(document));
      BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), app->startup()), p2p::exceptions::invalid_identity);
   }

   auto reopened = make_app(document_for(std::move(driver), path));
   auto persistence = open_peer_persistence(*reopened);
   forge::asio::blocking::run(reopened->runtime(), persistence->async_close());
   forge::asio::blocking::run(reopened->runtime(), reopened->shutdown());
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_peer_state_persistence_test_suite)

#if FORGE_HAS_MDBX
BOOST_AUTO_TEST_CASE(p2p_peer_state_mdbx_preserves_eviction_and_sequence_state) {
   auto root = root_guard{};
   const auto path = root.root / "mdbx";
   check_object_peer_state_persistence([&] { return document_for("mdbx", path); });
}

BOOST_AUTO_TEST_CASE(p2p_plugin_mdbx_maintenance_prunes_expired_peer_state) {
   auto root = root_guard{};
   check_plugin_maintenance_prunes_expired_peer("mdbx", root.root / "mdbx-maintenance");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_mdbx_prune_rotates_bounded_categories) {
   auto root = root_guard{};
   check_object_peer_state_prune_rotates_categories("mdbx", root.root / "mdbx-prune-rotation");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_mdbx_prune_preserves_hydrated_identity_when_unhydrated_row_expires_first) {
   auto root = root_guard{};
   check_object_peer_state_prune_reports_exact_unhydrated_identity("mdbx", root.root / "mdbx-prune-exact");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_mdbx_durable_operations_flush_before_acknowledgement) {
   auto root = root_guard{};
   check_object_peer_state_durable_acknowledgement("mdbx", root.root / "mdbx-durable-ack");
}

BOOST_AUTO_TEST_CASE(p2p_state_schema_mdbx_retries_durable_flush_after_failure) {
   auto root = root_guard{};
   check_p2p_state_schema_flush_retries("mdbx", root.root / "mdbx-schema-flush-retry");
}

BOOST_AUTO_TEST_CASE(p2p_plugin_mdbx_startup_failure_rolls_back_open_peer_state) {
   auto root = root_guard{};
   check_plugin_startup_rolls_back_open_peer_state("mdbx", root.root / "mdbx-startup-rollback");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_mdbx_rejects_oversized_hydration_rows) {
   auto root = root_guard{};
   check_object_peer_state_rejects_oversized_hydration("mdbx", root.root / "mdbx-oversized-hydration");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_mdbx_clears_terminal_hydration_cursor) {
   auto root = root_guard{};
   check_object_peer_state_clears_terminal_hydration_cursor("mdbx", root.root / "mdbx-terminal-cursor");
}

BOOST_AUTO_TEST_CASE(p2p_dht_record_state_mdbx_reopens_prunes_and_isolates_profiles) {
   auto root = root_guard{};
   check_object_dht_record_persistence("mdbx", root.root / "mdbx-dht-records");
}

BOOST_AUTO_TEST_CASE(p2p_state_mdbx_rejects_or_resets_v1_v2_and_missing_markers) {
   auto root = root_guard{};
   check_object_p2p_state_incompatible_and_missing_marker_policy("mdbx", root.root / "mdbx-schema-policy");
}
#endif

#if FORGE_HAS_ROCKSDB
BOOST_AUTO_TEST_CASE(p2p_peer_state_rocksdb_preserves_eviction_and_sequence_state) {
   auto root = root_guard{};
   const auto path = root.root / "rocksdb";
   check_object_peer_state_persistence([&] { return document_for("rocksdb", path); });
}

BOOST_AUTO_TEST_CASE(p2p_plugin_rocksdb_maintenance_prunes_expired_peer_state) {
   auto root = root_guard{};
   check_plugin_maintenance_prunes_expired_peer("rocksdb", root.root / "rocksdb-maintenance");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_rocksdb_prune_rotates_bounded_categories) {
   auto root = root_guard{};
   check_object_peer_state_prune_rotates_categories("rocksdb", root.root / "rocksdb-prune-rotation");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_rocksdb_prune_preserves_hydrated_identity_when_unhydrated_row_expires_first) {
   auto root = root_guard{};
   check_object_peer_state_prune_reports_exact_unhydrated_identity("rocksdb", root.root / "rocksdb-prune-exact");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_rocksdb_durable_operations_flush_before_acknowledgement) {
   auto root = root_guard{};
   check_object_peer_state_durable_acknowledgement("rocksdb", root.root / "rocksdb-durable-ack");
}

BOOST_AUTO_TEST_CASE(p2p_state_schema_rocksdb_retries_durable_flush_after_failure) {
   auto root = root_guard{};
   check_p2p_state_schema_flush_retries("rocksdb", root.root / "rocksdb-schema-flush-retry");
}

BOOST_AUTO_TEST_CASE(p2p_plugin_rocksdb_startup_failure_rolls_back_open_peer_state) {
   auto root = root_guard{};
   check_plugin_startup_rolls_back_open_peer_state("rocksdb", root.root / "rocksdb-startup-rollback");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_rocksdb_rejects_oversized_hydration_rows) {
   auto root = root_guard{};
   check_object_peer_state_rejects_oversized_hydration("rocksdb", root.root / "rocksdb-oversized-hydration");
}

BOOST_AUTO_TEST_CASE(p2p_peer_state_rocksdb_clears_terminal_hydration_cursor) {
   auto root = root_guard{};
   check_object_peer_state_clears_terminal_hydration_cursor("rocksdb", root.root / "rocksdb-terminal-cursor");
}

BOOST_AUTO_TEST_CASE(p2p_dht_record_state_rocksdb_reopens_prunes_and_isolates_profiles) {
   auto root = root_guard{};
   check_object_dht_record_persistence("rocksdb", root.root / "rocksdb-dht-records");
}

BOOST_AUTO_TEST_CASE(p2p_state_rocksdb_rejects_or_resets_v1_v2_and_missing_markers) {
   auto root = root_guard{};
   check_object_p2p_state_incompatible_and_missing_marker_policy("rocksdb", root.root / "rocksdb-schema-policy");
}
#endif

BOOST_AUTO_TEST_SUITE_END()
