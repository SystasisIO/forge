#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/db/object/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.binding;
import forge.api.core.registry;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.app.events;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.app.signals;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.db.authenticated.proof;
import forge.db.authenticated.store;
import forge.db.authenticated.types;
import forge.db.ids.object_id;
import forge.db.blob.ref;
import forge.db.blob.snapshot;
import forge.db.blob.store;
import forge.db.blob.transaction;
import forge.db.blob.types;
import forge.db.object.cursor;
import forge.db.object.exceptions;
import forge.db.object.header;
import forge.db.object.hooks;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.store;
import forge.db.revision.exceptions;
import forge.db.revision.store;
import forge.db.revision.transaction;
import forge.db.revision.types;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.crypto.secrets.types;
import forge.plugins.db.store.api;
import forge.plugins.db.store.exceptions;
import forge.plugins.db.store.plugin;
import forge.plugins.db.store.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.types;
import forge.raw.raw;

#if FORGE_HAS_ROCKSDB
import forge.db.rocksdb.driver;
#endif

#if FORGE_HAS_MDBX
import forge.asio.affine;
import forge.db.mdbx.driver;
#endif

namespace {

namespace crypto_secrets = forge::plugins::crypto::secrets;
namespace p2p_node = forge::plugins::net::p2p::node;
namespace store_plugin = forge::plugins::db::store;

struct by_id;
struct by_name;
struct by_path;
struct by_usage_id;
struct by_usage_state;
struct by_usage_bytes;

struct account : forge::db::object::object<account, 1, 7> {
   std::string name;
   std::uint64_t balance = 0;

   bool operator==(const account&) const = default;
};

BOOST_DESCRIBE_STRUCT(account, (forge::db::object::object<account, 1, 7>), (name, balance))

using account_object = forge::db::object::object_index<
    account, forge::db::object::indexed_by<
                 forge::db::object::primary_unique<by_id>,
                 forge::db::object::ordered_unique<by_name, forge::db::object::member<&account::name>>>>;

class setup_interceptor final : public forge::db::object::interceptor {
 public:
   boost::asio::awaitable<void> before_mutation(const forge::db::object::object_mutation&) override {
      co_return;
   }
};

class setup_observer final : public forge::db::object::observer {
 public:
   boost::asio::awaitable<void> after_commit(const forge::db::object::change_set&) override {
      co_return;
   }
};

class contract_store_handle_state final : public store_plugin::store_handle_state {
 public:
   [[nodiscard]] std::string name() const override {
      return "legacy";
   }

   [[nodiscard]] std::shared_ptr<forge::db::core::driver> require_driver() const override {
      return {};
   }

   [[nodiscard]] std::shared_ptr<forge::db::object::store> require_objects() const override {
      return {};
   }

   [[nodiscard]] std::shared_ptr<forge::db::blob::store> require_blobs() const override {
      return {};
   }

   boost::asio::awaitable<store_plugin::transaction> begin_transaction() const override {
      co_return store_plugin::transaction{};
   }
};

static_assert(!std::is_abstract_v<contract_store_handle_state>);

struct file_record : forge::db::object::object<file_record, 1, 8> {
   std::string path;
   forge::db::blob::ref<> content;

   bool operator==(const file_record&) const = default;
};

BOOST_DESCRIBE_STRUCT(file_record, (forge::db::object::object<file_record, 1, 8>), (path, content))

using file_object = forge::db::object::object_index<
    file_record, forge::db::object::indexed_by<
                     forge::db::object::primary_unique<by_id>,
                     forge::db::object::ordered_unique<by_path, forge::db::object::member<&file_record::path>>>>;

struct usage_record : forge::db::object::object<usage_record, 1, 9> {
   std::uint32_t state = 0;
   std::uint64_t bytes = 0;

   bool operator==(const usage_record&) const = default;
};

BOOST_DESCRIBE_STRUCT(usage_record, (forge::db::object::object<usage_record, 1, 9>), (state, bytes))

using usage_object = forge::db::object::object_index<
    usage_record,
    forge::db::object::indexed_by<
        forge::db::object::ranked_primary_unique<
            by_usage_id, forge::db::object::ranked_schema<1>,
            forge::db::object::sum<by_usage_bytes, forge::db::object::member<&usage_record::bytes>>>,
        forge::db::object::ranked_non_unique<
            by_usage_state, forge::db::object::member<&usage_record::state>, forge::db::object::ranked_schema<1>,
            forge::db::object::sum<by_usage_bytes, forge::db::object::member<&usage_record::bytes>>>>>;

struct byte_less {
   bool operator()(const forge::db::core::record_key& left, const forge::db::core::record_key& right) const {
      return left.bytes() < right.bytes();
   }
};

struct memory_state {
   std::map<std::string, std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less>> records;
   std::size_t flush_calls = 0;
   std::size_t snapshot_calls = 0;
   std::size_t active_writes = 0;
   bool overlapping_writes = false;
};

class memory_session final : public forge::db::core::session {
 public:
   memory_session(std::shared_ptr<memory_state> state, bool writes)
       : state_{std::move(state)}, writes_{writes}, working_{state_->records} {
      if (writes_) {
         ++state_->active_writes;
         if (state_->active_writes > 1U) {
            state_->overlapping_writes = true;
         }
      }
   }

   ~memory_session() override {
      close();
   }

   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{
          .snapshot_reads = !writes_,
          .writes = writes_,
          .savepoints = writes_,
          .record_locks = writes_,
      };
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family family,
                                                                     forge::db::core::record_key key) override {
      const auto family_found = working_.find(family.name);
      if (family_found == working_.end()) {
         co_return std::nullopt;
      }
      const auto found = family_found->second.find(key);
      if (found == family_found->second.end()) {
         co_return std::nullopt;
      }
      co_return found->second;
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>>
   get_for_update(forge::db::core::family family, forge::db::core::record_key key) override {
      if (!writes_) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation,
                               "test snapshot cannot lock records");
      }
      co_return co_await get(std::move(family), std::move(key));
   }

   boost::asio::awaitable<void> put(forge::db::core::family family, forge::db::core::record_key key,
                                    std::vector<std::byte> value) override {
      if (!writes_) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "test snapshot is read-only");
      }
      working_[family.name][std::move(key)] = std::move(value);
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family family, forge::db::core::record_key key) override {
      if (!writes_) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "test snapshot is read-only");
      }
      working_[family.name].erase(key);
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page> scan_page(forge::db::core::family family,
                                                                  forge::db::core::record_range range,
                                                                  forge::db::core::page_request request) override {
      forge::db::object::validate_page_request(request);

      auto result = forge::db::core::record_page{};
      auto& records = working_[family.name];
      auto current = records.lower_bound(request.after ? request.after->boundary : range.begin);
      if (request.after && current != records.end() && current->first == request.after->boundary) {
         ++current;
      }

      auto last_returned = std::optional<forge::db::core::record_key>{};
      while (current != records.end()) {
         if (range.has_end && !(current->first.bytes() < range.end.bytes())) {
            break;
         }
         result.entries.push_back(forge::db::core::record_entry{.key = current->first, .value = current->second});
         last_returned = current->first;
         ++current;
         if (result.entries.size() == request.limit) {
            break;
         }
      }

      if (current != records.end() && (!range.has_end || current->first.bytes() < range.end.bytes())) {
         result.next = forge::db::core::cursor{.boundary = std::move(*last_returned)};
      }

      co_return result;
   }

   boost::asio::awaitable<void> commit() override {
      if (!writes_) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "test snapshot cannot commit");
      }
      state_->records = std::move(working_);
      close();
      co_return;
   }

   boost::asio::awaitable<void> rollback() override {
      close();
      co_return;
   }

   boost::asio::awaitable<void> create_savepoint() override {
      savepoints_.push_back(working_);
      co_return;
   }

   boost::asio::awaitable<void> rollback_to_savepoint() override {
      if (savepoints_.empty()) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "test savepoint stack is empty");
      }
      working_ = std::move(savepoints_.back());
      savepoints_.pop_back();
      co_return;
   }

   boost::asio::awaitable<void> release_savepoint() override {
      if (savepoints_.empty()) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "test savepoint stack is empty");
      }
      savepoints_.pop_back();
      co_return;
   }

 private:
   void close() noexcept {
      if (writes_ && !closed_) {
         closed_ = true;
         --state_->active_writes;
      }
   }

   std::shared_ptr<memory_state> state_;
   bool writes_ = false;
   bool closed_ = false;
   std::map<std::string, std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less>> working_;
   std::vector<decltype(working_)> savepoints_;
};

class memory_driver final : public forge::db::core::driver {
 public:
   boost::asio::awaitable<void> async_flush(bool) override {
      ++state_->flush_calls;
      co_return;
   }

   [[nodiscard]] std::size_t flush_calls() const noexcept {
      return state_->flush_calls;
   }

   [[nodiscard]] std::size_t snapshot_calls() const noexcept {
      return state_->snapshot_calls;
   }

   [[nodiscard]] std::size_t active_writes() const noexcept {
      return state_->active_writes;
   }

   [[nodiscard]] bool overlapping_writes() const noexcept {
      return state_->overlapping_writes;
   }

   [[nodiscard]] std::size_t close_calls() const noexcept {
      return close_calls_;
   }

   void seed_record(forge::db::core::family family, forge::db::core::record_key key, std::vector<std::byte> value) {
      state_->records[family.name][std::move(key)] = std::move(value);
   }

 private:
   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_transaction() override {
      co_return std::make_unique<memory_session>(state_, true);
   }

   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_snapshot() override {
      ++state_->snapshot_calls;
      co_return std::make_unique<memory_session>(state_, false);
   }

   boost::asio::awaitable<void> close_driver() override {
      ++close_calls_;
      co_return;
   }

   std::shared_ptr<memory_state> state_ = std::make_shared<memory_state>();
   std::size_t close_calls_ = 0;
};

class installer_plugin final : public forge::app::plugin {
 public:
   explicit installer_plugin(std::shared_ptr<memory_driver> driver, std::string store_name = "accounts")
       : driver_{std::move(driver)}, store_name_{std::move(store_name)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "test.plugins.db.store.installer"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto api = context.apis().get<store_plugin::api>(store_plugin::api::ref());
      co_await api->add_store(store_name_, driver_);
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::shared_ptr<memory_driver> driver_;
   std::string store_name_;
};

[[nodiscard]] forge::app::plugin_descriptor installer_descriptor(std::shared_ptr<memory_driver> driver,
                                                                 std::string store_name = "accounts") {
   return forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "test.plugins.db.store.installer"},
       .dependencies = {forge::app::plugin_id{.value = "forge.plugins.db.store"}},
       .factory = [driver = std::move(driver),
                   store_name =
                       std::move(store_name)] { return std::make_unique<installer_plugin>(driver, store_name); },
   };
}

[[nodiscard]] std::unique_ptr<forge::app::application_shell> make_app(forge::config::core::document document = {},
                                                                      std::shared_ptr<memory_driver> driver = {}) {
   auto builder = forge::app::application_builder{};
   builder.name("db-store-plugin-test")
       .runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "db-store-plugin-test"})
       .plugin(store_plugin::descriptor());
   if (driver) {
      builder.plugin(installer_descriptor(std::move(driver)));
   }

   auto app = std::move(builder).build();
   app->configure(document);
   forge::asio::blocking::run(app->runtime(), app->startup());
   return app;
}

[[nodiscard]] account make_account(std::uint64_t instance, std::string name, std::uint64_t balance) {
   auto value = account{};
   value.id = decltype(value.id){instance};
   value.name = std::move(name);
   value.balance = balance;
   return value;
}

[[nodiscard]] file_record make_file(std::uint64_t instance, std::string path, forge::db::blob::ref<> content) {
   auto value = file_record{};
   value.id = decltype(value.id){instance};
   value.path = std::move(path);
   value.content = std::move(content);
   return value;
}

[[nodiscard]] std::vector<std::byte> bytes(std::string_view value) {
   return std::vector<std::byte>{
       reinterpret_cast<const std::byte*>(value.data()),
       reinterpret_cast<const std::byte*>(value.data() + value.size()),
   };
}

constexpr auto p2p_peer_store_name = std::string_view{"p2p-peer-state"};
constexpr auto p2p_certificate_secret_id = std::string_view{"p2p/test-certificate"};
constexpr auto p2p_private_key_secret_id = std::string_view{"p2p/test-private-key"};

