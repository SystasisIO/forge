module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

export module forge.db.object.store;

import forge.db.ids.object_id;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.object.exceptions;
import forge.db.object.header;
import forge.db.object.hooks;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.snapshot;
import forge.db.object.transaction;

export namespace forge::db::object {

enum class write_policy : std::uint8_t {
   single_writer,
   backend,
};

enum class id_allocation_policy : std::uint8_t {
   monotonic,
   transactional,
};

class store {
 private:
   struct impl;

   explicit store(std::shared_ptr<impl> implementation);

 public:
   struct config {
      forge::db::core::family family{"objectdb"};
   };

   struct options {
      write_policy writes = write_policy::single_writer;
      id_allocation_policy id_allocation = id_allocation_policy::monotonic;
   };

   struct allocation {
      forge::db::ids::object_id type;
      std::uint64_t next = 0;
   };

   static boost::asio::awaitable<store> open(std::shared_ptr<forge::db::core::driver> value);
   static boost::asio::awaitable<store> open(std::shared_ptr<forge::db::core::driver> value, options runtime);
   static boost::asio::awaitable<store> open(std::shared_ptr<forge::db::core::driver> value, config settings);
   static boost::asio::awaitable<store> open(std::shared_ptr<forge::db::core::driver> value, config settings,
                                             options runtime);
   static boost::asio::awaitable<store> open_existing(std::shared_ptr<forge::db::core::driver> value, config settings,
                                                      options runtime);

   [[nodiscard]] forge::db::object::header header() const noexcept;
   [[nodiscard]] std::shared_ptr<forge::db::core::driver> driver() const noexcept;
   [[nodiscard]] forge::db::core::family family() const;
   [[nodiscard]] options runtime_options() const noexcept;

   template <application_object_model Object> void register_object();

   template <system_object_model Object> void register_system_object();

   void add_interceptor(std::shared_ptr<interceptor> value);
   void add_observer(std::shared_ptr<observer> value);

   // Requires this store's live single-writer Object transaction. No writes.
   boost::asio::awaitable<void> require_pristine(transaction& active);

   // Fresh header-only namespace; exact registered application counters only.
   // A loading failure prevents commit until the entire transaction rolls back.
   boost::asio::awaitable<void> restore_allocations(transaction& active, std::vector<allocation> values);

   boost::asio::awaitable<transaction> begin_transaction();
   boost::asio::awaitable<snapshot> begin_read();
   [[nodiscard]] snapshot join(const forge::db::core::snapshot& active);
   boost::asio::awaitable<transaction> join(forge::db::core::transaction& active);
   boost::asio::awaitable<transaction> join(transaction& active);

   template <typename SharedTransaction>
      requires requires(SharedTransaction& active) {
         { active.db_transaction() } -> std::same_as<forge::db::core::transaction&>;
      }
   boost::asio::awaitable<transaction> join(SharedTransaction& active) {
      co_return co_await join(active.db_transaction());
   }

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

 private:
   friend class transaction;
   friend class snapshot;

   static boost::asio::awaitable<store> open_impl(std::shared_ptr<forge::db::core::driver> value, config settings,
                                                  options runtime, bool initialize);

   void register_object_type(forge::db::ids::object_id type, std::type_index model);
   void register_system_object_type(forge::db::ids::object_id type, std::type_index model);
   void ensure_registered_type(forge::db::ids::object_id type, std::type_index model) const;

   std::shared_ptr<impl> impl_;
};

template <application_object_model Object> void store::register_object() {
   register_object_type(object_id_of<Object>::value, std::type_index{typeid(Object)});
}

template <system_object_model Object> void store::register_system_object() {
   register_system_object_type(object_id_of<Object>::value, std::type_index{typeid(Object)});
}

template <forge::db::ids::typed_id_like Id>
boost::asio::awaitable<typename index_for_id_t<Id>::value_type> store::get(Id id) {
   const auto value = co_await find(id);
   if (!value.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::not_found, "db object was not found");
   }
   co_return *value;
}

template <forge::db::ids::typed_id_like Id>
boost::asio::awaitable<std::optional<typename index_for_id_t<Id>::value_type>> store::find(Id id) {
   auto read = co_await begin_read();
   co_return co_await read.find(id);
}

template <object_model Object>
boost::asio::awaitable<typename Object::value_type> store::get(forge::db::ids::object_id id) {
   const auto value = co_await find<Object>(id);
   if (!value.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::not_found, "db object was not found");
   }
   co_return *value;
}

