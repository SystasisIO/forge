module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <utility>

module forge.db.revision.transaction;

import forge.db.revision.exceptions;
import forge.log.logger;

namespace forge::db::revision {

scope::scope(revision_id_t candidate) : candidate_{candidate} {}

revision_id_t scope::id() const {
   if (!candidate_) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db revision scope is closed");
   }
   return *candidate_;
}

transaction::transaction(forge::db::core::transaction active, scope joined)
    : active_{std::move(active)}, scope_{std::move(joined)} {}

forge::db::core::transaction& transaction::db_transaction() {
   if (!active_ || !active_->active()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "db revision transaction is closed");
   }
   return *active_;
}

revision_id_t transaction::id() const {
   return scope_.id();
}

boost::asio::awaitable<void> transaction::commit() {
   if (active_) {
      const auto was_active = active_->active();
      co_await active_->commit();
      if (was_active) {
         dlog("forge.db.revision", "revision committed",
              ("event", "revision.committed")("revision", scope_.id()));
      }
   }
}

boost::asio::awaitable<void> transaction::rollback() {
   if (active_) {
      co_await active_->rollback();
   }
}

} // namespace forge::db::revision