[[nodiscard]] forge::config::core::value p2p_secret(std::string id, std::string_view material, std::string purpose) {
   auto source = forge::config::core::value::object_type{};
   source.emplace("type", forge::config::core::value{"value"});
   source.emplace("encoding", forge::config::core::value{"raw"});
   source.emplace("value", forge::config::core::value{std::string{material}});

   auto secret = forge::config::core::value::object_type{};
   secret.emplace("id", forge::config::core::value{std::move(id)});
   secret.emplace("kind", forge::config::core::value{"bytes"});
   secret.emplace("source", forge::config::core::value{std::move(source)});
   secret.emplace("purposes", forge::config::core::value::array_type{
                                  forge::config::core::value{std::move(purpose)},
                              });
   secret.emplace("operations", forge::config::core::value::array_type{
                                    forge::config::core::value{"get_bytes"},
                                });
   secret.emplace("allow-raw-export", forge::config::core::value{true});
   return forge::config::core::value{std::move(secret)};
}

[[nodiscard]] forge::config::core::document
p2p_production_config(const forge::tests::p2p::identity_fixture& identity,
                      std::string peer_store = std::string{p2p_peer_store_name}, bool include_private_key = true) {
   auto secrets = forge::config::core::value::array_type{};
   secrets.push_back(
       p2p_secret(std::string{p2p_certificate_secret_id}, identity.certificate_pem, "p2p.identity.certificate"));
   if (include_private_key) {
      secrets.push_back(
          p2p_secret(std::string{p2p_private_key_secret_id}, identity.private_key_pem, "p2p.identity.private-key"));
   }

   auto document = forge::config::core::document{};
   document.set("plugins.crypto.secrets.secrets", std::move(secrets));
   document.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                               forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"},
                                           });
   document.set("plugins.net.p2p.node.peer-store.store", std::move(peer_store));
   document.set("plugins.net.p2p.node.identity.certificate-secret", std::string{p2p_certificate_secret_id});
   document.set("plugins.net.p2p.node.identity.private-key-secret", std::string{p2p_private_key_secret_id});
   return document;
}

void set_p2p_bootstrap(forge::config::core::document& document, const std::optional<std::string>& bootstrap) {
   if (bootstrap) {
      document.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                     forge::config::core::value{*bootstrap},
                                                 });
   }
}

[[nodiscard]] std::unique_ptr<forge::app::application_shell> make_p2p_app(std::shared_ptr<memory_driver> driver,
                                                                          forge::config::core::document document) {
   auto builder = forge::app::application_builder{};
   builder.name("p2p-node-db-store-lifecycle-test")
       .runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "p2p-node-db-store-test"})
       .plugin(store_plugin::descriptor())
       .plugin(crypto_secrets::descriptor())
       .plugin(p2p_node::descriptor());
   if (driver) {
      builder.plugin(installer_descriptor(std::move(driver), std::string{p2p_peer_store_name}));
   }

   auto app = std::move(builder).build();
   app->configure(document);
   return app;
}

[[nodiscard]] forge::db::core::record_key header_record_key() {
   return forge::db::core::record_key{std::vector<std::byte>(11U, std::byte{0U})};
}

[[nodiscard]] std::vector<std::byte> packed_header(std::uint32_t version) {
   auto value = forge::db::object::header{};
   value.id = forge::db::object::header_id;
   value.version = version;
   const auto packed = forge::raw::pack(value);
   auto result = std::vector<std::byte>{};
   result.reserve(packed.size());
   for (const auto byte : packed) {
      result.push_back(static_cast<std::byte>(byte));
   }
   return result;
}

[[nodiscard]] const forge::config::core::field_descriptor&
require_field(const forge::config::core::component_descriptor& descriptor, const std::string& name) {
   const auto found = std::ranges::find_if(descriptor.fields, [&](const auto& field) { return field.name == name; });
   BOOST_REQUIRE(found != descriptor.fields.end());
   return *found;
}

[[nodiscard]] forge::config::core::value configured_store(std::string name, std::filesystem::path path,
                                                          bool revision = false) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("name", forge::config::core::value{std::move(name)});
   object.emplace("driver", forge::config::core::value{std::string{"rocksdb"}});
   object.emplace("path", forge::config::core::value{path.string()});
   auto object_layer = forge::config::core::value::object_type{};
   object_layer.emplace("family", forge::config::core::value{std::string{"objectdb"}});
   object_layer.emplace("write-policy", forge::config::core::value{std::string{"single-writer"}});
   object.emplace("object", forge::config::core::value{std::move(object_layer)});
   if (revision) {
      object.emplace("revision", forge::config::core::value{forge::config::core::value::object_type{}});
   }
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value configured_object_blob_store(std::string name, std::filesystem::path path,
                                                                      bool revision = false) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("name", forge::config::core::value{std::move(name)});
   object.emplace("driver", forge::config::core::value{std::string{"rocksdb"}});
   object.emplace("path", forge::config::core::value{path.string()});

   auto object_layer = forge::config::core::value::object_type{};
   object_layer.emplace("family", forge::config::core::value{std::string{"objectdb"}});
   object_layer.emplace("write-policy", forge::config::core::value{std::string{"single-writer"}});
   object.emplace("object", forge::config::core::value{std::move(object_layer)});

   auto data_blobs = forge::config::core::value::object_type{};
   data_blobs.emplace("enable-blob-files", forge::config::core::value{true});
   data_blobs.emplace("min-blob-size", forge::config::core::value{std::uint64_t{16U}});

   auto blob_layer = forge::config::core::value::object_type{};
   blob_layer.emplace("data-family", forge::config::core::value{std::string{"blobdb.data"}});
   blob_layer.emplace("refs-family", forge::config::core::value{std::string{"blobdb.refs"}});
   blob_layer.emplace("data-blobs", forge::config::core::value{std::move(data_blobs)});
   object.emplace("blob", forge::config::core::value{std::move(blob_layer)});
   if (revision) {
      object.emplace("revision", forge::config::core::value{forge::config::core::value::object_type{}});
   }

   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value configured_mdbx_store(std::string name, std::filesystem::path path,
                                                               bool blob = false, bool revision = true,
                                                               std::optional<std::string> durability = "durable-sync",
                                                               std::vector<std::string> families = {}) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("name", forge::config::core::value{std::move(name)});
   object.emplace("driver", forge::config::core::value{std::string{"mdbx"}});
   object.emplace("path", forge::config::core::value{path.string()});

   if (!families.empty()) {
      auto configured_families = forge::config::core::value::array_type{};
      configured_families.reserve(families.size());
      for (auto& family : families) {
         configured_families.emplace_back(std::move(family));
      }
      object.emplace("families", forge::config::core::value{std::move(configured_families)});
   }

   if (durability) {
      auto lane = forge::config::core::value::object_type{};
      lane.emplace("max-pending-operations", forge::config::core::value{std::uint64_t{64U}});
      lane.emplace("max-waiting-submissions", forge::config::core::value{std::uint64_t{64U}});
      lane.emplace("thread-name", forge::config::core::value{std::string{"store-mdbx-test"}});

      auto map = forge::config::core::value::object_type{};
      map.emplace("upper-size", forge::config::core::value{std::uint64_t{64U * 1024U * 1024U}});
      map.emplace("growth-step", forge::config::core::value{std::uint64_t{1024U * 1024U}});

      auto mdbx = forge::config::core::value::object_type{};
      mdbx.emplace("durability", forge::config::core::value{std::move(*durability)});
      mdbx.emplace("max-readers", forge::config::core::value{std::uint64_t{64U}});
      mdbx.emplace("map", forge::config::core::value{std::move(map)});
      mdbx.emplace("lane", forge::config::core::value{std::move(lane)});
      object.emplace("mdbx", forge::config::core::value{std::move(mdbx)});
   }

   auto object_layer = forge::config::core::value::object_type{};
   object_layer.emplace("family", forge::config::core::value{std::string{"objectdb"}});
   object_layer.emplace("write-policy", forge::config::core::value{std::string{"single-writer"}});
   object.emplace("object", forge::config::core::value{std::move(object_layer)});

   if (blob) {
      auto blob_layer = forge::config::core::value::object_type{};
      blob_layer.emplace("data-family", forge::config::core::value{std::string{"blobdb.data"}});
      blob_layer.emplace("refs-family", forge::config::core::value{std::string{"blobdb.refs"}});
      object.emplace("blob", forge::config::core::value{std::move(blob_layer)});
   }
   if (revision) {
      object.emplace("revision", forge::config::core::value{forge::config::core::value::object_type{}});
   }

   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::document document_for_rocksdb(const std::filesystem::path& path) {
   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores", forge::config::core::value::array_type{configured_store("accounts", path)});
   return document;
}

[[nodiscard]] forge::config::core::document document_for_mdbx(const std::filesystem::path& path, bool blob = false,
                                                              bool revision = true,
                                                              std::string durability = "durable-sync") {
   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores", forge::config::core::value::array_type{configured_mdbx_store(
                                               "files", path, blob, revision, std::move(durability))});
   return document;
}

struct root_guard {
   std::filesystem::path root =
       std::filesystem::temp_directory_path() /
       ("forge_db_store_plugin_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

   root_guard() {
      std::filesystem::remove_all(root);
   }

   ~root_guard() {
      std::filesystem::remove_all(root);
   }
};

void check_p2p_started(forge::app::application_shell& app) {
   auto p2p = app.apis().get<p2p_node::api>(p2p_node::api::ref());
   const auto info = p2p->network_info();
   BOOST_TEST(info.started);
   BOOST_REQUIRE_EQUAL(info.local_endpoints.size(), 1U);
   BOOST_TEST(info.local_endpoints.front().is_direct_quic());
   BOOST_TEST(info.local_endpoints.front().transport.host == "127.0.0.1");
   BOOST_TEST(info.local_endpoints.front().transport.port != 0U);
   BOOST_REQUIRE(info.local_endpoints.front().peer.has_value());
}

void check_p2p_identity_secret_purposes(forge::app::application_shell& app) {
   auto secrets = app.apis().get<crypto_secrets::api>(crypto_secrets::api::ref());
   const auto status = forge::asio::blocking::run(app.runtime(), secrets->status({}));
   BOOST_REQUIRE_EQUAL(status.configured_secrets, 2U);

   const auto certificate = std::find_if(status.secrets.begin(), status.secrets.end(),
                                         [](const auto& secret) { return secret.id == p2p_certificate_secret_id; });
   BOOST_REQUIRE(certificate != status.secrets.end());
   BOOST_REQUIRE_EQUAL(certificate->purposes.size(), 1U);
   BOOST_TEST(certificate->purposes.front() == "p2p.identity.certificate");

   const auto private_key = std::find_if(status.secrets.begin(), status.secrets.end(),
                                         [](const auto& secret) { return secret.id == p2p_private_key_secret_id; });
   BOOST_REQUIRE(private_key != status.secrets.end());
   BOOST_REQUIRE_EQUAL(private_key->purposes.size(), 1U);
   BOOST_TEST(private_key->purposes.front() == "p2p.identity.private-key");
}

void check_p2p_peer_visible(forge::app::application_shell& app, const forge::net::p2p::peer_id& expected) {
   auto diagnostics = app.apis().get<p2p_node::diagnostics_source>(p2p_node::diagnostics_source::ref());
   const auto snapshot = diagnostics->snapshot();
   BOOST_TEST(std::ranges::any_of(snapshot.peers, [&](const auto& peer) { return peer.peer == expected; }));
}

template <typename AppFactory> void check_p2p_private_peer_state_reopens(AppFactory&& make_client) {
   const auto server_identity = forge::tests::p2p::make_identity_fixture("p2p-peer-state-server");
   const auto client_identity = forge::tests::p2p::make_identity_fixture("p2p-peer-state-client");
   auto server = make_p2p_app(std::make_shared<memory_driver>(), p2p_production_config(server_identity));
   forge::asio::blocking::run(server->runtime(), server->startup());
   auto server_p2p = server->apis().get<p2p_node::api>(p2p_node::api::ref());
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());
   BOOST_REQUIRE(server_endpoint->peer.has_value());
   const auto server_peer = *server_endpoint->peer;

   {
      auto client = make_client(client_identity, std::optional<std::string>{server_endpoint->to_string()});
      forge::asio::blocking::run(client->runtime(), client->startup());
      check_p2p_started(*client);
      check_p2p_identity_secret_purposes(*client);
      check_p2p_peer_visible(*client, server_peer);
      forge::asio::blocking::run(client->runtime(), client->shutdown());
   }

