module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
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
export import forge.db.object.index;
export import forge.db.core.record;

export namespace forge::swarm::storage {

class catalog {
 public:
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
   [[nodiscard]] static boost::asio::awaitable<std::shared_ptr<catalog>> async_open(
      forge::db::object::store, options, std::stop_token = {});
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

 private:
   struct impl;
   class checkpoint;
   explicit catalog(std::shared_ptr<impl>);
   std::shared_ptr<impl> _impl;
};

} // namespace forge::swarm::storage
