module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include "ranked_index.hxx"

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <typeindex>
#include <typeinfo>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.db.object.snapshot;

import forge.db.ids.object_id;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.cursor;
import forge.db.object.exceptions;
import forge.db.object.index;
import forge.db.object.object;
import forge.raw.raw;

export namespace forge::db::object {

class snapshot {
 public:
   using ensure_registered_fn = std::function<void(forge::db::ids::object_id, std::type_index)>;

   snapshot() = default;
   snapshot(forge::db::core::snapshot active, forge::db::core::family family, ensure_registered_fn ensure);

   template <application_object_model Object> boost::asio::awaitable<std::uint64_t> allocation_next() const;

   template <forge::db::ids::typed_id_like Id>
   boost::asio::awaitable<typename index_for_id_t<Id>::value_type> get(Id id);

   template <forge::db::ids::typed_id_like Id>
   boost::asio::awaitable<std::optional<typename index_for_id_t<Id>::value_type>> find(Id id);

   template <object_model Object>
   boost::asio::awaitable<typename Object::value_type> get(forge::db::ids::object_id id);

   template <object_model Object>
   boost::asio::awaitable<std::optional<typename Object::value_type>> find(forge::db::ids::object_id id);

   template <object_model Object, typename Tag>
   [[nodiscard]] index_view<Object, Tag> index() const;

 private:
   class access;

   void ensure_registered_type(forge::db::ids::object_id type, std::type_index model) const;
   boost::asio::awaitable<std::uint64_t> read_allocation_next(forge::db::ids::object_id type) const;
   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get_record(forge::db::core::record_key key) const;
   boost::asio::awaitable<forge::db::core::record_page> scan_records(forge::db::core::record_range range, forge::db::core::page_request request) const;

   struct impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::db::object

#include "ordered_key.hxx"

namespace forge::db::object {

class snapshot::access {
 public:
   explicit access(const snapshot& owner) : owner_{owner} {}

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::record_key key) const {
      co_return co_await owner_.get_record(std::move(key));
   }

   boost::asio::awaitable<forge::db::core::record_page> scan_page(forge::db::core::record_range range, forge::db::core::page_request request) const {
      co_return co_await owner_.scan_records(std::move(range), std::move(request));
   }

   template <object_model Object>
   void ensure_registered() const {
      owner_.ensure_registered_type(object_id_of<Object>::value, std::type_index{typeid(Object)});
   }

