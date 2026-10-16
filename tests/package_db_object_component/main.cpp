#include <boost/asio/awaitable.hpp>
#include <concepts>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <boost/describe.hpp>
#include <forge/db/object/macros.hpp>

import forge.db.ids.object_id;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.header;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.snapshot;
import forge.db.object.store;
import forge.db.object.transaction;

struct external_key {
   std::uint32_t value = 0;
};

BOOST_DESCRIBE_STRUCT(external_key, (), (value))

template <> struct forge::db::object::sort_key<external_key> {
   forge::db::object::sort_key_bytes operator()(const external_key& key) const {
      return {
          static_cast<std::byte>((key.value >> 24U) & 0xffU),
          static_cast<std::byte>((key.value >> 16U) & 0xffU),
          static_cast<std::byte>((key.value >> 8U) & 0xffU),
          static_cast<std::byte>(key.value & 0xffU),
      };
   }
};

struct account : forge::db::object::object<account, 1, 7> {
   std::string name;
   external_key key;
   std::uint64_t usage = 0;
};

BOOST_DESCRIBE_STRUCT(account, (forge::db::object::object<account, 1, 7>), (name, key, usage))

struct by_id;
struct by_name;
struct by_key;
struct by_usage;

using account_object = forge::db::object::object_index<
    account,
    forge::db::object::indexed_by<forge::db::object::ranked_primary_unique<
                                     by_id,
                                     forge::db::object::ranked_schema<1>,
                                     forge::db::object::sum<
                                        by_usage, forge::db::object::member<&account::usage>>>,
                                  forge::db::object::ordered_unique<by_name, forge::db::object::member<&account::name>>,
                                  forge::db::object::ordered_unique<by_key, forge::db::object::member<&account::key>>>>;

FORGE_DB_OBJECT(account_object)

template <typename View>
concept ranked_findable_by_name = requires(View& index, const std::string& name) {
   index.find_rank(name);
};

boost::asio::awaitable<void> require_initial_objects(forge::db::object::store& objects,
                                                     forge::db::object::transaction& active) {
   co_await objects.require_pristine(active);
}

boost::asio::awaitable<void> restore_initial_allocations(forge::db::object::store& objects,
                                                         forge::db::object::transaction& active) {
   auto values = std::vector<forge::db::object::store::allocation>{
       {.type = forge::db::object::object_id_of<account_object>::value, .next = 8U}};
   co_await objects.restore_allocations(active, std::move(values));
}

int main() {
   static_assert(requires(forge::db::object::store& objects, forge::db::object::transaction& active,
                          std::vector<forge::db::object::store::allocation> values) {
      { objects.restore_allocations(active, std::move(values)) } -> std::same_as<boost::asio::awaitable<void>>;
   });
   static_assert(requires(forge::db::object::store& objects, forge::db::object::transaction& active) {
      { objects.require_pristine(active) } -> std::same_as<boost::asio::awaitable<void>>;
   });
   static_assert(requires(std::shared_ptr<forge::db::core::driver> driver) {
      {
         forge::db::object::store::open_existing(driver, forge::db::object::store::config{},
                                                 forge::db::object::store::options{})
      } -> std::same_as<boost::asio::awaitable<forge::db::object::store>>;
   });
   static_assert(forge::db::object::object_model<account_object>);
   static_assert(forge::db::object::system_object_value<forge::db::object::header>);
   static_assert(forge::db::object::system_object_model<forge::db::object::header_index>);
   static_assert(!forge::db::object::application_object_value<forge::db::object::header>);
   static_assert(forge::db::object::sortable_key<external_key>);
   static_assert(std::same_as<forge::db::object::index_for_id_t<account::id_t>, account_object>);
   static_assert(std::same_as<forge::db::object::index_for_id_t<forge::db::object::header::id_t>,
                              forge::db::object::header_index>);
   static_assert(requires(forge::db::object::store& store,
                          const forge::db::core::snapshot& view) {
      { store.join(view) } -> std::same_as<forge::db::object::snapshot>;
   });
   using ranked_view = forge::db::object::index_view<account_object, by_id>;
   using ordered_view = forge::db::object::index_view<account_object, by_name>;
   static_assert(requires(ranked_view& index, const account& value, account::id_t id) {
      index.count();
      index.sum<by_usage>();
      index.nth(0U);
      index.rank(value);
      index.find_rank(id);
      index.lower_bound_rank(id);
      index.upper_bound_rank(id);
      index.equal_range_rank(id);
   });
   static_assert(!ranked_findable_by_name<ordered_view>);
   constexpr auto type = forge::db::object::object_id_of<account_object>::value;
   const auto key = forge::db::core::record_key{std::vector<std::byte>{std::byte{0x01}}};
   return type.space == 1 && type.type == 7 && !key.empty() ? 0 : 1;
}
