module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include "ranked_index.hxx"

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <typeindex>
#include <typeinfo>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.db.object.transaction;

import forge.db.ids.object_id;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.cursor;
import forge.db.object.exceptions;
import forge.db.object.hooks;
import forge.db.object.index;
import forge.db.object.object;
import forge.raw.raw;

export namespace forge::db::object {

namespace detail {
class transaction_access;
}

class transaction {
 public:
   using ensure_registered_fn = std::function<void(forge::db::ids::object_id, std::type_index)>;
   using allocate_id_fn = std::function<boost::asio::awaitable<forge::db::ids::object_id>(
       forge::db::ids::object_id, forge::db::core::transaction&)>;
   using allocation_seal_map = std::map<forge::db::ids::object_id, std::uint64_t>;
   using seal_allocations_fn = std::function<boost::asio::awaitable<void>(allocation_seal_map)>;
   using release_fn = std::function<void()>;

   transaction() = default;
   transaction(forge::db::core::transaction&& active, forge::db::core::family family, ensure_registered_fn ensure,
               allocate_id_fn allocate, std::vector<std::shared_ptr<interceptor>> interceptors,
               std::vector<std::shared_ptr<observer>> observers, release_fn release);
   transaction(forge::db::core::transaction&& active, forge::db::core::family family, ensure_registered_fn ensure,
               allocate_id_fn allocate, seal_allocations_fn seal,
               std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
               release_fn release, boost::asio::any_io_executor cleanup_executor);
   transaction(forge::db::core::transaction& active, forge::db::core::family family, ensure_registered_fn ensure,
               allocate_id_fn allocate, seal_allocations_fn seal,
               std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
               release_fn release = {});
   transaction(forge::db::core::transaction& active, forge::db::core::family family, ensure_registered_fn ensure,
               allocate_id_fn allocate, std::vector<std::shared_ptr<interceptor>> interceptors,
               std::vector<std::shared_ptr<observer>> observers);

   transaction(forge::db::core::transaction&& active, forge::db::core::family family, ensure_registered_fn ensure,
               std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
               release_fn release);
   transaction(forge::db::core::transaction&& active, forge::db::core::family family, ensure_registered_fn ensure,
               std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
               release_fn release, boost::asio::any_io_executor cleanup_executor);
   transaction(forge::db::core::transaction& active, forge::db::core::family family, ensure_registered_fn ensure,
               std::vector<std::shared_ptr<interceptor>> interceptors,
               std::vector<std::shared_ptr<observer>> observers);

   transaction(const transaction&) = delete;
   transaction& operator=(const transaction&) = delete;
   transaction(transaction&&) noexcept = default;
   transaction& operator=(transaction&&) noexcept = default;

   template <forge::db::ids::typed_id_like Id>
   boost::asio::awaitable<typename index_for_id_t<Id>::value_type> get(Id id);

   template <forge::db::ids::typed_id_like Id>
   boost::asio::awaitable<std::optional<typename index_for_id_t<Id>::value_type>> find(Id id);

   template <object_model Object> boost::asio::awaitable<typename Object::value_type> get(forge::db::ids::object_id id);

   template <object_model Object>
   boost::asio::awaitable<std::optional<typename Object::value_type>> find(forge::db::ids::object_id id);

   template <application_object_value Value> boost::asio::awaitable<void> insert(Value value);

   template <application_object_value Value, typename Fn>
      requires std::default_initializable<Value> && std::invocable<Fn&, Value&>
   boost::asio::awaitable<Value> create(Fn&& fn);

   template <application_object_value Value> boost::asio::awaitable<void> replace(Value value);

   template <forge::db::ids::typed_id_like Id, typename Fn>
      requires application_object_model<index_for_id_t<Id>>
   boost::asio::awaitable<void> modify(Id id, Fn&& fn);

   template <forge::db::ids::typed_id_like Id>
      requires application_object_model<index_for_id_t<Id>>
   boost::asio::awaitable<void> erase(Id id);

   template <application_object_model Object> boost::asio::awaitable<void> erase(forge::db::ids::object_id id);

   template <object_model Object, typename Tag> [[nodiscard]] index_view<Object, Tag> index() const;