   forge::asio::blocking::run(server->runtime(), server->shutdown());

   {
      auto reopened = make_client(client_identity, std::nullopt);
      forge::asio::blocking::run(reopened->runtime(), reopened->startup());
      check_p2p_started(*reopened);
      check_p2p_peer_visible(*reopened, server_peer);
      forge::asio::blocking::run(reopened->runtime(), reopened->shutdown());
   }
}

#if FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB
void configure_static_topology(forge::config::core::document& document) {
   document.set("plugins.net.p2p.node.topology.mode", std::string{"static-only"});
   document.set("plugins.net.p2p.node.topology.peers.low", std::uint64_t{3});
   document.set("plugins.net.p2p.node.topology.peers.target", std::uint64_t{5});
   document.set("plugins.net.p2p.node.topology.peers.high", std::uint64_t{7});
   document.set("plugins.net.p2p.node.topology.refresh-interval-ms", std::uint64_t{321});
   document.set("plugins.net.p2p.node.topology.query-timeout-ms", std::uint64_t{123});
   document.set("plugins.net.p2p.node.topology.max-candidates", std::uint64_t{11});
   document.set("plugins.net.p2p.node.topology.max-parallel-queries", std::uint64_t{2});
   document.set("plugins.net.p2p.node.topology.max-parallel-dials", std::uint64_t{1});
   document.set("plugins.net.p2p.node.peer-exchange.enabled", true);
   document.set("plugins.net.p2p.node.peer-exchange.max-peers", std::uint64_t{2});
}

void check_static_topology_snapshot(forge::app::application_shell& app) {
   auto diagnostics = app.apis().get<p2p_node::diagnostics_source>(p2p_node::diagnostics_source::ref());
   const auto snapshot = diagnostics->snapshot();
   BOOST_TEST(snapshot.topology.mode == "static-only");
   BOOST_TEST(snapshot.topology.phase == "idle");
   BOOST_TEST(snapshot.topology.low_watermark == 3U);
   BOOST_TEST(snapshot.topology.target_watermark == 5U);
   BOOST_TEST(snapshot.topology.high_watermark == 7U);
   BOOST_TEST(snapshot.topology.refresh_interval == std::chrono::milliseconds{321});
   BOOST_TEST(snapshot.topology.query_timeout == std::chrono::milliseconds{123});
   BOOST_TEST(snapshot.topology.max_candidates == 11U);
   BOOST_TEST(snapshot.topology.max_parallel_queries == 2U);
   BOOST_TEST(snapshot.topology.max_parallel_dials == 1U);
   BOOST_TEST(snapshot.topology.max_peer_exchange_peers == 2U);
   BOOST_TEST(!snapshot.topology.dht_enabled);
   BOOST_TEST(snapshot.topology.peer_exchange_enabled);
   BOOST_TEST(snapshot.topology.completed_refreshes == 0U);
   BOOST_TEST(!snapshot.topology.refresh_queued);
   BOOST_TEST(!snapshot.topology.refresh_in_flight);
   BOOST_TEST(snapshot.topology.active_operations == 0U);
   BOOST_TEST(snapshot.connections.active_sessions == 0U);
   BOOST_TEST(snapshot.metrics.sessions_opened == 0U);
   BOOST_TEST(snapshot.metrics.dht_queries == 0U);
   BOOST_TEST(snapshot.metrics.peer_exchange_messages == 0U);
   BOOST_TEST(snapshot.metrics.rendezvous_registrations == 0U);
   BOOST_TEST(snapshot.metrics.rendezvous_discovers == 0U);
}

template <typename AppFactory> void check_p2p_static_topology_reopens(AppFactory&& make_client) {
   const auto identity = forge::tests::p2p::make_identity_fixture("p2p-static-topology-reopen");
   for (auto attempt = 0U; attempt < 2U; ++attempt) {
      auto app = make_client(identity);
      forge::asio::blocking::run(app->runtime(), app->startup());
      check_static_topology_snapshot(*app);
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}
#endif

} // namespace

FORGE_DB_OBJECT(account_object)
FORGE_DB_OBJECT(file_object)
FORGE_DB_OBJECT(usage_object)

template <typename Handle>
concept can_register_system_header =
    requires(const Handle& handle) { handle.template register_object<forge::db::object::header_index>(); };

template <typename Handle>
concept can_insert_system_header =
    requires(const Handle& handle, forge::db::object::header value) { handle.insert(value); };

static_assert(!can_register_system_header<store_plugin::object_handle>);
static_assert(!can_insert_system_header<store_plugin::object_handle>);

BOOST_AUTO_TEST_SUITE(store_plugin_test_suite)