template <object_model Object>
boost::asio::awaitable<std::optional<typename Object::value_type>> store::find(forge::db::ids::object_id id) {
   auto read = co_await begin_read();
   co_return co_await read.template find<Object>(id);
}

template <application_object_value Value> boost::asio::awaitable<void> store::insert(Value value) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   try {
      co_await active.insert(std::move(value));
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
}

template <application_object_value Value, typename Fn>
   requires std::default_initializable<Value> && std::invocable<Fn&, Value&>
boost::asio::awaitable<Value> store::create(Fn&& fn) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   auto created = std::optional<Value>{};
   try {
      created = co_await active.template create<Value>(std::forward<Fn>(fn));
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
   co_return std::move(*created);
}

template <application_object_value Value> boost::asio::awaitable<void> store::replace(Value value) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   try {
      co_await active.replace(std::move(value));
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
}

template <forge::db::ids::typed_id_like Id, typename Fn>
   requires application_object_model<index_for_id_t<Id>>
boost::asio::awaitable<void> store::modify(Id id, Fn&& fn) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   try {
      co_await active.modify(id, std::forward<Fn>(fn));
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
}

template <forge::db::ids::typed_id_like Id>
   requires application_object_model<index_for_id_t<Id>>
boost::asio::awaitable<void> store::erase(Id id) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   try {
      co_await active.erase(id);
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
}

template <application_object_model Object> boost::asio::awaitable<void> store::erase(forge::db::ids::object_id id) {
   auto active = co_await begin_transaction();
   auto error = std::exception_ptr{};
   try {
      co_await active.template erase<Object>(id);
      co_await active.commit();
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await active.rollback();
      std::rethrow_exception(error);
   }
}

template <object_model Object, typename Tag> [[nodiscard]] index_view<Object, Tag> store::index() const {
   ensure_registered_type(object_id_of<Object>::value, std::type_index{typeid(Object)});

   auto aggregate = index_aggregate_query{};
   auto ranks = index_rank_query{};
   auto nth = index_nth_query<typename Object::value_type>{};
   auto exact_rank = index_exact_rank_query<typename Object::value_type>{};
   if constexpr (ranked_index<index_by_tag<Object, Tag>>) {
      aggregate = [owner = *this](
                      forge::db::core::record_range range) mutable -> boost::asio::awaitable<index_aggregate_result> {
         auto read = co_await owner.begin_read();
         auto view = read.template index<Object, Tag>();
         co_return co_await view.query_aggregate(std::move(range));
      };
      ranks = [owner =
                   *this](forge::db::core::record_range range) mutable -> boost::asio::awaitable<index_rank_result> {
         auto read = co_await owner.begin_read();
         auto view = read.template index<Object, Tag>();
         co_return co_await view.query_rank_result(std::move(range));
      };
      nth = [owner = *this](
                std::uint64_t position) mutable -> boost::asio::awaitable<std::optional<typename Object::value_type>> {
         auto read = co_await owner.begin_read();
         co_return co_await read.template index<Object, Tag>().nth(position);
      };
      exact_rank = [owner = *this](const typename Object::value_type& value) mutable
          -> boost::asio::awaitable<std::optional<std::uint64_t>> {
         auto read = co_await owner.begin_read();
         co_return co_await read.template index<Object, Tag>().query_exact_rank(value);
      };
   }

   return index_view<Object, Tag>{
       [owner = *this](forge::db::core::record_range range, forge::db::core::page_request request) mutable
           -> boost::asio::awaitable<object_page<typename Object::value_type>> {
          auto read = co_await owner.begin_read();
          auto view = read.template index<Object, Tag>();
          co_return co_await view.page(std::move(range), std::move(request));
       },
       [owner = *this]() mutable -> index_page_query<typename Object::value_type> {
          auto active = std::make_shared<std::optional<snapshot>>();
          return [owner, active](forge::db::core::record_range range, forge::db::core::page_request request) mutable
                     -> boost::asio::awaitable<object_page<typename Object::value_type>> {
             if (!active->has_value()) {
                active->emplace(co_await owner.begin_read());
             }
             auto view = active->value().template index<Object, Tag>();
             co_return co_await view.page(std::move(range), std::move(request));
          };
       },
       std::move(aggregate),
       std::move(ranks),
       std::move(nth),
       std::move(exact_rank)};
}

} // namespace forge::db::object