   [[nodiscard]] forge::db::core::transaction& db_transaction() const;
   [[nodiscard]] change_set projected_changes() const;
   [[nodiscard]] bool tracks_allocations() const noexcept;
   template <application_object_model Object> boost::asio::awaitable<std::uint64_t> allocation_next() const;
   void add_precommit_observer(std::shared_ptr<precommit_observer> value);

   boost::asio::awaitable<void> commit();
   boost::asio::awaitable<void> rollback();

 private:
   class access;
   struct impl;

   explicit transaction(std::shared_ptr<impl> implementation);
   transaction(forge::db::core::transaction&& active, forge::db::core::family family, ensure_registered_fn ensure,
               allocate_id_fn allocate, seal_allocations_fn seal,
               std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
               release_fn release, boost::asio::any_io_executor cleanup_executor, bool backend_writes,
               bool reuse_rolled_back_ids);
   transaction(forge::db::core::transaction& active, forge::db::core::family family, ensure_registered_fn ensure,
               allocate_id_fn allocate, seal_allocations_fn seal,
               std::vector<std::shared_ptr<interceptor>> interceptors, std::vector<std::shared_ptr<observer>> observers,
               release_fn release, bool backend_writes, bool reuse_rolled_back_ids);

   [[nodiscard]] change_set& changes() const;
   [[nodiscard]] forge::db::core::transaction& active_transaction() const;

   void ensure_registered_type(forge::db::ids::object_id type, std::type_index model) const;
   boost::asio::awaitable<void> before_mutation(const object_mutation& mutation) const;
   boost::asio::awaitable<forge::db::ids::object_id> allocate_id(forge::db::ids::object_id type) const;
   boost::asio::awaitable<std::uint64_t> read_allocation_next(forge::db::ids::object_id type) const;
   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get_record(forge::db::core::record_key key) const;
   boost::asio::awaitable<void> put_record(forge::db::core::record_key key, std::vector<std::byte> value) const;
   boost::asio::awaitable<void> erase_record(forge::db::core::record_key key) const;
   boost::asio::awaitable<std::optional<std::vector<std::byte>>> lock_record(forge::db::core::record_key key) const;
   boost::asio::awaitable<forge::db::core::record_page> scan_records(forge::db::core::record_range range,
                                                                     forge::db::core::page_request request) const;

   std::shared_ptr<impl> impl_;
   bool owns_commit_ = false;

   friend class detail::transaction_access;
};

namespace detail {

class transaction_access {
 public:
   using tracked_allocate_id_fn = std::function<boost::asio::awaitable<forge::db::ids::object_id>(
       forge::db::ids::object_id, forge::db::core::transaction&, change_set&)>;

   [[nodiscard]] static transaction
   make_owned(forge::db::core::transaction&& active, forge::db::core::family family,
              transaction::ensure_registered_fn ensure, transaction::allocate_id_fn allocate,
              transaction::seal_allocations_fn seal, std::vector<std::shared_ptr<interceptor>> interceptors,
              std::vector<std::shared_ptr<observer>> observers, transaction::release_fn release,
              boost::asio::any_io_executor cleanup_executor, bool backend_writes, bool reuse_rolled_back_ids);
   [[nodiscard]] static transaction
   make_joined(forge::db::core::transaction& active, forge::db::core::family family,
               transaction::ensure_registered_fn ensure, transaction::allocate_id_fn allocate,
               transaction::seal_allocations_fn seal, std::vector<std::shared_ptr<interceptor>> interceptors,
               std::vector<std::shared_ptr<observer>> observers, transaction::release_fn release, bool backend_writes,
               bool reuse_rolled_back_ids);
   static void bind_store(transaction& active, std::shared_ptr<const void> identity,
                          tracked_allocate_id_fn allocate = {});
   [[nodiscard]] static bool belongs_to(const transaction& active, const void* identity) noexcept;
   [[nodiscard]] static transaction joined(transaction& active);
   static void begin_allocation_restore(transaction& active, std::size_t count);
   static void publish_restored_allocation(transaction& active, allocation_change value) noexcept;
   static void finish_allocation_restore(transaction& active) noexcept;
   static void fail_allocation_restore(transaction& active) noexcept;
};

} // namespace detail

} // namespace forge::db::object

namespace forge::db::object {

class transaction::access {
 public:
   explicit access(const transaction& owner) : owner_{owner} {}

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::record_key key) const {
      co_return co_await owner_.get_record(std::move(key));
   }