BOOST_AUTO_TEST_CASE(store_plugin_descriptor_api_and_config_are_nested) {
   auto plugin = store_plugin::plugin{};
   BOOST_TEST(plugin.id().value == "forge.plugins.db.store");
   BOOST_TEST(plugin.version() == "2.0.0");
   BOOST_TEST(store_plugin::api::ref().id.value == "forge.plugins.db.store");

   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == "plugins.db.store");

   const auto& stores = require_field(*descriptor, "stores");
   BOOST_TEST(!stores.has_default);

   const auto api_descriptor = store_plugin::api::describe();
   BOOST_TEST(api_descriptor.id.value == "forge.plugins.db.store");
   BOOST_TEST(api_descriptor.version.major == 2U);
   BOOST_TEST(api_descriptor.version.revision == 0U);
   BOOST_TEST(api_descriptor.methods.empty());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_descriptor_keeps_production_dependencies_and_api_major) {
   const auto descriptor = p2p_node::descriptor();
   BOOST_TEST(descriptor.id.value == "forge.plugins.net.p2p.node");
   BOOST_REQUIRE_EQUAL(descriptor.dependencies.size(), 2U);
   BOOST_TEST(descriptor.dependencies[0].value == "forge.plugins.db.store");
   BOOST_TEST(descriptor.dependencies[1].value == "forge.plugins.crypto.secrets");

   const auto plugin = descriptor.factory();
   BOOST_REQUIRE(plugin != nullptr);
   BOOST_TEST(plugin->version() == "6.0.0");

   const auto api_descriptor = p2p_node::api::describe();
   BOOST_TEST(api_descriptor.id.value == "forge.plugins.net.p2p.node");
   BOOST_TEST(api_descriptor.version.major == 2U);

   const auto diagnostics_source_descriptor = p2p_node::diagnostics_source::describe();
   BOOST_TEST(diagnostics_source_descriptor.id.value == "forge.plugins.net.p2p.node.diagnostics_source");
   BOOST_TEST(diagnostics_source_descriptor.version.major == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_production_lifecycle_reopens_persisted_peer_state) {
   auto driver = std::make_shared<memory_driver>();
   check_p2p_private_peer_state_reopens(
       [driver](const forge::tests::p2p::identity_fixture& identity, const std::optional<std::string>& bootstrap) {
          auto document = p2p_production_config(identity);
          set_p2p_bootstrap(document, bootstrap);
          return make_p2p_app(driver, std::move(document));
       });
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_rejects_unknown_named_peer_store_before_listening) {
   const auto identity = forge::tests::p2p::make_identity_fixture("p2p-unknown-store");
   auto app = make_p2p_app(std::make_shared<memory_driver>(), p2p_production_config(identity, "missing-peer-store"));

   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), app->startup()),
                     store_plugin::exceptions::unknown_store);

   auto p2p = app->apis().get<p2p_node::api>(p2p_node::api::ref());
   BOOST_CHECK_THROW((void)p2p->local_endpoint(), p2p_node::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_rejects_missing_identity_secret_before_listening) {
   const auto identity = forge::tests::p2p::make_identity_fixture("p2p-missing-secret");
   auto app = make_p2p_app(std::make_shared<memory_driver>(),
                           p2p_production_config(identity, std::string{p2p_peer_store_name}, false));

   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), app->startup()),
                     crypto_secrets::exceptions::secret_not_found);

   auto p2p = app->apis().get<p2p_node::api>(p2p_node::api::ref());
   BOOST_CHECK_THROW((void)p2p->local_endpoint(), p2p_node::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_invalid_programmatic_setup) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   auto driver = std::make_shared<memory_driver>();

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("", driver)),
                     store_plugin::exceptions::invalid_argument);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("bad", nullptr)),
                     store_plugin::exceptions::invalid_argument);

   auto revision_without_object = store_plugin::store_options{};
   revision_without_object.object.reset();
   revision_without_object.blob = store_plugin::blob_layer_options{};
   revision_without_object.revision = store_plugin::revision_layer_options{};
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(runtime, api->add_store("revision-without-object", driver, revision_without_object)),
       store_plugin::exceptions::invalid_argument);

   forge::asio::blocking::run(runtime, api->add_store("accounts", driver));
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("accounts", driver)),
                     store_plugin::exceptions::duplicate_store);

   forge::asio::blocking::run(runtime, plugin.after_initialize());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("ready-late", driver)),
                     store_plugin::exceptions::stopped);
   forge::asio::blocking::run(runtime, plugin.startup());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("late", driver)),
                     store_plugin::exceptions::stopped);

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_does_not_publish_object_store_with_incompatible_header) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();
   driver->seed_record(forge::db::core::family{"objectdb"}, header_record_key(),
                       packed_header(forge::db::object::header::current_version + 1U));

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("accounts", driver));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.startup()), store_plugin::exceptions::startup_failed);

   const auto status = forge::asio::blocking::run(runtime, api->status());
   BOOST_REQUIRE_EQUAL(status.stores.size(), 1U);
   BOOST_CHECK(!status.stores.front().started);

   const auto handle = forge::asio::blocking::run(runtime, api->store("accounts"));
   BOOST_CHECK_THROW((void)handle.objects(), store_plugin::exceptions::stopped);
   BOOST_CHECK_EQUAL(driver->active_writes(), 0U);

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_programmatic_overlapping_layer_families) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   auto driver = std::make_shared<memory_driver>();

   auto object_data_overlap = store_plugin::store_options{};
   object_data_overlap.object = store_plugin::object_layer_options{.family = forge::db::core::family{"shared"}};
   object_data_overlap.blob = store_plugin::blob_layer_options{
       .data_family = forge::db::core::family{"shared"},
       .refs_family = forge::db::core::family{"blob.refs"},
   };
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("object-data", driver, object_data_overlap)),
                     store_plugin::exceptions::invalid_argument);

   auto object_refs_overlap = store_plugin::store_options{};
   object_refs_overlap.object = store_plugin::object_layer_options{.family = forge::db::core::family{"shared"}};
   object_refs_overlap.blob = store_plugin::blob_layer_options{
       .data_family = forge::db::core::family{"blob.data"},
       .refs_family = forge::db::core::family{"shared"},
   };
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("object-refs", driver, object_refs_overlap)),
                     store_plugin::exceptions::invalid_argument);

   auto blob_overlap = store_plugin::store_options{};
   blob_overlap.blob = store_plugin::blob_layer_options{
       .data_family = forge::db::core::family{"blob.shared"},
       .refs_family = forge::db::core::family{"blob.shared"},
   };
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->add_store("blob-overlap", driver, blob_overlap)),
                     store_plugin::exceptions::invalid_argument);
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_duplicate_configured_store_names) {
   auto runtime = forge::asio::runtime{};
   auto plugin = store_plugin::plugin{};
   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores", forge::config::core::value::array_type{
                                               configured_store("accounts", "/tmp/forge-db-store-plugin-duplicate-a"),
                                               configured_store("accounts", "/tmp/forge-db-store-plugin-duplicate-b"),
                                           });

   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         runtime, plugin.configure(forge::config::core::component_view{document, "plugins.db.store"})),
                     store_plugin::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_configured_store_without_layers) {
   auto runtime = forge::asio::runtime{};
   auto plugin = store_plugin::plugin{};
   auto store = forge::config::core::value::object_type{};
   store.emplace("name", forge::config::core::value{std::string{"empty"}});
   store.emplace("driver", forge::config::core::value{std::string{"rocksdb"}});
   store.emplace("path", forge::config::core::value{std::string{"/tmp/forge-db-store-plugin-no-layers"}});

   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores",
                forge::config::core::value::array_type{forge::config::core::value{std::move(store)}});

   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         runtime, plugin.configure(forge::config::core::component_view{document, "plugins.db.store"})),
                     store_plugin::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_invalid_object_id_allocation_configuration) {
   auto runtime = forge::asio::runtime{};

   const auto expect_invalid = [&](std::string allocation, std::string writes) {
      auto plugin = store_plugin::plugin{};
      auto configured = configured_store("accounts", "/tmp/forge-db-store-plugin-id-allocation");
      auto& store = std::get<forge::config::core::value::object_type>(configured.storage);
      auto& object = std::get<forge::config::core::value::object_type>(store.at("object").storage);
      object["id-allocation"] = forge::config::core::value{std::move(allocation)};
      object["write-policy"] = forge::config::core::value{std::move(writes)};

      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores", forge::config::core::value::array_type{std::move(configured)});
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                                document, "plugins.db.store"})),
                        store_plugin::exceptions::invalid_config);
   };

   expect_invalid("unknown", "single-writer");
   expect_invalid("transactional", "backend");
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_configured_revision_without_object_layer) {
   auto runtime = forge::asio::runtime{};
   auto plugin = store_plugin::plugin{};
   auto store = forge::config::core::value::object_type{};
   store.emplace("name", forge::config::core::value{std::string{"invalid-revision"}});
   store.emplace("driver", forge::config::core::value{std::string{"rocksdb"}});
   store.emplace("path", forge::config::core::value{std::string{"/tmp/forge-db-store-plugin-revision"}});
   auto blob = forge::config::core::value::object_type{};
   blob.emplace("data-family", forge::config::core::value{std::string{"blob.data"}});
   blob.emplace("refs-family", forge::config::core::value{std::string{"blob.refs"}});
   store.emplace("blob", forge::config::core::value{std::move(blob)});
   store.emplace("revision", forge::config::core::value{forge::config::core::value::object_type{}});

   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores",
                forge::config::core::value::array_type{forge::config::core::value{std::move(store)}});

   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         runtime, plugin.configure(forge::config::core::component_view{document, "plugins.db.store"})),
                     store_plugin::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_configured_overlapping_layer_families) {
   auto runtime = forge::asio::runtime{};

   auto expect_invalid = [&](std::string object_family, std::string data_family, std::string refs_family) {
      auto plugin = store_plugin::plugin{};
      auto store = forge::config::core::value::object_type{};
      store.emplace("name", forge::config::core::value{std::string{"files"}});
      store.emplace("driver", forge::config::core::value{std::string{"rocksdb"}});
      store.emplace("path", forge::config::core::value{std::string{"/tmp/forge-db-store-plugin-overlap"}});

      auto object_layer = forge::config::core::value::object_type{};
      object_layer.emplace("family", forge::config::core::value{std::move(object_family)});
      store.emplace("object", forge::config::core::value{std::move(object_layer)});

      auto blob_layer = forge::config::core::value::object_type{};
      blob_layer.emplace("data-family", forge::config::core::value{std::move(data_family)});
      blob_layer.emplace("refs-family", forge::config::core::value{std::move(refs_family)});
      store.emplace("blob", forge::config::core::value{std::move(blob_layer)});

      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores",
                   forge::config::core::value::array_type{forge::config::core::value{std::move(store)}});

      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                                document, "plugins.db.store"})),
                        store_plugin::exceptions::invalid_config);
   };

   expect_invalid("shared", "shared", "blob.refs");
   expect_invalid("shared", "blob.data", "shared");
   expect_invalid("objectdb", "blob.shared", "blob.shared");
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_invalid_configured_extra_families) {
   auto runtime = forge::asio::runtime{};

   const auto expect_invalid = [&](std::vector<std::string> families) {
      auto plugin = store_plugin::plugin{};
      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores", forge::config::core::value::array_type{configured_mdbx_store(
                                                  "files", "/tmp/forge-db-store-plugin-extra-families", true, true,
                                                  "durable-sync", std::move(families))});

      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                                document, "plugins.db.store"})),
                        store_plugin::exceptions::invalid_config);
   };

   expect_invalid({""});
   expect_invalid({"spine-chain-state", "spine-chain-state"});
   expect_invalid({"objectdb"});
   expect_invalid({"blobdb.data"});
   expect_invalid({"blobdb.refs"});
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_invalid_mdbx_configuration) {
   auto runtime = forge::asio::runtime{};
   const auto expect_invalid = [&](forge::config::core::value store) {
      auto plugin = store_plugin::plugin{};
      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores", forge::config::core::value::array_type{std::move(store)});
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                                document, "plugins.db.store"})),
                        store_plugin::exceptions::invalid_config);
   };

   {
      auto store = forge::config::core::value::object_type{};
      store.emplace("name", forge::config::core::value{std::string{"rocks-with-mdbx"}});
      store.emplace("driver", forge::config::core::value{std::string{"rocksdb"}});
      store.emplace("path", forge::config::core::value{std::string{"/tmp/rocks-with-mdbx"}});
      store.emplace("object", forge::config::core::value{forge::config::core::value::object_type{}});
      store.emplace("mdbx", forge::config::core::value{forge::config::core::value::object_type{}});
      expect_invalid(forge::config::core::value{std::move(store)});
   }
   {
      auto store = forge::config::core::value::object_type{};
      store.emplace("name", forge::config::core::value{std::string{"bad-durability"}});
      store.emplace("driver", forge::config::core::value{std::string{"mdbx"}});
      store.emplace("path", forge::config::core::value{std::string{"/tmp/bad-durability"}});
      store.emplace("object", forge::config::core::value{forge::config::core::value::object_type{}});
      auto mdbx = forge::config::core::value::object_type{};
      mdbx.emplace("durability", forge::config::core::value{std::string{"unsafe"}});
      store.emplace("mdbx", forge::config::core::value{std::move(mdbx)});
      expect_invalid(forge::config::core::value{std::move(store)});
   }
   {
      auto store = forge::config::core::value::object_type{};
      store.emplace("name", forge::config::core::value{std::string{"zero-lane"}});
      store.emplace("driver", forge::config::core::value{std::string{"mdbx"}});
      store.emplace("path", forge::config::core::value{std::string{"/tmp/zero-lane"}});
      store.emplace("object", forge::config::core::value{forge::config::core::value::object_type{}});
      auto lane = forge::config::core::value::object_type{};
      lane.emplace("max-pending-operations", forge::config::core::value{std::uint64_t{0U}});
      auto mdbx = forge::config::core::value::object_type{};
      mdbx.emplace("lane", forge::config::core::value{std::move(lane)});
      store.emplace("mdbx", forge::config::core::value{std::move(mdbx)});
      expect_invalid(forge::config::core::value{std::move(store)});
   }
   {
      auto store = forge::config::core::value::object_type{};
      store.emplace("name", forge::config::core::value{std::string{"mdbx-blob-files"}});
      store.emplace("driver", forge::config::core::value{std::string{"mdbx"}});
      store.emplace("path", forge::config::core::value{std::string{"/tmp/mdbx-blob-files"}});
      auto blob_files = forge::config::core::value::object_type{};
      blob_files.emplace("enable-blob-files", forge::config::core::value{true});
      auto blob = forge::config::core::value::object_type{};
      blob.emplace("data-blobs", forge::config::core::value{std::move(blob_files)});
      store.emplace("blob", forge::config::core::value{std::move(blob)});
      store.emplace("mdbx", forge::config::core::value{forge::config::core::value::object_type{}});
      expect_invalid(forge::config::core::value{std::move(store)});
   }
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_configure_after_stop_or_shutdown) {
   auto runtime = forge::asio::runtime{};
   auto document = forge::config::core::document{};

   {
      auto plugin = store_plugin::plugin{};
      plugin.request_stop();

      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                                document, "plugins.db.store"})),
                        store_plugin::exceptions::stopped);
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.startup()),
                        store_plugin::exceptions::startup_failed);
   }

   {
      auto plugin = store_plugin::plugin{};
      auto scheduler = forge::asio::task::scheduler{runtime};
      auto apis = forge::api::core::registry{};
      auto signals = forge::app::signal_bus{};
      auto events = forge::app::event_bus{};
      auto context = forge::app::plugin_context{scheduler, apis, signals, events};
      forge::asio::blocking::run(runtime,
                                 plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
      forge::asio::blocking::run(runtime, plugin.initialize(context));
      forge::asio::blocking::run(runtime, plugin.after_initialize());
      forge::asio::blocking::run(runtime, plugin.startup());
      forge::asio::blocking::run(runtime, plugin.shutdown());

      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                                document, "plugins.db.store"})),
                        store_plugin::exceptions::stopped);
   }
}

BOOST_AUTO_TEST_CASE(store_plugin_custom_driver_store_handle_reads_writes_flushes_and_stops) {
   auto driver = std::make_shared<memory_driver>();
   auto app = make_app({}, driver);
   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());

   auto handle = forge::asio::blocking::run(app->runtime(), api->store("accounts"));
   BOOST_TEST(handle.name() == "accounts");
   BOOST_CHECK_THROW((void)handle.blobs(), store_plugin::exceptions::unavailable_layer);
   BOOST_CHECK_THROW((void)handle.revisions(), store_plugin::exceptions::unavailable_layer);
   handle.objects().register_object<account_object>();

   forge::asio::blocking::run(app->runtime(), handle.objects().insert(make_account(42, "alice", 100)));

   const auto loaded = forge::asio::blocking::run(app->runtime(), handle.objects().get(decltype(account{}.id){42}));
   BOOST_TEST(loaded.name == "alice");
   BOOST_TEST(loaded.balance == 100U);

   auto read = forge::asio::blocking::run(app->runtime(), handle.begin_read());
   BOOST_CHECK(read.active());
   BOOST_TEST(read.name() == "accounts");
   BOOST_TEST(forge::asio::blocking::run(app->runtime(), read.objects().get(decltype(account{}.id){42})).name ==
              "alice");
   BOOST_CHECK_THROW((void)read.blobs(), store_plugin::exceptions::unavailable_layer);

   const auto found_by_name =
       forge::asio::blocking::run(app->runtime(), handle.objects().index<account_object, by_name>().find("alice"));
   BOOST_REQUIRE(found_by_name.has_value());
   BOOST_TEST(found_by_name->id.instance == 42U);

   forge::asio::blocking::run(app->runtime(), handle.objects().modify(decltype(account{}.id){42},
                                                                      [](account& value) { value.balance += 50; }));
   BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.objects().get(decltype(account{}.id){42})).balance ==
              150U);

   forge::asio::blocking::run(app->runtime(), api->flush("accounts", true));
   BOOST_TEST(driver->flush_calls() == 1U);
   forge::asio::blocking::run(app->runtime(), api->flush_all(true));
   BOOST_TEST(driver->flush_calls() == 2U);

   const auto status = forge::asio::blocking::run(app->runtime(), api->status());
   BOOST_REQUIRE_EQUAL(status.stores.size(), 1U);
   BOOST_TEST(status.stores.front().name == "accounts");
   BOOST_TEST(status.stores.front().driver == "custom");
   BOOST_TEST(!status.stores.front().durability.has_value());
   BOOST_TEST(status.stores.front().started);

   forge::asio::blocking::run(app->runtime(), app->shutdown());
   BOOST_TEST(driver->close_calls() == 0U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), handle.objects().find(decltype(account{}.id){42})),
                     store_plugin::exceptions::stopped);
}

BOOST_AUTO_TEST_CASE(store_plugin_object_handle_forwards_ranked_aggregates_and_snapshot_ranks) {
   auto driver = std::make_shared<memory_driver>();
   auto app = make_app({}, driver);
   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app->runtime(), api->store("accounts"));
   handle.objects().register_object<usage_object>();

   auto insert = [&handle, &app](std::uint64_t id, std::uint32_t state, std::uint64_t bytes) {
      auto value = usage_record{};
      value.id = usage_record::id_t{id};
      value.state = state;
      value.bytes = bytes;
      forge::asio::blocking::run(app->runtime(), handle.objects().insert(value));
   };
   insert(1U, 1U, 10U);
   insert(2U, 1U, 20U);
   insert(3U, 2U, 30U);

   auto ranked = handle.objects().index<usage_object, by_usage_state>();
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(app->runtime(), ranked.count()), 3U);
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(app->runtime(), ranked.sum<by_usage_bytes>()), 60U);
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(app->runtime(), ranked.equal_range(1U).count()), 2U);
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(app->runtime(), ranked.lower_bound_rank(2U)), 2U);

   auto read = forge::asio::blocking::run(app->runtime(), handle.begin_read());
   auto snapshot_ranked = read.objects().index<usage_object, by_usage_state>();
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(app->runtime(), snapshot_ranked.nth(2U))->id.instance, 3U);

   auto stream = ranked.lower_bound(0U).stream({.page_size = 1U});
   const auto first = forge::asio::blocking::run(app->runtime(), stream.next());
   BOOST_REQUIRE(first.has_value());
   BOOST_CHECK_EQUAL(first->id.instance, 1U);

   forge::asio::blocking::run(app->runtime(), app->shutdown());

   const auto second = forge::asio::blocking::run(app->runtime(), stream.next());
   BOOST_REQUIRE(second.has_value());
   BOOST_CHECK_EQUAL(second->id.instance, 2U);

   auto unopened = ranked.lower_bound(0U).stream({.page_size = 1U});
   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), unopened.next()), store_plugin::exceptions::stopped);
}