 private:
   const snapshot& owner_;
};

} // namespace forge::db::object

#include "record_key.hxx"

namespace forge::db::object::detail {

template <object_model Object>
[[nodiscard]] forge::db::core::record_key object_record_key(id_t_of<Object> id) {
   return forge::db::object::detail::record_key::object(forge::db::ids::to_object_id(id));
}

inline std::vector<std::uint8_t> to_uint8_vector(const std::vector<std::byte>& input) {
   auto out = std::vector<std::uint8_t>{};
   out.reserve(input.size());
   for (auto value : input) {
      out.push_back(static_cast<std::uint8_t>(value));
   }
   return out;
}

template <typename T>
T unpack_value(const std::vector<std::byte>& bytes) {
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

template <typename Access>
ranked_index::read_access make_ranked_read_access(Access source) {
   return ranked_index::read_access{
      .get = [source](ranked_index::bytes key) mutable
         -> boost::asio::awaitable<std::optional<ranked_index::bytes>> {
         co_return co_await source.get(forge::db::core::record_key{std::move(key)});
      },
      .next = [source](ranked_index::bytes prefix, ranked_index::bytes after) mutable
         -> boost::asio::awaitable<std::optional<ranked_index::record>> {
         auto page = co_await source.scan_page(
            detail::ordered_key::prefix_range(std::move(prefix)),
            forge::db::core::page_request{
               .after = forge::db::core::cursor{forge::db::core::record_key{std::move(after)}},
               .limit = 1});
         if (page.entries.empty()) {
            co_return std::nullopt;
         }
         co_return ranked_index::record{
            .key = page.entries.front().key.bytes(),
            .value = std::move(page.entries.front().value)};
      },
      .has_any = [source](ranked_index::bytes prefix) mutable -> boost::asio::awaitable<bool> {
         auto page = co_await source.scan_page(
            detail::ordered_key::prefix_range(std::move(prefix)),
            forge::db::core::page_request{.limit = 1});
         co_return !page.entries.empty();
      },
   };
}

template <object_model Object>
id_t_of<Object> typed_id_from(forge::db::ids::object_id id) {
   if (!forge::db::ids::matches<id_t_of<Object>::space, id_t_of<Object>::type>(id)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "object_id does not match db object type");
   }
   return id_t_of<Object>{id.instance};
}

template <object_model Object, typename Access>
boost::asio::awaitable<std::optional<typename Object::value_type>> read_snapshot_object(Access view,
                                                                                        forge::db::ids::object_id id) {
   view.template ensure_registered<Object>();
   const auto typed = typed_id_from<Object>(id);
   const auto key = object_record_key<Object>(typed);
   const auto bytes = co_await view.get(key);
   if (!bytes.has_value()) {
      co_return std::nullopt;
   }
   co_return unpack_value<typename Object::value_type>(*bytes);
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<object_page<typename Object::value_type>> page_snapshot_objects(Access view,
                                                                                       forge::db::core::record_range range,
                                                                                       forge::db::core::page_request request) {
   view.template ensure_registered<Object>();
   forge::db::object::validate_page_request(request);

   auto records = co_await view.scan_page(std::move(range), std::move(request));
   auto out = object_page<typename Object::value_type>{};
   out.next = std::move(records.next);

   for (const auto& entry : records.entries) {
      using index = index_by_tag<Object, Tag>;
      if constexpr (primary_index<index>) {
         out.items.push_back(unpack_value<typename Object::value_type>(entry.value));
      } else {
         const auto id = unpack_value<id_t_of<Object>>(entry.value);
         auto value = co_await read_snapshot_object<Object>(view, forge::db::ids::to_object_id(id));
         if (!value.has_value()) {
            FORGE_THROW_EXCEPTION(exceptions::not_found, "db object index points to a missing object");
         }
         out.items.push_back(std::move(*value));
      }
   }

   co_return out;
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<index_aggregate_result>
query_snapshot_aggregate(Access view, forge::db::core::record_range range) {
   try {
      const auto descriptor = detail::ordered_key::ranked_layout<Object, Tag>();
      auto result = co_await ranked_index::query(
         make_ranked_read_access(view), descriptor,
         detail::ordered_key::ranked_bounds<Object, Tag>(descriptor, range));
      co_return index_aggregate_result{.count = result.count, .sums = std::move(result.sums)};
   } catch (const ranked_index::error& failure) {
      throw_ranked_error(failure);
   }
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<index_rank_result>
query_snapshot_ranks(Access view, forge::db::core::record_range range) {
   try {
      const auto descriptor = detail::ordered_key::ranked_layout<Object, Tag>();
      const auto result = co_await ranked_index::query_ranks(
         make_ranked_read_access(view), descriptor,
         detail::ordered_key::ranked_bounds<Object, Tag>(descriptor, range));
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
boost::asio::awaitable<std::optional<typename Object::value_type>>
nth_snapshot_object(Access view, std::uint64_t position) {
   using index = index_by_tag<Object, Tag>;
   try {
      const auto descriptor = detail::ordered_key::ranked_layout<Object, Tag>();
      const auto logical = co_await ranked_index::nth_key(make_ranked_read_access(view), descriptor, position);
      if (!logical.has_value()) {
         co_return std::nullopt;
      }
      const auto encoded = co_await view.get(forge::db::core::record_key{
         ranked_index::source_key(descriptor, *logical)});
      if (!encoded.has_value()) {
         FORGE_THROW_EXCEPTION(exceptions::aggregate_corruption,
                               "ranked index points to a missing source record");
      }
      if constexpr (primary_index<index>) {
         co_return unpack_value<typename Object::value_type>(*encoded);
      } else {
         const auto id = unpack_value<id_t_of<Object>>(*encoded);
         auto value = co_await read_snapshot_object<Object>(view, forge::db::ids::to_object_id(id));
         if (!value.has_value()) {
            FORGE_THROW_EXCEPTION(exceptions::aggregate_corruption,
                                  "ranked index points to a missing object");
         }
         co_return std::move(*value);
      }
   } catch (const ranked_index::error& failure) {
      throw_ranked_error(failure);
   }
}

template <object_model Object, typename Tag, typename Access>
boost::asio::awaitable<std::optional<std::uint64_t>>
query_snapshot_exact_rank(Access view, const typename Object::value_type& value) {
   if (!(co_await detail::ordered_key::ranked_entry_exists<Object, Tag>(
          view, value,
          [](const std::vector<std::byte>& encoded, const id_t_of<Object>& expected) {
             return detail::unpack_value<id_t_of<Object>>(encoded) == expected;
          }))) {
      co_return std::nullopt;
   }
   const auto bounds = co_await query_snapshot_ranks<Object, Tag>(
      view, detail::ordered_key::range_for_value<Object, Tag>(value));
   if (bounds.upper - bounds.lower != 1U) {
      co_return std::nullopt;
   }
   co_return bounds.lower;
}

} // namespace forge::db::object::detail

export namespace forge::db::object {

template <application_object_model Object> boost::asio::awaitable<std::uint64_t> snapshot::allocation_next() const {
   ensure_registered_type(object_id_of<Object>::value, std::type_index{typeid(Object)});
   co_return co_await read_allocation_next(object_id_of<Object>::value);
}

template <forge::db::ids::typed_id_like Id>
boost::asio::awaitable<typename index_for_id_t<Id>::value_type> snapshot::get(Id id) {
   co_return co_await get<index_for_id_t<Id>>(forge::db::ids::to_object_id(id));
}

template <forge::db::ids::typed_id_like Id>
boost::asio::awaitable<std::optional<typename index_for_id_t<Id>::value_type>> snapshot::find(Id id) {
   co_return co_await find<index_for_id_t<Id>>(forge::db::ids::to_object_id(id));
}

template <object_model Object>
boost::asio::awaitable<typename Object::value_type> snapshot::get(forge::db::ids::object_id id) {
   const auto value = co_await find<Object>(id);
   if (!value.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::not_found, "db object was not found");
   }
   co_return *value;
}

template <object_model Object>
boost::asio::awaitable<std::optional<typename Object::value_type>> snapshot::find(forge::db::ids::object_id id) {
   co_return co_await detail::read_snapshot_object<Object>(access{*this}, id);
}

template <object_model Object, typename Tag>
[[nodiscard]] index_view<Object, Tag> snapshot::index() const {
   access{*this}.template ensure_registered<Object>();
   auto aggregate = index_aggregate_query{};
   auto ranks = index_rank_query{};
   auto nth = index_nth_query<typename Object::value_type>{};
   auto exact_rank = index_exact_rank_query<typename Object::value_type>{};
   if constexpr (ranked_index<index_by_tag<Object, Tag>>) {
      aggregate = [owner = *this](forge::db::core::record_range range) mutable
         -> boost::asio::awaitable<index_aggregate_result> {
         co_return co_await detail::query_snapshot_aggregate<Object, Tag>(access{owner}, std::move(range));
      };
      ranks = [owner = *this](forge::db::core::record_range range) mutable
         -> boost::asio::awaitable<index_rank_result> {
         co_return co_await detail::query_snapshot_ranks<Object, Tag>(access{owner}, std::move(range));
      };
      nth = [owner = *this](std::uint64_t position) mutable
         -> boost::asio::awaitable<std::optional<typename Object::value_type>> {
         co_return co_await detail::nth_snapshot_object<Object, Tag>(access{owner}, position);
      };
      exact_rank = [owner = *this](const typename Object::value_type& value) mutable
         -> boost::asio::awaitable<std::optional<std::uint64_t>> {
         co_return co_await detail::query_snapshot_exact_rank<Object, Tag>(access{owner}, value);
      };
   }
   return index_view<Object, Tag>{
      [owner = *this](forge::db::core::record_range range, forge::db::core::page_request request) mutable
         -> boost::asio::awaitable<object_page<typename Object::value_type>> {
         co_return co_await detail::page_snapshot_objects<Object, Tag>(access{owner}, std::move(range), std::move(request));
      },
      [owner = *this]() mutable -> index_page_query<typename Object::value_type> {
         return [owner](forge::db::core::record_range range, forge::db::core::page_request request) mutable
                   -> boost::asio::awaitable<object_page<typename Object::value_type>> {
            co_return co_await detail::page_snapshot_objects<Object, Tag>(
               access{owner},
               std::move(range),
               std::move(request));
         };
      }, std::move(aggregate), std::move(ranks), std::move(nth), std::move(exact_rank)};
}

} // namespace forge::db::object