   boost::asio::awaitable<void> put(forge::db::core::record_key key, std::vector<std::byte> value) const {
      co_await owner_.put_record(std::move(key), std::move(value));
   }

   boost::asio::awaitable<void> erase(forge::db::core::record_key key) const {
      co_await owner_.erase_record(std::move(key));
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> lock(forge::db::core::record_key key) const {
      co_return co_await owner_.lock_record(std::move(key));
   }

   boost::asio::awaitable<forge::db::core::record_page> scan_page(forge::db::core::record_range range,
                                                                  forge::db::core::page_request request) const {
      co_return co_await owner_.scan_records(std::move(range), std::move(request));
   }

   [[nodiscard]] change_set& changes() const {
      return owner_.changes();
   }

   template <object_model Object> void ensure_registered() const {
      owner_.ensure_registered_type(object_id_of<Object>::value, std::type_index{typeid(Object)});
   }

   boost::asio::awaitable<void> before_mutation(const object_mutation& mutation) const {
      return owner_.before_mutation(mutation);
   }

   boost::asio::awaitable<forge::db::ids::object_id> allocate_id(forge::db::ids::object_id type) const {
      co_return co_await owner_.allocate_id(type);
   }

 private:
   const transaction& owner_;
};

} // namespace forge::db::object

#include "ordered_key.hxx"

namespace forge::db::object::detail {

template <typename T> std::vector<std::byte> to_byte_vector(const std::vector<T>& input) {
   auto out = std::vector<std::byte>{};
   out.reserve(input.size());
   for (auto value : input) {
      out.push_back(static_cast<std::byte>(value));
   }
   return out;
}

inline std::vector<std::uint8_t> to_uint8_vector(const std::vector<std::byte>& input) {
   auto out = std::vector<std::uint8_t>{};
   out.reserve(input.size());
   for (auto value : input) {
      out.push_back(static_cast<std::uint8_t>(value));
   }
   return out;
}

template <typename T> std::vector<std::byte> pack_value(const T& value) {
   auto bytes = std::vector<std::uint8_t>{};
   forge::raw::pack(bytes, value);
   return to_byte_vector(bytes);
}

template <typename T> T unpack_value(const std::vector<std::byte>& bytes) {
   return forge::raw::unpack<T>(to_uint8_vector(bytes));
}

[[noreturn]] inline void throw_ranked_error(const ranked_index::error& failure) {
   switch (failure.code) {
   case ranked_index::error_code::rebuild_required:
      FORGE_THROW_EXCEPTION(exceptions::aggregate_rebuild_required, failure.what());
   case ranked_index::error_code::corruption:
      FORGE_THROW_EXCEPTION(exceptions::aggregate_corruption, failure.what());
   case ranked_index::error_code::overflow:
      FORGE_THROW_EXCEPTION(exceptions::aggregate_overflow, failure.what());
   }
   FORGE_THROW_EXCEPTION(exceptions::aggregate_corruption, "unknown ranked index failure");
}

template <typename Access> ranked_index::read_access make_ranked_read_access(Access source) {
   return ranked_index::read_access{
       .get = [source](ranked_index::bytes key) mutable -> boost::asio::awaitable<std::optional<ranked_index::bytes>> {
          co_return co_await source.get(forge::db::core::record_key{std::move(key)});
       },
       .next = [source](ranked_index::bytes prefix, ranked_index::bytes after) mutable
           -> boost::asio::awaitable<std::optional<ranked_index::record>> {
          auto range = detail::ordered_key::prefix_range(std::move(prefix));
          auto page = co_await source.scan_page(
              std::move(range),
              forge::db::core::page_request{
                  .after = forge::db::core::cursor{forge::db::core::record_key{std::move(after)}}, .limit = 1});
          if (page.entries.empty()) {
             co_return std::nullopt;
          }
          co_return ranked_index::record{.key = page.entries.front().key.bytes(),
                                         .value = std::move(page.entries.front().value)};
       },
       .has_any = [source](ranked_index::bytes prefix) mutable -> boost::asio::awaitable<bool> {
          auto page = co_await source.scan_page(detail::ordered_key::prefix_range(std::move(prefix)),
                                                forge::db::core::page_request{.limit = 1});
          co_return !page.entries.empty();
       },
   };
}

template <typename Access> ranked_index::write_access make_ranked_write_access(Access source) {
   auto read = make_ranked_read_access(source);
   auto result = ranked_index::write_access{};
   result.get = std::move(read.get);
   result.next = std::move(read.next);
   result.has_any = std::move(read.has_any);
   result.lock =
       [source](ranked_index::bytes key) mutable -> boost::asio::awaitable<std::optional<ranked_index::bytes>> {
      co_return co_await source.lock(forge::db::core::record_key{std::move(key)});
   };
   result.put = [source](ranked_index::bytes key, ranked_index::bytes value) mutable -> boost::asio::awaitable<void> {
      co_await source.put(forge::db::core::record_key{std::move(key)}, std::move(value));
   };
   result.erase = [source](ranked_index::bytes key) mutable -> boost::asio::awaitable<void> {
      co_await source.erase(forge::db::core::record_key{std::move(key)});
   };
   return result;
}

template <object_model Object> id_t_of<Object> typed_id_from(forge::db::ids::object_id id) {
   if (!forge::db::ids::matches<id_t_of<Object>::space, id_t_of<Object>::type>(id)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "object_id does not match db object type");
   }
   return id_t_of<Object>{id.instance};
}

