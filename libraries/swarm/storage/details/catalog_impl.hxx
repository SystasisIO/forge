#pragma once

namespace forge::swarm::storage {

struct catalog::impl {
   struct admission {
      explicit admission(std::shared_ptr<impl>);
      ~admission();
      admission(admission&&) noexcept;
      admission(const admission&) = delete;
      admission& operator=(const admission&) = delete;
      admission& operator=(admission&&) = delete;
      std::shared_ptr<impl> owner;
   };

   impl(std::shared_ptr<access>, options);
   std::shared_ptr<access> objects;
   const options config;
   static constexpr std::uint32_t closed_bit = std::uint32_t{1} << 31U;
   static constexpr std::uint32_t retiring_bit = std::uint32_t{1} << 30U;
   static constexpr std::uint32_t retired_bit = std::uint32_t{1} << 29U;
   static constexpr std::uint32_t count_mask = retired_bit - 1U;
   std::atomic<std::uint32_t> inflight{0};
   forge::asio::notification changed;

   static void check_token(std::stop_token);
   static boost::asio::awaitable<void> check_stop(std::stop_token);
   static void validate_state(db::job_state);
   static void validate_settings(const job_settings&);
   static void validate_locator(const std::string&);
   static void validate_row(const db::content&);
   static void validate_page(const forge::db::core::page_request&);
   boost::asio::awaitable<db::content> validate(db::content, std::stop_token);
   static boost::asio::awaitable<void> commit_flush(impl&, forge::db::object::transaction&);

   template <typename T, typename Work>
   static boost::asio::awaitable<T> on_database(admission active, std::stop_token token, Work work) {
      // This is a separate co_spawn root, not the caller's coroutine/filter.
      // Keep native cancellation connected to dependency waits, but permit joined cleanup after cancellation.
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await check_stop(token);
      if constexpr (std::is_void_v<T>) {
         co_await std::invoke(work, active.owner, token);
         co_await check_stop(token);
      } else {
         auto result = co_await std::invoke(work, active.owner, token);
         co_await check_stop(token);
         co_return result;
      }
   }

   template <typename T, typename Work>
   static boost::asio::awaitable<T> dispatch(admission active, std::stop_token token, Work work) {
      const auto executor = active.owner->config.database;
      // Default associated cancellation propagates to this structured DB operation.
      if constexpr (std::is_void_v<T>) {
         co_await boost::asio::co_spawn(executor,
            on_database<T>(std::move(active), token, std::move(work)), boost::asio::use_awaitable);
         co_await check_stop(token);
      } else {
         auto result = co_await boost::asio::co_spawn(executor,
            on_database<T>(std::move(active), token, std::move(work)), boost::asio::use_awaitable);
         co_await check_stop(token);
         co_return result;
      }
   }

   template <typename Reader>
   static boost::asio::awaitable<void> require_schema(Reader& read) {
      const auto rows = co_await read.template index<db::schema_index, db::by_schema_id>()
         .lower_bound(db::schema_state::id_t{0}).page({.limit = 2});
      if (rows.items.size() != 1U || rows.next || rows.items.front().id != db::schema_id ||
          rows.items.front().version != db::schema_version) {
         FORGE_THROW_EXCEPTION(exceptions::schema_mismatch, "catalog requires exactly the current schema singleton");
      }
   }

   template <typename Reader>
   boost::asio::awaitable<std::optional<db::content>> lookup(
      Reader& read, core::swarm_id id, std::stop_token token) {
      co_await check_stop(token);
      auto row = co_await read.template index<db::content_index, db::by_swarm>().find(id.value);
      if (row) {
         row = co_await validate(std::move(*row), token);
         if (row->swarm != id) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "catalog identity index points to a different content row");
         }
      }
      co_await check_stop(token);
      co_return row;
   }

   template <typename T, typename Work>
   static boost::asio::awaitable<T> mutate(
      std::shared_ptr<impl> self, std::stop_token token, Work work, bool bootstrap = false) {
      co_await check_stop(token);
      auto tx = co_await self->objects->begin_transaction();
      auto* backend = &tx.db_transaction();
      using result_type = std::conditional_t<std::is_void_v<T>, bool, T>;
      auto result = std::optional<result_type>{};
      auto failure = std::exception_ptr{};
      try {
         co_await check_stop(token);
         if (!bootstrap) co_await require_schema(tx);
         co_await check_stop(token);
         if constexpr (std::is_void_v<T>) {
            co_await std::invoke(work, tx);
            result = true;
         } else {
            result = co_await std::invoke(work, tx);
         }
         co_await check_stop(token);
         const auto executor = co_await boost::asio::this_coro::executor;
         co_await boost::asio::co_spawn(executor, commit_flush(*self, tx),
            boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
      } catch (const forge::db::object::exceptions::duplicate_object&) {
         failure = std::make_exception_ptr(exceptions::conflict{"catalog identity or locator is already present"});
      } catch (...) {
         failure = std::current_exception();
      }
      if (failure) {
         if (backend->active()) {
            const auto executor = co_await boost::asio::this_coro::executor;
            co_await boost::asio::co_spawn(executor, tx.rollback(),
               boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
         }
         std::rethrow_exception(failure);
      }
      co_await check_stop(token);
      if constexpr (!std::is_void_v<T>) co_return std::move(*result);
   }

   static boost::asio::awaitable<std::shared_ptr<catalog>> open(std::shared_ptr<impl>, std::stop_token);
   static boost::asio::awaitable<void> close(std::shared_ptr<impl>);
   static boost::asio::awaitable<db::content> create(
      std::shared_ptr<impl>, core::descriptor, std::string, job_settings, std::stop_token);
   static boost::asio::awaitable<std::optional<db::content>> find(
      std::shared_ptr<impl>, core::swarm_id, std::stop_token);
   static boost::asio::awaitable<void> update(
      std::shared_ptr<impl>, core::swarm_id, job_settings, std::stop_token);
   static boost::asio::awaitable<void> erase(std::shared_ptr<impl>, core::swarm_id, std::stop_token);
   static boost::asio::awaitable<forge::db::object::object_page<db::content>> by_state(
      std::shared_ptr<impl>, db::job_state, forge::db::core::page_request, std::stop_token);
   static boost::asio::awaitable<forge::db::object::object_page<db::content>> expiring_before(
      std::shared_ptr<impl>, std::int64_t, forge::db::core::page_request, std::stop_token);
   static boost::asio::awaitable<std::shared_ptr<core::checkpoint_store>> bind_checkpoint(
      std::shared_ptr<impl>, core::swarm_id, std::string, std::stop_token);
};

} // namespace forge::swarm::storage
