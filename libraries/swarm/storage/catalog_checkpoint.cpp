module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>

module forge.swarm.storage.catalog;

import forge.db.core.driver;
import forge.db.object.exceptions;
import forge.db.object.index;
import forge.db.object.snapshot;
import forge.db.object.transaction;

#include "details/catalog_impl.hxx"
#include "details/catalog_checkpoint.hxx"

namespace forge::swarm::storage {

catalog::checkpoint::checkpoint(std::shared_ptr<impl> owner, db::content row)
   : _owner(std::move(owner)),
     _binding(std::make_shared<const binding>(binding{
        row.id, row.swarm, std::move(row.locator), std::move(row.manifest)})) {}
catalog::checkpoint::~checkpoint() = default;

void catalog::checkpoint::validate_binding(const db::content& row, const binding& expected) {
   if (row.id != expected.row || row.swarm != expected.swarm || row.locator != expected.locator ||
       row.manifest != expected.manifest) {
      FORGE_THROW_EXCEPTION(exceptions::missing_metadata, "checkpoint binding no longer identifies the same catalog content");
   }
}

boost::asio::awaitable<std::shared_ptr<core::checkpoint_store>> catalog::impl::bind_checkpoint(
   std::shared_ptr<impl> self, core::swarm_id id, std::string locator, std::stop_token token) {
   locator = co_await self->config.cpu.execute({"swarm.catalog.locator", token},
      [locator = std::move(locator)]() mutable { validate_locator(locator); return std::move(locator); });
   co_await check_stop(token);
   auto read = co_await self->objects.begin_read();
   co_await require_schema(read);
   auto row = co_await self->lookup(read, id, token);
   if (!row || row->locator != locator) {
      FORGE_THROW_EXCEPTION(exceptions::missing_metadata, "checkpoint content or locator is missing");
   }
   co_await check_stop(token);
   co_return std::make_shared<checkpoint>(std::move(self), std::move(*row));
}

boost::asio::awaitable<std::optional<core::resume_state>> catalog::checkpoint::async_load(
   core::swarm_id id, std::stop_token token) {
   return impl::dispatch<std::optional<core::resume_state>>(impl::admission{_owner}, token,
      [expected = _binding, id](std::shared_ptr<impl> self, std::stop_token stop) {
         return load(std::move(self), expected, id, stop);
      });
}

boost::asio::awaitable<void> catalog::checkpoint::async_save(core::resume_state state, std::stop_token token) {
   return impl::dispatch<void>(impl::admission{_owner}, token,
      [expected = _binding, state = std::move(state)](std::shared_ptr<impl> self, std::stop_token stop) mutable {
         return save(std::move(self), expected, std::move(state), stop);
      });
}

boost::asio::awaitable<std::optional<core::resume_state>> catalog::checkpoint::load(
   std::shared_ptr<impl> self, std::shared_ptr<const binding> expected,
   core::swarm_id id, std::stop_token token) {
   if (id != expected->swarm) { FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "checkpoint requested for a different identity"); }
   co_await impl::check_stop(token);
   auto read = co_await self->objects.begin_read();
   co_await impl::require_schema(read);
   co_await impl::check_stop(token);
   auto row = co_await read.find(expected->row);
   if (!row) { FORGE_THROW_EXCEPTION(exceptions::missing_metadata, "checkpoint catalog content was erased"); }
   row = co_await self->validate(std::move(*row), token);
   row = co_await self->config.cpu.execute({"swarm.catalog.binding", token},
      [row = std::move(*row), expected]() mutable {
         validate_binding(row, *expected);
         return std::move(row);
      });
   co_await impl::check_stop(token);
   if (row->checkpoint) {
      // A previous commit may have outlived an unsuccessful flush/observer acknowledgement.
      // Do not expose that generation as durable until this owned driver's fence succeeds.
      const auto executor = co_await boost::asio::this_coro::executor;
      co_await boost::asio::co_spawn(executor, self->objects.driver()->async_flush(true),
         boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
   }
   co_await impl::check_stop(token);
   co_return std::move(row->checkpoint);
}

boost::asio::awaitable<void> catalog::checkpoint::save(
   std::shared_ptr<impl> self, std::shared_ptr<const binding> expected,
   core::resume_state state, std::stop_token token) {
   state = co_await self->config.cpu.execute({"swarm.catalog.checkpoint", token},
      [expected, state = std::move(state)]() mutable {
         core::validate_checkpoint(state, expected->manifest);
         return std::move(state);
      });
   co_await impl::mutate<void>(self, token,
      [self, expected, token, state = std::move(state)](forge::db::object::transaction& tx) mutable
         -> boost::asio::awaitable<void> {
         co_await impl::check_stop(token);
         // Re-read under ObjectDB's writer gate; never replace a row captured by the handle.
         auto row = co_await tx.find(expected->row);
         if (!row) { FORGE_THROW_EXCEPTION(exceptions::missing_metadata, "checkpoint catalog content was erased"); }
         row = co_await self->validate(std::move(*row), token);
         auto checked = co_await self->config.cpu.execute({"swarm.catalog.checkpoint.compare", token},
            [expected, row = std::move(*row), state = std::move(state)]() mutable {
               validate_binding(row, *expected);
               if (row.checkpoint && (state.generation < row.checkpoint->generation ||
                   (state.generation == row.checkpoint->generation && state != *row.checkpoint))) {
                  FORGE_THROW_EXCEPTION(exceptions::stale_checkpoint, "checkpoint generation is stale or conflicting");
               }
               const bool identical = row.checkpoint && state == *row.checkpoint;
               return std::pair{std::move(state), identical};
            });
         co_await impl::check_stop(token);
         if (!checked.second) {
            co_await tx.modify(expected->row, [state = std::move(checked.first)](db::content& current) mutable {
               current.checkpoint = std::move(state);
            });
         }
         // Exact retries still commit/flush: the earlier acknowledgement may have failed.
      });
}

} // namespace forge::swarm::storage