template <object_model Object, typename Access>
boost::asio::awaitable<std::optional<typename Object::value_type>>
read_transaction_object(Access tx, forge::db::ids::object_id id) {
   tx.template ensure_registered<Object>();
   const auto typed = typed_id_from<Object>(id);
   const auto key = detail::ordered_key::object_record_key<Object>(typed);
   const auto bytes = co_await tx.get(key);
   if (!bytes.has_value()) {
      co_return std::nullopt;
   }
   co_return unpack_value<typename Object::value_type>(*bytes);
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<object_page<typename Object::value_type>>
page_transaction_objects(Access tx, forge::db::core::record_range range, forge::db::core::page_request request) {
   tx.template ensure_registered<Object>();
   forge::db::object::validate_page_request(request);

   auto records = co_await tx.scan_page(std::move(range), std::move(request));
   auto out = object_page<typename Object::value_type>{};
   out.next = std::move(records.next);

   for (const auto& entry : records.entries) {
      using index = index_by_tag<Object, Tag>;
      if constexpr (primary_index<index>) {
         out.items.push_back(unpack_value<typename Object::value_type>(entry.value));
      } else {
         const auto id = unpack_value<id_t_of<Object>>(entry.value);
         auto value = co_await read_transaction_object<Object>(tx, forge::db::ids::to_object_id(id));
         if (!value.has_value()) {
            FORGE_THROW_EXCEPTION(exceptions::not_found, "db object index points to a missing object");
         }
         out.items.push_back(std::move(*value));
      }
   }

   co_return out;
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<index_aggregate_result> query_transaction_aggregate(Access tx,
                                                                           forge::db::core::record_range range) {
   using index = index_by_tag<Object, Tag>;
   static_assert(forge::db::object::ranked_index<index>);
   try {
      const auto descriptor = detail::ordered_key::ranked_layout<Object, Tag>();
      auto result = co_await ranked_index::query(make_ranked_read_access(tx), descriptor,
                                                 detail::ordered_key::ranked_bounds<Object, Tag>(descriptor, range));
      co_return index_aggregate_result{.count = result.count, .sums = std::move(result.sums)};
   } catch (const ranked_index::error& failure) {
      throw_ranked_error(failure);
   }
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<index_rank_result> query_transaction_ranks(Access tx, forge::db::core::record_range range) {
   using index = index_by_tag<Object, Tag>;
   static_assert(forge::db::object::ranked_index<index>);
   try {
      const auto descriptor = detail::ordered_key::ranked_layout<Object, Tag>();
      const auto result = co_await ranked_index::query_ranks(
          make_ranked_read_access(tx), descriptor, detail::ordered_key::ranked_bounds<Object, Tag>(descriptor, range));
      co_return index_rank_result{
          .lower = result.lower,
          .upper = result.upper,
          .size = result.size,
      };
   } catch (const ranked_index::error& failure) {
      throw_ranked_error(failure);
   }
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<std::optional<typename Object::value_type>> nth_transaction_object(Access tx,
                                                                                          std::uint64_t position) {
   using index = index_by_tag<Object, Tag>;
   static_assert(forge::db::object::ranked_index<index>);
   try {
      const auto descriptor = detail::ordered_key::ranked_layout<Object, Tag>();
      const auto logical = co_await ranked_index::nth_key(make_ranked_read_access(tx), descriptor, position);
      if (!logical.has_value()) {
         co_return std::nullopt;
      }
      const auto encoded = co_await tx.get(forge::db::core::record_key{ranked_index::source_key(descriptor, *logical)});
      if (!encoded.has_value()) {
         FORGE_THROW_EXCEPTION(exceptions::aggregate_corruption, "ranked index points to a missing source record");
      }
      if constexpr (primary_index<index>) {
         co_return unpack_value<typename Object::value_type>(*encoded);
      } else {
         const auto id = unpack_value<id_t_of<Object>>(*encoded);
         auto value = co_await read_transaction_object<Object>(tx, forge::db::ids::to_object_id(id));
         if (!value.has_value()) {
            FORGE_THROW_EXCEPTION(exceptions::aggregate_corruption, "ranked index points to a missing object");
         }
         co_return std::move(*value);
      }
   } catch (const ranked_index::error& failure) {
      throw_ranked_error(failure);
   }
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<std::optional<std::uint64_t>>
query_transaction_exact_rank(Access tx, const typename Object::value_type& value) {
   if (!(co_await detail::ordered_key::ranked_entry_exists<Object, Tag>(
           tx, value, [](const std::vector<std::byte>& encoded, const id_t_of<Object>& expected) {
              return detail::unpack_value<id_t_of<Object>>(encoded) == expected;
           }))) {
      co_return std::nullopt;
   }
   const auto bounds =
       co_await query_transaction_ranks<Object, Tag>(tx, detail::ordered_key::range_for_value<Object, Tag>(value));
   if (bounds.upper - bounds.lower != 1U) {
      co_return std::nullopt;
   }
   co_return bounds.lower;
}

struct materialized_index {
   forge::db::core::record_key key;
   bool unique = false;
};

template <object_model Object, std::size_t Index = 0>
void materialize_indexes(const typename Object::value_type& value, std::vector<materialized_index>& out) {
   using indexes = typename Object::indexes_type::tuple_type;
   if constexpr (Index < std::tuple_size_v<indexes>) {
      using index = std::tuple_element_t<Index, indexes>;
      if constexpr (secondary_index<index>) {
         out.push_back(materialized_index{
             .key = detail::ordered_key::index_entry_key<Object, typename index::tag_type>(value),
             .unique = index::kind == index_kind::ordered_unique,
         });
      }
      materialize_indexes<Object, Index + 1>(value, out);
   }
}

template <object_model Object>
[[nodiscard]] std::vector<materialized_index> materialize_indexes(const typename Object::value_type& value) {
   auto out = std::vector<materialized_index>{};
   out.reserve(Object::indexes_type::size - 1U);
   materialize_indexes<Object>(value, out);
   return out;
}

template <object_model Object, typename Access>
boost::asio::awaitable<void> verify_unique_indexes(Access tx, const typename Object::value_type& value,
                                                   const std::vector<materialized_index>& indexes) {
   for (const auto& index : indexes) {
      if (!index.unique) {
         continue;
      }
      const auto existing = co_await tx.get(index.key);
      if (existing.has_value()) {
         const auto existing_id = unpack_value<id_t_of<Object>>(*existing);
         if (existing_id != value.id) {
            FORGE_THROW_EXCEPTION(exceptions::duplicate_object, "db object unique index value already exists");
         }
      }
   }
   co_return;
}

template <typename Access, typename Id>
boost::asio::awaitable<void> write_secondary_indexes(Access tx, const std::vector<materialized_index>& indexes, Id id) {
   const auto packed_id = pack_value(id);
   for (const auto& index : indexes) {
      co_await tx.put(index.key, packed_id);
   }
   co_return;
}

template <typename Access>
boost::asio::awaitable<void> remove_secondary_indexes(Access tx, const std::vector<materialized_index>& indexes) {
   for (const auto& index : indexes) {
      co_await tx.erase(index.key);
   }
   co_return;
}

template <object_model Object, std::size_t Index = 0U>
boost::asio::awaitable<void> lock_ranked_indexes(const ranked_index::write_access& access) {
   using indexes = typename Object::indexes_type::tuple_type;
   if constexpr (Index < std::tuple_size_v<indexes>) {
      using index = std::tuple_element_t<Index, indexes>;
      if constexpr (forge::db::object::ranked_index<index>) {
         try {
            co_await ranked_index::lock_root(access,
                                             detail::ordered_key::ranked_layout<Object, typename index::tag_type>());
         } catch (const ranked_index::error& failure) {
            throw_ranked_error(failure);
         }
      }
      co_await lock_ranked_indexes<Object, Index + 1U>(access);
   }
}

template <object_model Object, std::size_t Index = 0U>
boost::asio::awaitable<void>
plan_ranked_indexes(const typename Object::value_type* before, const typename Object::value_type* after,
                    const ranked_index::write_access& access, std::vector<ranked_index::mutation_plan>& plans) {
   using indexes = typename Object::indexes_type::tuple_type;
   if constexpr (Index < std::tuple_size_v<indexes>) {
      using index = std::tuple_element_t<Index, indexes>;
      if constexpr (forge::db::object::ranked_index<index>) {
         try {
            auto old_entry = std::optional<ranked_index::entry>{};
            auto new_entry = std::optional<ranked_index::entry>{};
            if (before) {
               old_entry.emplace(ranked_index::entry{
                   .key = detail::ordered_key::logical_key<Object, typename index::tag_type>(*before),
                   .contribution = detail::ordered_key::ranked_contribution<index>(*before)});
            }
            if (after) {
               new_entry.emplace(ranked_index::entry{
                   .key = detail::ordered_key::logical_key<Object, typename index::tag_type>(*after),
                   .contribution = detail::ordered_key::ranked_contribution<index>(*after)});
            }
            plans.push_back(co_await ranked_index::plan_change(
                access, detail::ordered_key::ranked_layout<Object, typename index::tag_type>(), std::move(old_entry),
                std::move(new_entry)));
         } catch (const ranked_index::error& failure) {
            throw_ranked_error(failure);
         }
      }
      co_await plan_ranked_indexes<Object, Index + 1U>(before, after, access, plans);
   }
}

inline boost::asio::awaitable<void> apply_ranked_plans(const ranked_index::write_access& access,
                                                       std::vector<ranked_index::mutation_plan> plans) {
   try {
      for (auto& plan : plans) {
         co_await ranked_index::apply(access, std::move(plan));
      }
   } catch (const ranked_index::error& failure) {
      throw_ranked_error(failure);
   }
}

template <object_model Object, typename Access>
boost::asio::awaitable<std::vector<ranked_index::mutation_plan>>
prepare_ranked_change(Access tx, const typename Object::value_type* before, const typename Object::value_type* after) {
   auto access = make_ranked_write_access(tx);
   co_await lock_ranked_indexes<Object>(access);
   auto plans = std::vector<ranked_index::mutation_plan>{};
   co_await plan_ranked_indexes<Object>(before, after, access, plans);
   co_return plans;
}

template <typename Access, application_object_value Value>
boost::asio::awaitable<void> insert_object(Access tx, Value value) {
   using object_model_type = index_for_id_t<typename Value::id_t>;
   tx.template ensure_registered<object_model_type>();

   const auto object_key = detail::ordered_key::object_record_key<object_model_type>(value.id);
   if ((co_await tx.get(object_key)).has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_object, "db object id already exists");
   }

   auto after = pack_value(value);
   const auto indexes = materialize_indexes<object_model_type>(value);
   auto mutation = object_mutation{
       .kind = mutation_kind::insert,
       .id = forge::db::ids::to_object_id(value.id),
       .after = after,
   };
   co_await tx.before_mutation(mutation);
   co_await verify_unique_indexes<object_model_type>(tx, value, indexes);
   auto ranked = co_await prepare_ranked_change<object_model_type>(tx, nullptr, std::addressof(value));
   co_await tx.put(object_key, std::move(after));
   co_await write_secondary_indexes(tx, indexes, value.id);
   co_await apply_ranked_plans(make_ranked_write_access(tx), std::move(ranked));
   tx.changes().mutations.push_back(std::move(mutation));
   co_return;
}

template <typename Access, application_object_value Value>
boost::asio::awaitable<void> replace_object(Access tx, Value value, mutation_kind kind) {
   using object_model_type = index_for_id_t<typename Value::id_t>;
   tx.template ensure_registered<object_model_type>();

   const auto existing =
       co_await read_transaction_object<object_model_type>(tx, forge::db::ids::to_object_id(value.id));
   if (!existing.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::not_found, "db object was not found");
   }

   auto before = pack_value(*existing);
   auto after = pack_value(value);
   const auto old_indexes = materialize_indexes<object_model_type>(*existing);
   const auto new_indexes = materialize_indexes<object_model_type>(value);
   auto mutation = object_mutation{
       .kind = kind,
       .id = forge::db::ids::to_object_id(value.id),
       .before = before,
       .after = after,
   };
   co_await tx.before_mutation(mutation);
   co_await verify_unique_indexes<object_model_type>(tx, value, new_indexes);
   auto ranked =
       co_await prepare_ranked_change<object_model_type>(tx, std::addressof(*existing), std::addressof(value));
   co_await remove_secondary_indexes(tx, old_indexes);
   co_await tx.put(detail::ordered_key::object_record_key<object_model_type>(value.id), std::move(after));
   co_await write_secondary_indexes(tx, new_indexes, value.id);
   co_await apply_ranked_plans(make_ranked_write_access(tx), std::move(ranked));
   tx.changes().mutations.push_back(std::move(mutation));
   co_return;
}

template <application_object_model Object, typename Access>
boost::asio::awaitable<void> erase_object(Access tx, forge::db::ids::object_id id) {
   tx.template ensure_registered<Object>();
   const auto typed = typed_id_from<Object>(id);
   const auto existing = co_await read_transaction_object<Object>(tx, forge::db::ids::to_object_id(typed));
   if (!existing.has_value()) {
      co_return;
   }

   auto before = pack_value(*existing);
   const auto indexes = materialize_indexes<Object>(*existing);
   auto mutation = object_mutation{
       .kind = mutation_kind::erase,
       .id = forge::db::ids::to_object_id(typed),
       .before = before,
   };
   co_await tx.before_mutation(mutation);
   auto ranked = co_await prepare_ranked_change<Object>(tx, std::addressof(*existing), nullptr);
   co_await remove_secondary_indexes(tx, indexes);
   co_await tx.erase(detail::ordered_key::object_record_key<Object>(typed));
   co_await apply_ranked_plans(make_ranked_write_access(tx), std::move(ranked));
   tx.changes().mutations.push_back(std::move(mutation));
   co_return;
}

template <typename Access, application_object_value Value, typename Fn>
   requires std::default_initializable<Value> && std::invocable<Fn&, Value&>
boost::asio::awaitable<Value> create_object(Access tx, Fn&& fn) {
   using object_model_type = index_for_id_t<typename Value::id_t>;
   tx.template ensure_registered<object_model_type>();

   auto allocated = co_await tx.allocate_id(object_id_of<object_model_type>::value);
   auto value = Value{};
   value.id = typed_id_from<object_model_type>(allocated);
   const auto generated_id = value.id;

   using result_type = std::invoke_result_t<Fn&, Value&>;
   static_assert(std::is_void_v<result_type>, "db object create initializer must return void");
   std::invoke(fn, value);
   if (value.id != generated_id) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor,
                            "db object create initializer must not change generated id");
   }

   co_await insert_object(tx, value);
   co_return value;
}

} // namespace forge::db::object::detail

