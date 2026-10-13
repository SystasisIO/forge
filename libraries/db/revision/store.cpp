module;

#include <forge/log/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <exception>
#include <memory>
#include <utility>

module forge.db.revision.store;

import forge.db.core.record;
import forge.log.logger;
import forge.db.object.system;
import forge.db.revision.exceptions;

#include "details/store_impl.hxx"

namespace forge::db::revision {

store::store(std::shared_ptr<impl> implementation)
    : impl_{std::move(implementation)} {}

boost::asio::awaitable<store>
store::open(std::shared_ptr<forge::db::core::driver> driver,
            forge::db::object::store objects) {
   auto implementation = std::make_shared<impl>(std::move(driver), std::move(objects));
   try {
      co_await implementation->initialize();
   } catch (const exceptions::corrupt_state&) {
      elog("forge.db.revision", "revision initialization rejected",
           ("event", "initialization.rejected")("reason", "corrupt_state"));
      throw;
   }
   dlog("forge.db.revision", "revision store initialized", ("event", "store.initialized"));
   co_return store{std::move(implementation)};
}

boost::asio::awaitable<scope> store::join(forge::db::core::transaction& active) {
   impl_->require_joinable(active);
   if (!forge::db::object::system::access::joined(impl_->objects, active)) {
      auto objects = co_await impl_->objects.join(active);
      static_cast<void>(objects);
   }
   co_return scope{co_await impl_->join(active)};
}

boost::asio::awaitable<scope> store::join(forge::db::object::transaction& active) {
   impl_->require_joinable(active.db_transaction());
   auto objects = co_await impl_->objects.join(active);
   static_cast<void>(objects);
   co_return scope{co_await impl_->join(active.db_transaction())};
}

boost::asio::awaitable<transaction> store::begin_transaction() {
   auto active = co_await impl_->driver->begin_transaction();
   auto error = std::exception_ptr{};
   auto joined = scope{};
   try {
      joined = co_await join(active);
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      try {
         co_await active.rollback();
      } catch (...) {
      }
      std::rethrow_exception(error);
   }
   co_return transaction{std::move(active), std::move(joined)};
}

boost::asio::awaitable<void>
store::revert(forge::db::core::transaction& active, revision_id_t expected_head) {
   impl_->require_control(active);
   if (!forge::db::object::system::access::joined(impl_->objects, active)) {
      auto objects = co_await impl_->objects.join(active);
      static_cast<void>(objects);
   }
   co_await impl_->revert(active, expected_head);
}

boost::asio::awaitable<void>
store::revert(forge::db::object::transaction& active, revision_id_t expected_head) {
   impl_->require_control(active.db_transaction());
   auto objects = co_await impl_->objects.join(active);
   static_cast<void>(objects);
   co_await impl_->revert(active.db_transaction(), expected_head);
}

boost::asio::awaitable<prune_result>
store::prune_through(forge::db::core::transaction& active,
                     revision_id_t inclusive_boundary,
                     prune_options options) {
   impl_->require_control(active);
   if (!forge::db::object::system::access::joined(impl_->objects, active)) {
      auto objects = co_await impl_->objects.join(active);
      static_cast<void>(objects);
   }
   co_return co_await impl_->prune_through(active, inclusive_boundary, options);
}

boost::asio::awaitable<prune_result>
store::prune_through(forge::db::object::transaction& active,
                     revision_id_t inclusive_boundary,
                     prune_options options) {
   impl_->require_control(active.db_transaction());
   auto objects = co_await impl_->objects.join(active);
   static_cast<void>(objects);
   co_return co_await impl_->prune_through(
      active.db_transaction(), inclusive_boundary, options);
}

} // namespace forge::db::revision
