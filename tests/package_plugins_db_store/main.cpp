#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>

#include <concepts>
#include <coroutine>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

import forge.plugins.db.store.plugin;
import forge.db.authenticated.store;
import forge.db.authenticated.types;
import forge.db.blob.snapshot;
import forge.db.core.record;
import forge.db.object.index;
import forge.db.object.object;
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

int main() {
   using object_handle = forge::plugins::db::store::object_handle;
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