BOOST_AUTO_TEST_CASE(store_plugin_after_initialize_opens_store_for_central_object_registration) {
   auto driver = std::make_shared<memory_driver>();
   auto registered = false;
   auto ready_handle = store_plugin::store_handle{};
   auto builder = forge::app::application_builder{};
   builder.name("db-store-ready-test")
       .runtime(forge::asio::runtime_options{.worker_threads = 1, .thread_name = "db-store-ready-test"})
       .plugin(store_plugin::descriptor())
       .plugin(installer_descriptor(driver))
       .after_initialize([&](const forge::app::application_context& context) -> boost::asio::awaitable<void> {
          auto api = context.api_view().get<store_plugin::api>(store_plugin::api::ref());
          const auto state = co_await api->status();
          BOOST_REQUIRE_EQUAL(state.stores.size(), 1U);
          BOOST_TEST(!state.stores.front().started);

          auto handle = co_await api->store("accounts");
          auto objects = handle.objects();
          objects.register_object<account_object>();
          objects.add_interceptor(std::make_shared<setup_interceptor>());
          objects.add_observer(std::make_shared<setup_observer>());

          BOOST_CHECK_THROW(co_await handle.begin_transaction(), store_plugin::exceptions::stopped);
          BOOST_CHECK_THROW(co_await handle.begin_read(), store_plugin::exceptions::stopped);
          BOOST_CHECK_THROW(co_await objects.begin_read(), store_plugin::exceptions::stopped);
          BOOST_CHECK_THROW(co_await objects.insert(make_account(7, "too-early", 1)),
                            store_plugin::exceptions::stopped);
          BOOST_CHECK_THROW(co_await api->flush("accounts"), store_plugin::exceptions::stopped);

          ready_handle = handle;
          registered = true;
          co_return;
       });

   auto app = std::move(builder).build();
   app->configure(forge::config::core::document{});
   forge::asio::blocking::run(app->runtime(), app->initialize());
   BOOST_TEST(registered);

   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto before_startup = forge::asio::blocking::run(app->runtime(), api->status());
   BOOST_TEST(!before_startup.stores.front().started);

   forge::asio::blocking::run(app->runtime(), app->startup());
   auto after_startup = forge::asio::blocking::run(app->runtime(), api->status());
   BOOST_TEST(after_startup.stores.front().started);

   forge::asio::blocking::run(app->runtime(), ready_handle.objects().insert(make_account(7, "ready", 70)));
   const auto loaded =
       forge::asio::blocking::run(app->runtime(), ready_handle.objects().get(decltype(account{}.id){7}));
   BOOST_TEST(loaded.name == "ready");

   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_blob_only_programmatic_store_rejects_objects_and_roundtrips_blob) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.object.reset();
   options.blob = store_plugin::blob_layer_options{};

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("blobs", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   auto handle = forge::asio::blocking::run(runtime, api->store("blobs"));
   BOOST_CHECK_THROW((void)handle.blobs(), store_plugin::exceptions::stopped);
   forge::asio::blocking::run(runtime, plugin.startup());

   BOOST_CHECK_THROW((void)handle.objects(), store_plugin::exceptions::unavailable_layer);
   BOOST_CHECK_THROW((void)handle.revisions(), store_plugin::exceptions::unavailable_layer);

   auto content = forge::asio::blocking::run(runtime, handle.blobs().put(bytes("blob-only-payload")));
   BOOST_TEST(content.size == 17U);
   BOOST_TEST(forge::asio::blocking::run(runtime, handle.blobs().has(content)));
   BOOST_TEST(forge::asio::blocking::run(runtime, handle.blobs().get(content)).size() == 17U);

   auto read = forge::asio::blocking::run(runtime, handle.begin_read());
   BOOST_CHECK_THROW((void)read.objects(), store_plugin::exceptions::unavailable_layer);
   BOOST_TEST(forge::asio::blocking::run(runtime, read.blobs().get(content)).size() == 17U);

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_shared_transaction_commits_object_metadata_and_blob_payload) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.blob = store_plugin::blob_layer_options{};

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("files", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto handle = forge::asio::blocking::run(runtime, api->store("files"));
   handle.objects().register_object<file_object>();

   auto tx = forge::asio::blocking::run(runtime, handle.begin_transaction());
   auto object_tx = forge::asio::blocking::run(runtime, handle.objects().join(tx));
   auto first_blob_tx = handle.blobs().join(tx);
   auto second_blob_tx = handle.blobs().join(tx);

   auto content = forge::asio::blocking::run(runtime, first_blob_tx.put(bytes("shared payload")));
   forge::asio::blocking::run(runtime, second_blob_tx.retain(content, forge::db::blob::owner_ref{"file:1"}));
   forge::asio::blocking::run(runtime, object_tx.insert(make_file(1, "/a.txt", content)));
   forge::asio::blocking::run(runtime, tx.commit());

   const auto loaded = forge::asio::blocking::run(runtime, handle.objects().get(decltype(file_record{}.id){1}));
   BOOST_TEST(loaded.path == "/a.txt");
   BOOST_TEST(loaded.content == content);
   BOOST_TEST(forge::asio::blocking::run(runtime, handle.blobs().get(loaded.content)).size() == 14U);
   BOOST_TEST(forge::asio::blocking::run(runtime, handle.blobs().ref_count(content)) == 1U);

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_repeated_blob_join_reuses_revision_transaction_participant) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.blob = store_plugin::blob_layer_options{};
   options.revision = store_plugin::revision_layer_options{};

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("files", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto handle = forge::asio::blocking::run(runtime, api->store("files"));
   auto tx = forge::asio::blocking::run(runtime, handle.begin_transaction());
   BOOST_TEST(forge::asio::blocking::run(runtime, handle.revisions().join(tx)).id() == 1U);
   auto first = handle.blobs().join(tx);
   auto moved = std::move(tx);
   auto second = handle.blobs().join(moved);

   const auto content = forge::asio::blocking::run(runtime, first.put(bytes("shared participant")));
   forge::asio::blocking::run(runtime, second.retain(content, forge::db::blob::owner_ref{"file:1"}));
   forge::asio::blocking::run(runtime, moved.rollback());

   BOOST_CHECK(!forge::asio::blocking::run(runtime, handle.blobs().has(content)));
   BOOST_CHECK_THROW((void)handle.blobs().join(moved), store_plugin::exceptions::stopped);

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_unified_snapshot_preserves_object_blob_and_refs_after_collection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.blob = store_plugin::blob_layer_options{};
   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("files", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto handle = forge::asio::blocking::run(runtime, api->store("files"));
   handle.objects().register_object<file_object>();
   const auto owner = forge::db::blob::owner_ref{"file:snapshot"};

   auto seed = forge::asio::blocking::run(runtime, handle.begin_transaction());
   auto seed_objects = forge::asio::blocking::run(runtime, handle.objects().join(seed));
   auto seed_blobs = handle.blobs().join(seed);
   const auto content = forge::asio::blocking::run(runtime, seed_blobs.put(bytes("unified snapshot payload")));
   forge::asio::blocking::run(runtime, seed_blobs.retain(content, owner));
   forge::asio::blocking::run(runtime, seed_objects.insert(make_file(1, "/snapshot.txt", content)));
   forge::asio::blocking::run(runtime, seed.commit());

   const auto snapshot_calls = driver->snapshot_calls();
   auto read = forge::asio::blocking::run(runtime, handle.begin_read());
   BOOST_CHECK(read.active());
   BOOST_TEST(read.name() == "files");
   BOOST_CHECK_EQUAL(driver->snapshot_calls(), snapshot_calls + 1U);

   auto erase = forge::asio::blocking::run(runtime, handle.begin_transaction());
   auto erase_objects = forge::asio::blocking::run(runtime, handle.objects().join(erase));
   auto erase_blobs = handle.blobs().join(erase);
   forge::asio::blocking::run(runtime, erase_objects.erase(file_record::id_t{1}));
   forge::asio::blocking::run(runtime, erase_blobs.release(content, owner));
   const auto collected = forge::asio::blocking::run(runtime, erase_blobs.collect_unreferenced({.limit = 10}));
   BOOST_CHECK_EQUAL(collected.removed, 1U);
   forge::asio::blocking::run(runtime, erase.commit());

   BOOST_CHECK(!forge::asio::blocking::run(runtime, handle.objects().find(file_record::id_t{1})).has_value());
   BOOST_CHECK(!forge::asio::blocking::run(runtime, handle.blobs().has(content)));

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      struct object_result {
         std::optional<file_record> value;
         std::exception_ptr error;
      };
      struct blob_result {
         std::vector<std::byte> value;
         std::exception_ptr error;
      };

      auto object_out = std::make_shared<object_result>();
      auto blob_out = std::make_shared<blob_result>();
      auto completed = std::make_shared<std::atomic_size_t>(0U);
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [view = read.objects(), object_out, completed]() mutable -> boost::asio::awaitable<void> {
             try {
                object_out->value = co_await view.find(file_record::id_t{1});
             } catch (...) {
                object_out->error = std::current_exception();
             }
             completed->fetch_add(1U, std::memory_order_release);
             co_return;
          },
          boost::asio::detached);
      boost::asio::co_spawn(
          executor,
          [view = read.blobs(), content, blob_out, completed]() mutable -> boost::asio::awaitable<void> {
             try {
                blob_out->value = co_await view.get(content);
             } catch (...) {
                blob_out->error = std::current_exception();
             }
             completed->fetch_add(1U, std::memory_order_release);
             co_return;
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      while (completed->load(std::memory_order_acquire) != 2U) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      if (object_out->error) {
         std::rethrow_exception(object_out->error);
      }
      if (blob_out->error) {
         std::rethrow_exception(blob_out->error);
      }
      BOOST_REQUIRE(object_out->value.has_value());
      BOOST_TEST(object_out->value->path == "/snapshot.txt");
      BOOST_TEST(blob_out->value == bytes("unified snapshot payload"));
      co_return;
   }());

   const auto indexed =
       forge::asio::blocking::run(runtime, read.objects().index<file_object, by_path>().find("/snapshot.txt"));
   BOOST_REQUIRE(indexed.has_value());
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(runtime, read.blobs().ref_count(content)), 1U);

   plugin.request_stop();
   auto stopping_read = forge::asio::blocking::run(runtime, handle.begin_read());
   BOOST_CHECK(stopping_read.active());
   forge::asio::blocking::run(runtime, plugin.shutdown());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, handle.begin_read()), store_plugin::exceptions::stopped);

   const auto after_shutdown = forge::asio::blocking::run(runtime, read.objects().get(file_record::id_t{1}));
   BOOST_TEST(after_shutdown.path == "/snapshot.txt");
   BOOST_TEST(forge::asio::blocking::run(runtime, read.blobs().get(content)) == bytes("unified snapshot payload"));
}

