#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <cstdint>
#include <vector>

namespace forge::db::core {

struct transaction::impl {
   enum class phase : std::uint8_t {
      active,
      preparing,
      prepared,
      rollback_only,
      closed,
   };

   explicit impl(std::unique_ptr<session> active_value, boost::asio::any_io_executor executor,
                 std::shared_ptr<const void> origin_value) noexcept;
   ~impl();

   void rollback_on_drop() noexcept;
   boost::asio::awaitable<void> run_after_rollback();

   std::unique_ptr<session> active;
   boost::asio::any_io_executor cleanup_executor;
   std::shared_ptr<const void> origin;
   std::vector<before_commit_fn> before_commit_hooks;
   std::vector<after_commit_fn> after_commit_hooks;
   std::vector<after_rollback_fn> after_rollback_hooks;
   std::vector<std::shared_ptr<transaction_participant>> participants;
   std::vector<savepoint_id_t> savepoints;
   savepoint_id_t next_savepoint = 1;
   phase current = phase::active;
   bool mutation_started = false;
   bool prewrite_locks_prepared = false;
   bool before_commit_started = false;
   bool commit_in_progress = false;
   bool closed = false;
   bool committed = false;
};

} // namespace forge::db::core
