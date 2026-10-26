#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <type_traits>
#include <utility>

import forge.plugins.db.store.plugin;
import forge.plugins.db.store.api;
import forge.db.authenticated.store;
import forge.db.authenticated.transaction;
import forge.db.authenticated.types;
import forge.db.blob.snapshot;
import forge.db.core.record;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.store;
import forge.db.revision.types;

struct usage_record : forge::db::object::object<usage_record, 1, 31> {
   std::uint64_t bytes = 0;
};

BOOST_DESCRIBE_STRUCT(usage_record, (forge::db::object::object<usage_record, 1, 31>), (bytes))

struct by_id;
struct by_bytes;

using usage_index = forge::db::object::object_index<
    usage_record, forge::db::object::indexed_by<forge::db::object::ranked_primary_unique<
                      by_id, forge::db::object::ranked_schema<1>,
                      forge::db::object::sum<by_bytes, forge::db::object::member<&usage_record::bytes>>>>>;

boost::asio::awaitable<void> use_revision_layer(forge::plugins::db::store::store_handle store) {
   auto active = co_await store.begin_transaction();
   const auto revision = co_await store.revisions().join(active);
   if (revision.id() == 0U) {
      co_await active.rollback();
      co_return;
   }
   co_await active.rollback();
}

boost::asio::awaitable<void> use_shared_read(forge::plugins::db::store::store_handle store) {
   auto read = co_await store.begin_read();
   if (read.active()) {
      static_cast<void>(read.objects());
      static_cast<void>(read.blobs());
   }
}

boost::asio::awaitable<void> use_checkpoint(forge::plugins::db::store::store_handle store,
                                            const std::filesystem::path& destination) {
   co_await store.create_checkpoint(destination);
}

boost::asio::awaitable<bool> use_strict_activation(forge::plugins::db::store::store_handle store,
                                                   std::filesystem::path destination) {
   auto closed = co_await store.close_for_activation();
   co_await closed.activate(std::move(destination));
   const auto status = closed.state();
   co_return status.published&& status.phase == forge::plugins::db::store::activation_phase::durable;
}

boost::asio::awaitable<void> use_authenticated_layer(forge::plugins::db::store::store_handle store) {
   auto authenticated = store.authenticated({
       .family = forge::db::core::family{"state.authenticated"},
       .domain = "package.consumer",
   });
   auto active = co_await store.begin_transaction();
   auto state = co_await authenticated.join(active, 1U);
   const auto mutations = std::vector<forge::db::authenticated::mutation>{};
   co_await state.stage(mutations);
   co_await active.rollback();
}

boost::asio::awaitable<void> use_allocation_restore(forge::plugins::db::store::store_handle store,
                                                    std::vector<forge::db::object::store::allocation> values) {
   auto active = co_await store.begin_transaction();
   co_await store.objects().restore_allocations(active, std::move(values));
   co_await active.rollback();
}

boost::asio::awaitable<forge::db::authenticated::checkpoint_summary>
use_authenticated_checkpoint(forge::plugins::db::store::store_handle source,
                             forge::plugins::db::store::store_handle target,
                             forge::db::authenticated::store::config settings,
                             forge::db::authenticated::root expected,
                             forge::db::authenticated::checkpoint_sink sink,
                             forge::db::authenticated::checkpoint_source frames,
                             forge::db::authenticated::checkpoint_verify verify,
                             forge::db::authenticated::checkpoint_limits limits) {
   auto read = co_await source.begin_read();
   const auto exported = co_await source.authenticated(settings).export_checkpoint(
       read, expected, std::move(sink), limits);
   auto active = co_await target.begin_transaction();
   auto restoring = target.authenticated(std::move(settings)).join_restore(active, expected, limits);
   const auto restored = co_await restoring.restore_checkpoint(std::move(frames), std::move(verify));
   co_await active.rollback();
   co_return restored == exported ? restored : forge::db::authenticated::checkpoint_summary{};
}

int package_api_contract_checks() {
   using store_plugin = forge::plugins::db::store::plugin;
   static_assert(std::is_constructible_v<store_plugin, store_plugin::options>);
   auto opening = store_plugin::options{};
   opening.prerequisites.push_back({
       .store = "local",
       .before = {"protected"},
       .check = [](forge::db::object::store& actual) -> boost::asio::awaitable<void> {
          actual.register_object<usage_index>();
          auto read = co_await actual.begin_read();
          static_cast<void>(read);
       },
   });
   const auto configured_descriptor = store_plugin::descriptor(std::move(opening));
   auto configured_plugin = configured_descriptor.factory();
   if (!configured_plugin) { return 1; }
   static_assert(!std::is_default_constructible_v<forge::plugins::db::store::closed_store>);
   static_assert(!std::is_copy_constructible_v<forge::plugins::db::store::closed_store>);
   static_assert(std::is_nothrow_move_constructible_v<forge::plugins::db::store::closed_store>);
   using object_handle = forge::plugins::db::store::object_handle;
   using revision_handle = forge::plugins::db::store::revision_handle;
   using store_transaction = forge::plugins::db::store::transaction;
   using authenticated_handle = forge::plugins::db::store::authenticated_handle;
   using store_snapshot = forge::plugins::db::store::snapshot;
   static_assert(std::same_as<
       decltype(std::declval<const authenticated_handle&>().export_checkpoint(
           std::declval<store_snapshot>(), forge::db::authenticated::root{},
           forge::db::authenticated::checkpoint_sink{}, forge::db::authenticated::checkpoint_limits{})),
       boost::asio::awaitable<forge::db::authenticated::checkpoint_summary>>);
   static_assert(std::same_as<
       decltype(std::declval<const authenticated_handle&>().join_restore(
           std::declval<store_transaction&>(), forge::db::authenticated::root{},
           forge::db::authenticated::checkpoint_limits{})), forge::db::authenticated::transaction>);
   static_assert(
       std::same_as<decltype(std::declval<const object_handle&>().restore_allocations(
                        std::declval<store_transaction&>(), std::vector<forge::db::object::store::allocation>{})),
                    boost::asio::awaitable<void>>);
   static_assert(
       std::same_as<decltype(std::declval<const object_handle&>().require_pristine(std::declval<store_transaction&>())),
                    boost::asio::awaitable<void>>);
   static_assert(std::same_as<decltype(std::declval<const revision_handle&>().require_pristine(
                                  std::declval<store_transaction&>())),
                              boost::asio::awaitable<void>>);
   static_assert(requires(const object_handle& objects, const usage_record& value) {
      objects.index<usage_index, by_id>().count();
      objects.index<usage_index, by_id>().sum<by_bytes>();
      objects.index<usage_index, by_id>().nth(0U);
      objects.index<usage_index, by_id>().rank(value);
   });

   const auto plugin = forge::plugins::db::store::plugin{};
   const auto descriptor = forge::plugins::db::store::plugin::descriptor();
   const auto api = forge::plugins::db::store::api::describe();
   const auto options = forge::db::revision::prune_options{
       .max_revisions = 1U,
       .max_deltas = 1U,
   };
   const auto mdbx = forge::plugins::db::store::mdbx_driver_config{
       .durability = "safe-nosync",
       .max_readers = 64U,
       .map = {.upper_size = 64ULL * 1024ULL * 1024ULL},
       .lane = {.max_pending_operations = 32U, .max_waiting_submissions = 32U},
   };
   return plugin.version() == "2.0.0" && descriptor.id.value == "forge.plugins.db.store" && api.version.major == 2U &&
                  api.version.revision == 0U && options.max_revisions == 1U && mdbx.max_readers == 64U
              ? 0
              : 1;
}