BOOST_AUTO_TEST_CASE(store_plugin_revision_layer_is_explicit_and_atomic) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.blob = store_plugin::blob_layer_options{};
   options.revision = store_plugin::revision_layer_options{};

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("files", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());

   auto handle = forge::asio::blocking::run(runtime, api->store("files"));
   handle.objects().register_object<account_object>();
   handle.objects().register_object<file_object>();

   const auto ready = forge::asio::blocking::run(runtime, api->status());
   BOOST_REQUIRE_EQUAL(ready.stores.size(), 1U);
   BOOST_TEST(ready.stores.front().revision);
   BOOST_TEST(!ready.stores.front().started);
   BOOST_CHECK_THROW((void)handle.revisions(), store_plugin::exceptions::stopped);

   forge::asio::blocking::run(runtime, plugin.startup());
   auto revisions = handle.revisions();

   forge::asio::blocking::run(runtime, handle.objects().insert(make_account(10, "outside-revision", 10)));
   auto state = forge::asio::blocking::run(runtime, handle.objects().get(forge::db::revision::state_id));
   BOOST_CHECK(!state.head.has_value());

   auto committed = forge::asio::blocking::run(runtime, handle.begin_transaction());
   const auto committed_revision = forge::asio::blocking::run(runtime, revisions.join(committed));
   BOOST_TEST(committed_revision.id() == 1U);
   auto committed_objects = forge::asio::blocking::run(runtime, handle.objects().join(committed));
   auto committed_blobs = handle.blobs().join(committed);
   const auto content = forge::asio::blocking::run(runtime, committed_blobs.put(bytes("revision payload")));
   forge::asio::blocking::run(runtime, committed_blobs.retain(content, forge::db::blob::owner_ref{"file:1"}));
   forge::asio::blocking::run(runtime, committed_objects.insert(make_file(1, "/revision.txt", content)));
   forge::asio::blocking::run(runtime, committed.commit());

   state = forge::asio::blocking::run(runtime, handle.objects().get(forge::db::revision::state_id));
   BOOST_REQUIRE(state.head.has_value());
   BOOST_TEST(*state.head == 1U);
   const auto first_entry =
       forge::asio::blocking::run(runtime, handle.objects().get(forge::db::revision::entry::id_t{1U}));
   BOOST_TEST(first_entry.delta_count > 0U);

   auto rolled_back = forge::asio::blocking::run(runtime, handle.begin_transaction());
   const auto rolled_back_revision = forge::asio::blocking::run(runtime, revisions.join(rolled_back));
   BOOST_TEST(rolled_back_revision.id() == 2U);
   auto rolled_back_objects = forge::asio::blocking::run(runtime, handle.objects().join(rolled_back));
   auto rolled_back_blobs = handle.blobs().join(rolled_back);
   const auto discarded_content =
       forge::asio::blocking::run(runtime, rolled_back_blobs.put(bytes("discarded payload")));
   forge::asio::blocking::run(runtime, rolled_back_objects.insert(make_file(2, "/discarded.txt", discarded_content)));
   forge::asio::blocking::run(runtime, rolled_back.rollback());

   state = forge::asio::blocking::run(runtime, handle.objects().get(forge::db::revision::state_id));
   BOOST_REQUIRE(state.head.has_value());
   BOOST_TEST(*state.head == 1U);
   BOOST_TEST(!forge::asio::blocking::run(runtime, handle.objects().find(decltype(file_record{}.id){2U})).has_value());
   BOOST_TEST(!forge::asio::blocking::run(runtime, handle.blobs().has(discarded_content)));

   auto savepoint_tx = forge::asio::blocking::run(runtime, handle.begin_transaction());
   const auto savepoint_revision = forge::asio::blocking::run(runtime, revisions.join(savepoint_tx));
   BOOST_TEST(savepoint_revision.id() == 2U);
   auto savepoint_objects = forge::asio::blocking::run(runtime, handle.objects().join(savepoint_tx));
   const auto point = forge::asio::blocking::run(runtime, savepoint_tx.db_transaction().create_savepoint());
   forge::asio::blocking::run(runtime, savepoint_objects.insert(make_account(11, "savepoint-discarded", 11)));
   forge::asio::blocking::run(runtime, savepoint_tx.db_transaction().rollback_to_savepoint(point));
   forge::asio::blocking::run(runtime, savepoint_tx.commit());

   const auto second_entry =
       forge::asio::blocking::run(runtime, handle.objects().get(forge::db::revision::entry::id_t{2U}));
   BOOST_TEST(second_entry.delta_count == 0U);
   BOOST_TEST(!forge::asio::blocking::run(runtime, handle.objects().find(decltype(account{}.id){11U})).has_value());

   forge::asio::blocking::run(runtime, plugin.shutdown());
   BOOST_CHECK_THROW((void)handle.revisions(), store_plugin::exceptions::stopped);
}

BOOST_AUTO_TEST_CASE(store_plugin_revision_handle_reverts_prunes_and_rejects_foreign_transactions) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto first_options = store_plugin::store_options{};
   first_options.object = store_plugin::object_layer_options{
       .family = forge::db::core::family{"objects.first"},
   };
   first_options.revision = store_plugin::revision_layer_options{};
   auto second_options = store_plugin::store_options{};
   second_options.object = store_plugin::object_layer_options{
       .family = forge::db::core::family{"objects.second"},
   };
   second_options.revision = store_plugin::revision_layer_options{};

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("first", driver, first_options));
   forge::asio::blocking::run(runtime, api->add_store("second", driver, second_options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto first = forge::asio::blocking::run(runtime, api->store("first"));
   auto second = forge::asio::blocking::run(runtime, api->store("second"));
   first.objects().register_object<account_object>();
   second.objects().register_object<account_object>();

   auto foreign = forge::asio::blocking::run(runtime, first.begin_transaction());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, second.revisions().join(foreign)),
                     store_plugin::exceptions::invalid_argument);
   forge::asio::blocking::run(runtime, foreign.rollback());

   auto revision_one = forge::asio::blocking::run(runtime, first.begin_transaction());
   BOOST_TEST(forge::asio::blocking::run(runtime, first.revisions().join(revision_one)).id() == 1U);
   auto object_one = forge::asio::blocking::run(runtime, first.objects().join(revision_one));
   forge::asio::blocking::run(runtime, object_one.insert(make_account(1, "one", 10)));
   forge::asio::blocking::run(runtime, revision_one.commit());

   auto revision_two = forge::asio::blocking::run(runtime, first.begin_transaction());
   BOOST_TEST(forge::asio::blocking::run(runtime, first.revisions().join(revision_two)).id() == 2U);
   auto object_two = forge::asio::blocking::run(runtime, first.objects().join(revision_two));
   forge::asio::blocking::run(
       runtime, object_two.modify(decltype(account{}.id){1U}, [](account& value) { value.balance = 20U; }));
   forge::asio::blocking::run(runtime, revision_two.commit());

   auto revert = forge::asio::blocking::run(runtime, first.begin_transaction());
   forge::asio::blocking::run(runtime, first.revisions().revert(revert, 2U));
   forge::asio::blocking::run(runtime, revert.commit());
   BOOST_TEST(forge::asio::blocking::run(runtime, first.objects().get(decltype(account{}.id){1U})).balance == 10U);

   auto revision_three = forge::asio::blocking::run(runtime, first.begin_transaction());
   BOOST_TEST(forge::asio::blocking::run(runtime, first.revisions().join(revision_three)).id() == 3U);
   auto object_three = forge::asio::blocking::run(runtime, first.objects().join(revision_three));
   forge::asio::blocking::run(
       runtime, object_three.modify(decltype(account{}.id){1U}, [](account& value) { value.balance = 30U; }));
   forge::asio::blocking::run(runtime, revision_three.commit());

   auto prune = forge::asio::blocking::run(runtime, first.begin_transaction());
   const auto pruned = forge::asio::blocking::run(
       runtime, first.revisions().prune_through(
                    prune, 1U, forge::db::revision::prune_options{.max_revisions = 1U, .max_deltas = 100U}));
   BOOST_TEST(pruned.revisions_pruned == 1U);
   BOOST_TEST(pruned.complete);
   forge::asio::blocking::run(runtime, prune.commit());

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_authenticated_handle_shares_named_store_transaction) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("first", driver));
   forge::asio::blocking::run(runtime, api->add_store("second", driver));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto first = forge::asio::blocking::run(runtime, api->store("first"));
   auto second = forge::asio::blocking::run(runtime, api->store("second"));
   auto authenticated = first.authenticated({
       .family = forge::db::core::family{"authenticated.first"},
       .domain = "forge.tests.store.first",
   });
   auto foreign_authenticated = second.authenticated({
       .family = forge::db::core::family{"authenticated.second"},
       .domain = "forge.tests.store.second",
   });

   auto foreign = forge::asio::blocking::run(runtime, first.begin_transaction());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, foreign_authenticated.join(foreign, 1U)),
                     store_plugin::exceptions::invalid_argument);
   forge::asio::blocking::run(runtime, foreign.rollback());

   auto active = forge::asio::blocking::run(runtime, first.begin_transaction());
   auto authenticated_transaction = forge::asio::blocking::run(runtime, authenticated.join(active, 0U));
   const auto mutations = std::vector<forge::db::authenticated::mutation>{
       {
           .key = {std::byte{0x01}},
           .value = forge::db::authenticated::bytes{std::byte{0x02}},
       },
   };
   const auto staged = forge::asio::blocking::run(runtime, authenticated_transaction.stage(mutations));
   BOOST_TEST(staged.commitment.version == 0U);
   forge::asio::blocking::run(runtime, active.commit());

   const auto latest = forge::asio::blocking::run(runtime, authenticated.latest());
   BOOST_REQUIRE(latest.has_value());
   BOOST_TEST(latest->version == 0U);
   const auto proof = forge::asio::blocking::run(runtime, authenticated.prove(0U, mutations.front().key));
   const auto verified =
       forge::db::authenticated::verify_point("forge.tests.store.first", proof.anchor, mutations.front().key, proof);
   BOOST_TEST(verified.exists);
   BOOST_REQUIRE(verified.value.has_value());
   BOOST_TEST(*verified.value == *mutations.front().value);
   const auto page = forge::asio::blocking::run(
       runtime, authenticated.scan_range(0U, forge::db::authenticated::range_request{.include_values = true}));
   BOOST_REQUIRE(page.items.size() == 1U);
   BOOST_TEST(page.items.front().key == mutations.front().key);
   BOOST_REQUIRE(page.items.front().value.has_value());
   BOOST_TEST(*page.items.front().value == *mutations.front().value);

   forge::asio::blocking::run(runtime, plugin.shutdown());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, authenticated.latest()), store_plugin::exceptions::stopped);
}