export namespace forge::db::object {

template <forge::db::ids::typed_id_like Id>
boost::asio::awaitable<typename index_for_id_t<Id>::value_type> transaction::get(Id id) {
   co_return co_await get<index_for_id_t<Id>>(forge::db::ids::to_object_id(id));
}

template <forge::db::ids::typed_id_like Id>
boost::asio::awaitable<std::optional<typename index_for_id_t<Id>::value_type>> transaction::find(Id id) {
   co_return co_await find<index_for_id_t<Id>>(forge::db::ids::to_object_id(id));
}

template <object_model Object>
boost::asio::awaitable<typename Object::value_type> transaction::get(forge::db::ids::object_id id) {
   const auto value = co_await find<Object>(id);
   if (!value.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::not_found, "db object was not found");
   }
   co_return *value;
}

template <object_model Object>
boost::asio::awaitable<std::optional<typename Object::value_type>> transaction::find(forge::db::ids::object_id id) {
   co_return co_await detail::read_transaction_object<Object>(access{*this}, id);
}

template <application_object_model Object> boost::asio::awaitable<std::uint64_t> transaction::allocation_next() const {
   ensure_registered_type(object_id_of<Object>::value, std::type_index{typeid(Object)});
   co_return co_await read_allocation_next(object_id_of<Object>::value);
}

