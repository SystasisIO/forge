module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

export module forge.swarm.storage.catalog;

export import forge.swarm.core.store;
export import forge.swarm.db.content;
export import forge.swarm.db.schema;
export import forge.swarm.storage.exceptions;
export import forge.asio.compute;
export import forge.db.object.store;
export import forge.db.object.snapshot;
export import forge.db.object.transaction;
export import forge.db.object.index;
export import forge.db.core.record;

export namespace forge::swarm::storage {

class catalog {
 public:
   class access;
   struct options {
      forge::asio::compute::executor cpu;
      boost::asio::any_io_executor database;
      std::uint32_t max_inflight = 8;
   };
   struct job_settings {
      db::job_state state = db::job_state::paused;
      std::int32_t priority = 0;
      std::int64_t expires_at = INT64_MAX;
   };

   static void register_objects(forge::db::object::store&);
   template <typename Objects>
      requires requires(Objects& objects) {
         { objects.template register_object<db::schema_index>() } -> std::same_as<void>;
         { objects.template register_object<db::content_index>() } -> std::same_as<void>;
      }
   static void register_objects(Objects& objects) {
      objects.template register_object<db::schema_index>();
      objects.template register_object<db::content_index>();
   }
   [[nodiscard]] static boost::asio::awaitable<std::shared_ptr<catalog>> async_open(
      forge::db::object::store, options, std::stop_token = {});
   [[nodiscard]] static boost::asio::awaitable<std::shared_ptr<catalog>> async_open(
      std::shared_ptr<access>, options, std::stop_token = {});
   ~catalog();
   catalog(const catalog&) = delete;
   catalog& operator=(const catalog&) = delete;

   boost::asio::awaitable<db::content> async_create(
      core::descriptor, std::string locator, job_settings, std::stop_token = {});
   boost::asio::awaitable<std::optional<db::content>> async_find(core::swarm_id, std::stop_token = {});
   boost::asio::awaitable<void> async_update(core::swarm_id, job_settings, std::stop_token = {});
   boost::asio::awaitable<void> async_erase(core::swarm_id, std::stop_token = {});
   boost::asio::awaitable<forge::db::object::object_page<db::content>> async_by_state(
      db::job_state, forge::db::core::page_request = {.limit = 16}, std::stop_token = {});
   boost::asio::awaitable<forge::db::object::object_page<db::content>> async_expiring_before(
      std::int64_t exclusive, forge::db::core::page_request = {.limit = 16}, std::stop_token = {});
   boost::asio::awaitable<std::shared_ptr<core::checkpoint_store>> async_checkpoint_store(
      core::swarm_id, std::string locator, std::stop_token = {});
   // Closes admission immediately; joins accepted work without closing the DB.
   // Await or destroy accepted unstarted operations before awaiting this drain.
   boost::asio::awaitable<void> async_close();

 private:
   struct impl;
   class checkpoint;
   explicit catalog(std::shared_ptr<impl>);
   std::shared_ptr<impl> _impl;
};

class catalog::access {
 public:
   virtual ~access() = default;
   [[nodiscard]] virtual forge::db::object::store::options runtime_options() const = 0;
   virtual boost::asio::awaitable<forge::db::object::snapshot> begin_read() = 0;
   virtual boost::asio::awaitable<forge::db::object::transaction> begin_transaction() = 0;
   // Durable synchronous flush of the SAME store used by these transactions.
   virtual boost::asio::awaitable<void> flush() = 0;
};

} // namespace forge::swarm::storage