BOOST_AUTO_TEST_CASE(store_plugin_begin_transaction_preserves_object_single_writer_gate) {
   auto driver = std::make_shared<memory_driver>();
   auto app = make_app({}, driver);
   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app->runtime(), api->store("accounts"));
   handle.objects().register_object<account_object>();

   forge::asio::blocking::run(app->runtime(), [&]() -> boost::asio::awaitable<void> {
      auto first = co_await handle.begin_transaction();

      auto second_started = std::make_shared<bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [handle, second_started, second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                auto second = co_await handle.begin_transaction();
                *second_started = true;
                co_await second.rollback();
             } catch (...) {
                *second_error = std::current_exception();
             }
             co_return;
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      BOOST_CHECK(!*second_started);
      BOOST_CHECK(!driver->overlapping_writes());

      co_await first.rollback();

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(*second_started);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());

   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_rejects_layer_joins_from_another_named_store) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto first_options = store_plugin::store_options{};
   first_options.object = store_plugin::object_layer_options{
       .family = forge::db::core::family{"objects.first"},
   };
   first_options.blob = store_plugin::blob_layer_options{
       .data_family = forge::db::core::family{"blobs.first.data"},
       .refs_family = forge::db::core::family{"blobs.first.refs"},
   };
   first_options.revision = store_plugin::revision_layer_options{};
   auto second_options = store_plugin::store_options{};
   second_options.object = store_plugin::object_layer_options{
       .family = forge::db::core::family{"objects.second"},
   };
   second_options.blob = store_plugin::blob_layer_options{
       .data_family = forge::db::core::family{"blobs.second.data"},
       .refs_family = forge::db::core::family{"blobs.second.refs"},
   };

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("first", driver, first_options));
   forge::asio::blocking::run(runtime, api->add_store("second", driver, second_options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto first = forge::asio::blocking::run(runtime, api->store("first"));
   auto second = forge::asio::blocking::run(runtime, api->store("second"));
   auto tx = forge::asio::blocking::run(runtime, first.begin_transaction());
   BOOST_TEST(forge::asio::blocking::run(runtime, first.revisions().join(tx)).id() == 1U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, second.objects().join(tx)),
                     store_plugin::exceptions::invalid_argument);
   BOOST_CHECK_THROW((void)second.blobs().join(tx), store_plugin::exceptions::invalid_argument);

   auto blobs = first.blobs().join(tx);
   const auto content = forge::asio::blocking::run(runtime, blobs.put(bytes("owned payload")));
   forge::asio::blocking::run(runtime, tx.rollback());
   BOOST_TEST(!forge::asio::blocking::run(runtime, first.blobs().has(content)));

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_shared_transaction_rollback_hides_object_and_blob) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.blob = store_plugin::blob_layer_options{};

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("files", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto handle = forge::asio::blocking::run(runtime, api->store("files"));
   handle.objects().register_object<file_object>();

   auto tx = forge::asio::blocking::run(runtime, handle.begin_transaction());
   auto object_tx = forge::asio::blocking::run(runtime, handle.objects().join(tx));
   auto blob_tx = handle.blobs().join(tx);

   auto content = forge::asio::blocking::run(runtime, blob_tx.put(bytes("rollback payload")));
   forge::asio::blocking::run(runtime, object_tx.insert(make_file(2, "/rollback.txt", content)));
   forge::asio::blocking::run(runtime, tx.rollback());

   BOOST_TEST(!forge::asio::blocking::run(runtime, handle.objects().find(decltype(file_record{}.id){2})).has_value());
   BOOST_TEST(!forge::asio::blocking::run(runtime, handle.blobs().has(content)));

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_shared_transaction_object_failure_rolls_back_blob_payload) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.blob = store_plugin::blob_layer_options{};

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("files", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto handle = forge::asio::blocking::run(runtime, api->store("files"));
   handle.objects().register_object<file_object>();
   forge::asio::blocking::run(runtime, handle.objects().insert(make_file(1, "/duplicate.txt", {})));

   auto tx = forge::asio::blocking::run(runtime, handle.begin_transaction());
   auto object_tx = forge::asio::blocking::run(runtime, handle.objects().join(tx));
   auto blob_tx = handle.blobs().join(tx);

   auto content = forge::asio::blocking::run(runtime, blob_tx.put(bytes("orphan candidate")));
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, object_tx.insert(make_file(2, "/duplicate.txt", content))),
                     forge::db::object::exceptions::duplicate_object);
   forge::asio::blocking::run(runtime, tx.rollback());

   BOOST_TEST(!forge::asio::blocking::run(runtime, handle.blobs().has(content)));
   const auto existing = forge::asio::blocking::run(runtime, handle.objects().get(decltype(file_record{}.id){1}));
   BOOST_TEST(existing.path == "/duplicate.txt");
   BOOST_TEST(!driver->overlapping_writes());

   forge::asio::blocking::run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_store_handle_remains_valid_during_dependent_shutdown) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("shutdown", driver));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto handle = forge::asio::blocking::run(runtime, api->store("shutdown"));
   handle.objects().register_object<account_object>();
   forge::asio::blocking::run(runtime, handle.objects().insert(make_account(9, "startup", 1)));

   plugin.request_stop();

   forge::asio::blocking::run(runtime, handle.objects().replace(make_account(9, "shutdown", 77)));
   forge::asio::blocking::run(runtime, api->flush("shutdown", true));
   const auto loaded = forge::asio::blocking::run(
       runtime, handle.objects().get<account_object>(forge::db::ids::object_id{.space = 1, .type = 7, .instance = 9}));
   BOOST_TEST(loaded.balance == 77U);
   BOOST_TEST(driver->flush_calls() == 1U);

   forge::asio::blocking::run(runtime, plugin.shutdown());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, handle.objects().find(decltype(account{}.id){9})),
                     store_plugin::exceptions::stopped);
}

BOOST_AUTO_TEST_CASE(store_plugin_store_handle_concurrent_close_is_snapshot_safe) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};
   auto driver = std::make_shared<memory_driver>();

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("shutdown", driver));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   forge::asio::blocking::run(runtime, plugin.startup());

   auto handle = forge::asio::blocking::run(runtime, api->store("shutdown"));
   handle.objects().register_object<account_object>();
   forge::asio::blocking::run(runtime, handle.objects().insert(make_account(10, "close-race", 1)));

   plugin.request_stop();

   auto done = std::atomic_bool{false};
   auto closer_error = std::exception_ptr{};
   auto closer = std::thread{[&] {
      try {
         forge::asio::blocking::run(runtime, plugin.shutdown());
      } catch (...) {
         closer_error = std::current_exception();
      }
      done.store(true, std::memory_order_release);
   }};

   auto successes = std::size_t{0};
   auto stopped = std::size_t{0};
   do {
      try {
         (void)forge::asio::blocking::run(runtime, handle.objects().find(decltype(account{}.id){10}));
         ++successes;
      } catch (const store_plugin::exceptions::stopped&) {
         ++stopped;
      }
   } while (!done.load(std::memory_order_acquire));

   closer.join();
   if (closer_error) {
      std::rethrow_exception(closer_error);
   }

   BOOST_TEST(successes + stopped > 0U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, handle.objects().find(decltype(account{}.id){10})),
                     store_plugin::exceptions::stopped);
}

BOOST_AUTO_TEST_CASE(store_plugin_unknown_store_fails_typed) {
   auto app = make_app();
   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());

   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), api->store("missing")),
                     store_plugin::exceptions::unknown_store);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), api->flush("missing", true)),
                     store_plugin::exceptions::unknown_store);

   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

#if FORGE_HAS_MDBX
BOOST_AUTO_TEST_CASE(p2p_node_plugin_mdbx_private_peer_state_persists_across_reopen) {
   auto root = root_guard{};
   const auto path = root.root / "p2p-peer-state-mdbx";

   check_p2p_private_peer_state_reopens(
       [&](const forge::tests::p2p::identity_fixture& identity, const std::optional<std::string>& bootstrap) {
          auto document = p2p_production_config(identity);
          set_p2p_bootstrap(document, bootstrap);
          document.set("plugins.db.store.stores",
                       forge::config::core::value::array_type{
                           configured_mdbx_store(std::string{p2p_peer_store_name}, path, false, false),
                       });
          return make_p2p_app({}, std::move(document));
       });
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_mdbx_static_topology_configuration_has_parity_after_reopen) {
   auto root = root_guard{};
   const auto path = root.root / "p2p-static-topology-mdbx";

   check_p2p_static_topology_reopens([&](const forge::tests::p2p::identity_fixture& identity) {
      auto document = p2p_production_config(identity);
      configure_static_topology(document);
      document.set("plugins.db.store.stores",
                   forge::config::core::value::array_type{
                       configured_mdbx_store(std::string{p2p_peer_store_name}, path, false, false),
                   });
      return make_p2p_app({}, std::move(document));
   });
}

BOOST_AUTO_TEST_CASE(store_plugin_configured_mdbx_exposes_extra_family_through_driver) {
   auto root = root_guard{};
   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores",
                forge::config::core::value::array_type{configured_mdbx_store(
                    "files", root.root / "configured-extra-family", true, true, "safe-nosync", {"spine-chain-state"})});

   auto app = make_app(std::move(document));
   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));
   (void)handle.objects();
   (void)handle.blobs();
   (void)handle.revisions();

   auto authenticated = handle.authenticated({
       .family = forge::db::core::family{"spine-chain-state"},
       .domain = "forge.tests.store.configured-family",
   });
   auto transaction = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
   auto participant = forge::asio::blocking::run(app->runtime(), authenticated.join(transaction, 1U));
   const auto mutations = std::vector<forge::db::authenticated::mutation>{
       {
           .key = {std::byte{0x01}},
           .value = forge::db::authenticated::bytes{std::byte{0x02}},
       },
   };
   const auto staged = forge::asio::blocking::run(app->runtime(), participant.stage(mutations));
   BOOST_TEST(staged.commitment.version == 1U);
   forge::asio::blocking::run(app->runtime(), transaction.commit());

   const auto latest = forge::asio::blocking::run(app->runtime(), authenticated.latest());
   BOOST_REQUIRE(latest.has_value());
   BOOST_TEST(latest->version == 1U);
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_programmatic_mdbx_store_shares_all_db_layers) {
   auto root = root_guard{};
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{{.thread_name = "store-mdbx-test"}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto plugin = store_plugin::plugin{};

   auto driver = forge::asio::blocking::run(runtime, forge::db::mdbx::driver::open(
                                                         forge::db::mdbx::config{
                                                             .path = (root.root / "mdbx-store").string(),
                                                             .families = {"objectdb", "blobdb.data", "blobdb.refs"},
                                                         },
                                                         lane.get_executor()));

   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, "plugins.db.store"}));
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));

   auto options = store_plugin::store_options{};
   options.blob = store_plugin::blob_layer_options{};
   options.revision = store_plugin::revision_layer_options{};
   auto api = apis.get<store_plugin::api>(store_plugin::api::ref());
   forge::asio::blocking::run(runtime, api->add_store("files", driver, options));
   forge::asio::blocking::run(runtime, plugin.after_initialize());
   auto handle = forge::asio::blocking::run(runtime, api->store("files"));
   handle.objects().register_object<file_object>();
   forge::asio::blocking::run(runtime, plugin.startup());

   auto transaction = forge::asio::blocking::run(runtime, handle.begin_transaction());
   auto objects = forge::asio::blocking::run(runtime, handle.objects().join(transaction));
   auto blobs = handle.blobs().join(transaction);
   const auto revision = forge::asio::blocking::run(runtime, handle.revisions().join(transaction));
   const auto content = forge::asio::blocking::run(runtime, blobs.put(bytes("mdbx plugin payload")));
   forge::asio::blocking::run(runtime, blobs.retain(content, forge::db::blob::owner_ref{"file:1"}));
   forge::asio::blocking::run(runtime, objects.insert(make_file(1, "/mdbx.bin", content)));
   forge::asio::blocking::run(runtime, transaction.commit());
   BOOST_TEST(revision.id() == 1U);

   auto read = forge::asio::blocking::run(runtime, handle.begin_read());
   const auto loaded = forge::asio::blocking::run(runtime, read.objects().get(file_record::id_t{1}));
   BOOST_TEST(loaded.path == "/mdbx.bin");
   BOOST_TEST(forge::asio::blocking::run(runtime, read.blobs().get(loaded.content)) == bytes("mdbx plugin payload"));

   read = {};
   forge::asio::blocking::run(runtime, plugin.shutdown());
   forge::asio::blocking::run(runtime, driver->async_close());
   driver.reset();
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_configured_mdbx_store_persists_all_layers) {
   auto root = root_guard{};
   const auto path = root.root / "configured-mdbx";
   auto content = forge::db::blob::ref<>{};

   {
      auto app = make_app(document_for_mdbx(path, true, true, "safe-nosync"));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      const auto status = forge::asio::blocking::run(app->runtime(), api->status());
      BOOST_REQUIRE_EQUAL(status.stores.size(), 1U);
      BOOST_REQUIRE(status.stores.front().durability.has_value());
      BOOST_TEST(*status.stores.front().durability == "safe-nosync");
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));
      handle.objects().register_object<file_object>();
      handle.objects().register_object<usage_object>();

      auto transaction = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
      const auto revision = forge::asio::blocking::run(app->runtime(), handle.revisions().join(transaction));
      auto objects = forge::asio::blocking::run(app->runtime(), handle.objects().join(transaction));
      auto blobs = handle.blobs().join(transaction);
      content = forge::asio::blocking::run(app->runtime(), blobs.put(bytes("configured mdbx payload")));
      forge::asio::blocking::run(app->runtime(), blobs.retain(content, forge::db::blob::owner_ref{"file:7"}));
      forge::asio::blocking::run(app->runtime(), objects.insert(make_file(7, "/configured.bin", content)));

      auto usage = usage_record{};
      usage.id = usage_record::id_t{7};
      usage.state = 2U;
      usage.bytes = content.size;
      forge::asio::blocking::run(app->runtime(), objects.insert(usage));
      forge::asio::blocking::run(app->runtime(), transaction.commit());
      BOOST_TEST(revision.id() == 1U);

      auto ranked = handle.objects().index<usage_object, by_usage_state>();
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), ranked.count()) == 1U);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), ranked.sum<by_usage_bytes>()) == content.size);
      forge::asio::blocking::run(app->runtime(), api->flush_all(true));
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto app = make_app(document_for_mdbx(path, true));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));
      handle.objects().register_object<file_object>();
      handle.objects().register_object<usage_object>();

      auto read = forge::asio::blocking::run(app->runtime(), handle.begin_read());
      const auto file = forge::asio::blocking::run(app->runtime(), read.objects().get(file_record::id_t{7}));
      BOOST_TEST(file.path == "/configured.bin");
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), read.blobs().get(file.content)) ==
                 bytes("configured mdbx payload"));
      const auto state = forge::asio::blocking::run(app->runtime(), read.objects().get(forge::db::revision::state_id));
      BOOST_REQUIRE(state.head.has_value());
      BOOST_TEST(*state.head == 1U);

      auto ranked = read.objects().index<usage_object, by_usage_state>();
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), ranked.count()) == 1U);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), ranked.sum<by_usage_bytes>()) == content.size);

      read = {};
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}