template <application_object_value Value> boost::asio::awaitable<void> transaction::insert(Value value) {
   co_await detail::insert_object(access{*this}, std::move(value));
   co_return;
}

template <application_object_value Value, typename Fn>
   requires std::default_initializable<Value> && std::invocable<Fn&, Value&>
boost::asio::awaitable<Value> transaction::create(Fn&& fn) {
   co_return co_await detail::create_object<access, Value>(access{*this}, std::forward<Fn>(fn));
}

template <application_object_value Value> boost::asio::awaitable<void> transaction::replace(Value value) {
   co_await detail::replace_object(access{*this}, std::move(value), mutation_kind::replace);
   co_return;
}

template <forge::db::ids::typed_id_like Id, typename Fn>
   requires application_object_model<index_for_id_t<Id>>
boost::asio::awaitable<void> transaction::modify(Id id, Fn&& fn) {
   using object_model_type = index_for_id_t<Id>;
   auto next = co_await get(id);
   using result_type = std::invoke_result_t<Fn&, typename object_model_type::value_type&>;
   static_assert(std::is_void_v<result_type>, "db object modify mutator must return void");
   std::invoke(fn, next);
   co_await detail::replace_object(access{*this}, std::move(next), mutation_kind::modify);
   co_return;
}

template <forge::db::ids::typed_id_like Id>
   requires application_object_model<index_for_id_t<Id>>