BOOST_AUTO_TEST_CASE(store_plugin_configured_mdbx_reports_default_durability) {
   auto root = root_guard{};
   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores", forge::config::core::value::array_type{configured_mdbx_store(
                                               "files", root.root / "default-mdbx", false, false, std::nullopt)});

   auto app = make_app(std::move(document));
   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   const auto status = forge::asio::blocking::run(app->runtime(), api->status());
   BOOST_REQUIRE_EQUAL(status.stores.size(), 1U);
   BOOST_REQUIRE(status.stores.front().durability.has_value());
   BOOST_TEST(*status.stores.front().durability == "durable-sync");
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

BOOST_AUTO_TEST_CASE(store_plugin_configured_mdbx_snapshot_defers_physical_close) {
   auto root = root_guard{};
   const auto path = root.root / "deferred-close";
   auto read = store_plugin::snapshot{};
   auto content = forge::db::blob::ref<>{};

   {
      auto app = make_app(document_for_mdbx(path, true, false));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));
      handle.objects().register_object<file_object>();

      auto transaction = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
      auto objects = forge::asio::blocking::run(app->runtime(), handle.objects().join(transaction));
      auto blobs = handle.blobs().join(transaction);
      content = forge::asio::blocking::run(app->runtime(), blobs.put(bytes("snapshot survives shutdown")));
      forge::asio::blocking::run(app->runtime(), objects.insert(make_file(1, "/snapshot.bin", content)));
      forge::asio::blocking::run(app->runtime(), transaction.commit());

      read = forge::asio::blocking::run(app->runtime(), handle.begin_read());
      forge::asio::blocking::run(app->runtime(), app->shutdown());
      BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), handle.begin_read()),
                        store_plugin::exceptions::stopped);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), read.objects().get(file_record::id_t{1})).path ==
                 "/snapshot.bin");
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), read.blobs().get(content)) ==
                 bytes("snapshot survives shutdown"));
   }

   auto runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         runtime, forge::db::mdbx::driver::open(
                                      forge::db::mdbx::config{
                                          .path = path.string(),
                                          .families = {"objectdb", "blobdb.data", "blobdb.refs"},
                                      },
                                      forge::asio::affine::lane::options{.thread_name = "mdbx-competing-open"})),
                     forge::db::mdbx::exceptions::environment_busy);

   read = {};

   auto reopened = make_app(document_for_mdbx(path, true, false));
   auto api = reopened->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(reopened->runtime(), api->store("files"));
   handle.objects().register_object<file_object>();
   BOOST_TEST(forge::asio::blocking::run(reopened->runtime(), handle.objects().get(file_record::id_t{1})).path ==
              "/snapshot.bin");
   forge::asio::blocking::run(reopened->runtime(), reopened->shutdown());
}
#endif

#if FORGE_HAS_ROCKSDB
BOOST_AUTO_TEST_CASE(p2p_node_plugin_rocksdb_private_peer_state_persists_across_reopen) {
   auto root = root_guard{};
   const auto path = root.root / "p2p-peer-state-rocksdb";

   check_p2p_private_peer_state_reopens(
       [&](const forge::tests::p2p::identity_fixture& identity, const std::optional<std::string>& bootstrap) {
          auto document = p2p_production_config(identity);
          set_p2p_bootstrap(document, bootstrap);
          document.set("plugins.db.store.stores", forge::config::core::value::array_type{
                                                      configured_store(std::string{p2p_peer_store_name}, path),
                                                  });
          return make_p2p_app({}, std::move(document));
       });
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_rocksdb_static_topology_configuration_has_parity_after_reopen) {
   auto root = root_guard{};
   const auto path = root.root / "p2p-static-topology-rocksdb";

   check_p2p_static_topology_reopens([&](const forge::tests::p2p::identity_fixture& identity) {
      auto document = p2p_production_config(identity);
      configure_static_topology(document);
      document.set("plugins.db.store.stores", forge::config::core::value::array_type{
                                                  configured_store(std::string{p2p_peer_store_name}, path),
                                              });
      return make_p2p_app({}, std::move(document));
   });
}

BOOST_AUTO_TEST_CASE(store_plugin_configured_rocksdb_store_persists_across_reopen) {
   auto root = root_guard{};
   const auto db_path = root.root / "objectdb";

   {
      auto app = make_app(document_for_rocksdb(db_path));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("accounts"));
      handle.objects().register_object<account_object>();

      forge::asio::blocking::run(app->runtime(), handle.objects().insert(make_account(7, "persisted", 900)));
      forge::asio::blocking::run(app->runtime(), api->flush_all(true));
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto app = make_app(document_for_rocksdb(db_path));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("accounts"));
      handle.objects().register_object<account_object>();

      const auto loaded = forge::asio::blocking::run(app->runtime(), handle.objects().get(decltype(account{}.id){7}));
      BOOST_TEST(loaded.name == "persisted");
      BOOST_TEST(loaded.balance == 900U);

      auto snapshot = forge::asio::blocking::run(app->runtime(), handle.objects().begin_read());
      const auto from_snapshot = forge::asio::blocking::run(app->runtime(), snapshot.get(decltype(account{}.id){7}));
      BOOST_TEST(from_snapshot.name == "persisted");

      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}

BOOST_AUTO_TEST_CASE(store_plugin_configured_rocksdb_store_persists_object_and_blob_across_reopen) {
   auto root = root_guard{};
   const auto db_path = root.root / "storedb";

   {
      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores",
                   forge::config::core::value::array_type{configured_object_blob_store("files", db_path)});
      auto app = make_app(std::move(document));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));
      handle.objects().register_object<file_object>();

      auto tx = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
      auto object_tx = forge::asio::blocking::run(app->runtime(), handle.objects().join(tx));
      auto blob_tx = handle.blobs().join(tx);

      const auto content = forge::asio::blocking::run(app->runtime(), blob_tx.put(bytes("configured rocksdb blob")));
      forge::asio::blocking::run(app->runtime(), object_tx.insert(make_file(11, "/rocks.txt", content)));
      forge::asio::blocking::run(app->runtime(), tx.commit());
      forge::asio::blocking::run(app->runtime(), api->flush_all(true));
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores",
                   forge::config::core::value::array_type{configured_object_blob_store("files", db_path)});
      auto app = make_app(std::move(document));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));
      handle.objects().register_object<file_object>();

      const auto loaded =
          forge::asio::blocking::run(app->runtime(), handle.objects().get(decltype(file_record{}.id){11}));
      BOOST_TEST(loaded.path == "/rocks.txt");
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().get(loaded.content)).size() == 23U);

      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}

BOOST_AUTO_TEST_CASE(store_plugin_rocksdb_unified_snapshot_survives_blob_collection) {
   auto root = root_guard{};
   const auto db_path = root.root / "snapshot-store";
   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores",
                forge::config::core::value::array_type{configured_object_blob_store("files", db_path)});
   auto app = make_app(std::move(document));
   auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
   auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));
   handle.objects().register_object<file_object>();
   const auto owner = forge::db::blob::owner_ref{"file:rocksdb-snapshot"};

   auto seed = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
   auto seed_objects = forge::asio::blocking::run(app->runtime(), handle.objects().join(seed));
   auto seed_blobs = handle.blobs().join(seed);
   const auto content =
       forge::asio::blocking::run(app->runtime(), seed_blobs.put(bytes("rocksdb unified snapshot payload")));
   forge::asio::blocking::run(app->runtime(), seed_blobs.retain(content, owner));
   forge::asio::blocking::run(app->runtime(), seed_objects.insert(make_file(21, "/snapshot.bin", content)));
   forge::asio::blocking::run(app->runtime(), seed.commit());

   auto read = forge::asio::blocking::run(app->runtime(), handle.begin_read());

   auto erase = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
   auto erase_objects = forge::asio::blocking::run(app->runtime(), handle.objects().join(erase));
   auto erase_blobs = handle.blobs().join(erase);
   forge::asio::blocking::run(app->runtime(), erase_objects.erase(file_record::id_t{21}));
   forge::asio::blocking::run(app->runtime(), erase_blobs.release(content, owner));
   BOOST_TEST(forge::asio::blocking::run(app->runtime(), erase_blobs.collect_unreferenced({.limit = 10})).removed ==
              1U);
   forge::asio::blocking::run(app->runtime(), erase.commit());

   BOOST_CHECK(!forge::asio::blocking::run(app->runtime(), handle.objects().find(file_record::id_t{21})).has_value());
   BOOST_CHECK(!forge::asio::blocking::run(app->runtime(), handle.blobs().has(content)));

   const auto old_file = forge::asio::blocking::run(app->runtime(), read.objects().get(file_record::id_t{21}));
   BOOST_TEST(old_file.path == "/snapshot.bin");
   const auto indexed =
       forge::asio::blocking::run(app->runtime(), read.objects().index<file_object, by_path>().find("/snapshot.bin"));
   BOOST_REQUIRE(indexed.has_value());
   BOOST_TEST(forge::asio::blocking::run(app->runtime(), read.blobs().ref_count(content)) == 1U);

   forge::asio::blocking::run(app->runtime(), app->shutdown());
   BOOST_TEST(forge::asio::blocking::run(app->runtime(), read.blobs().get(content)) ==
              bytes("rocksdb unified snapshot payload"));
}

BOOST_AUTO_TEST_CASE(store_plugin_configured_rocksdb_revision_preserves_blob_retention_across_reopen) {
   auto root = root_guard{};
   const auto db_path = root.root / "revision-store";
   auto content = forge::db::blob::ref<>{};

   {
      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores",
                   forge::config::core::value::array_type{configured_object_blob_store("files", db_path, true)});
      auto app = make_app(std::move(document));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));

      auto baseline = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
      auto baseline_blobs = handle.blobs().join(baseline);
      content = forge::asio::blocking::run(app->runtime(), baseline_blobs.put(bytes("retained revision payload")));
      forge::asio::blocking::run(app->runtime(),
                                 baseline_blobs.retain(content, forge::db::blob::owner_ref{"file:retained"}));
      forge::asio::blocking::run(app->runtime(), baseline.commit());

      auto revision = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.revisions().join(revision)).id() == 1U);
      auto revision_blobs = handle.blobs().join(revision);
      forge::asio::blocking::run(app->runtime(),
                                 revision_blobs.release(content, forge::db::blob::owner_ref{"file:retained"}));
      forge::asio::blocking::run(app->runtime(), revision.commit());

      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().ref_count(content)) == 0U);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().collect_unreferenced()).removed == 0U);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().has(content)));

      forge::asio::blocking::run(app->runtime(), api->flush_all(true));
      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }

   {
      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores",
                   forge::config::core::value::array_type{configured_object_blob_store("files", db_path, true)});
      auto app = make_app(std::move(document));
      auto api = app->apis().get<store_plugin::api>(store_plugin::api::ref());
      auto handle = forge::asio::blocking::run(app->runtime(), api->store("files"));

      const auto state =
          forge::asio::blocking::run(app->runtime(), handle.objects().get(forge::db::revision::state_id));
      BOOST_REQUIRE(state.head.has_value());
      BOOST_TEST(*state.head == 1U);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().collect_unreferenced()).removed == 0U);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().has(content)));

      auto revert = forge::asio::blocking::run(app->runtime(), handle.begin_transaction());
      forge::asio::blocking::run(app->runtime(), handle.revisions().revert(revert, 1U));
      forge::asio::blocking::run(app->runtime(), revert.commit());

      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().ref_count(content)) == 1U);
      BOOST_TEST(forge::asio::blocking::run(app->runtime(), handle.blobs().get(content)).size() == 25U);

      forge::asio::blocking::run(app->runtime(), app->shutdown());
   }
}
#endif

BOOST_AUTO_TEST_SUITE_END()