boost::asio::awaitable<void> transaction::erase(Id id) {
   co_await erase<index_for_id_t<Id>>(forge::db::ids::to_object_id(id));
   co_return;
}

template <application_object_model Object>
boost::asio::awaitable<void> transaction::erase(forge::db::ids::object_id id) {
   co_await detail::erase_object<Object>(access{*this}, id);
   co_return;
}

template <object_model Object, typename Tag> [[nodiscard]] index_view<Object, Tag> transaction::index() const {
   access{*this}.template ensure_registered<Object>();
   auto aggregate = index_aggregate_query{};
   auto ranks = index_rank_query{};
   auto nth = index_nth_query<typename Object::value_type>{};
   auto exact_rank = index_exact_rank_query<typename Object::value_type>{};
   if constexpr (ranked_index<index_by_tag<Object, Tag>>) {
      aggregate = [implementation = impl_](
                      forge::db::core::record_range range) mutable -> boost::asio::awaitable<index_aggregate_result> {
         auto owner = transaction{implementation};
         co_return co_await detail::query_transaction_aggregate<Object, Tag>(access{owner}, std::move(range));
      };
      ranks = [implementation =
                   impl_](forge::db::core::record_range range) mutable -> boost::asio::awaitable<index_rank_result> {
         auto owner = transaction{implementation};
         co_return co_await detail::query_transaction_ranks<Object, Tag>(access{owner}, std::move(range));
      };
      nth = [implementation = impl_](
                std::uint64_t position) mutable -> boost::asio::awaitable<std::optional<typename Object::value_type>> {
         auto owner = transaction{implementation};
         co_return co_await detail::nth_transaction_object<Object, Tag>(access{owner}, position);
      };
      exact_rank = [implementation = impl_](const typename Object::value_type& value) mutable
          -> boost::asio::awaitable<std::optional<std::uint64_t>> {
         auto owner = transaction{implementation};
         co_return co_await detail::query_transaction_exact_rank<Object, Tag>(access{owner}, value);
      };
   }
   return index_view<Object, Tag>{
       [implementation = impl_](forge::db::core::record_range range, forge::db::core::page_request request) mutable
           -> boost::asio::awaitable<object_page<typename Object::value_type>> {
          auto owner = transaction{implementation};
          co_return co_await detail::page_transaction_objects<Object, Tag>(access{owner}, std::move(range),
                                                                           std::move(request));
       },
       {},
       std::move(aggregate),
       std::move(ranks),
       std::move(nth),
       std::move(exact_rank)};
}

} // namespace forge::db::object
