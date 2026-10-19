module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
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
import forge.swarm.core.exceptions;

#include "details/catalog_impl.hxx"

namespace forge::swarm::storage {

catalog::impl::impl(forge::db::object::store value, options settings)
   : objects(std::move(value)), config(std::move(settings)) {
   if (!config.cpu.valid() || !config.database || config.max_inflight == 0U || config.max_inflight > 32U) {
      throw exceptions::invalid_options{"catalog requires CPU/DB executors and max_inflight in 1..32"};
   }
   const auto runtime = objects.runtime_options();
   if (runtime.writes != forge::db::object::write_policy::single_writer ||
       runtime.id_allocation != forge::db::object::id_allocation_policy::monotonic) {
      throw exceptions::invalid_options{"catalog requires single-writer ObjectDB with monotonic IDs"};
   }
}

catalog::impl::admission::admission(std::shared_ptr<impl> value) : owner(std::move(value)) {
   auto count = owner->inflight.load(std::memory_order_relaxed);
   do {
      if (count >= owner->config.max_inflight) {
         throw core::exceptions::storage_busy{"catalog operation limit is exhausted"};
      }
   } while (!owner->inflight.compare_exchange_weak(count, count + 1U, std::memory_order_relaxed));
}

catalog::impl::admission::admission(admission&& other) noexcept : owner(std::move(other.owner)) {}
catalog::impl::admission::~admission() {
   if (owner) owner->inflight.fetch_sub(1U, std::memory_order_relaxed);
}

void catalog::impl::check_token(std::stop_token token) {
   if (token.stop_requested()) throw forge::asio::exceptions::canceled{"catalog operation canceled"};
}

boost::asio::awaitable<void> catalog::impl::check_stop(std::stop_token token) {
   check_token(token);
   const auto state = co_await boost::asio::this_coro::cancellation_state;
   if (state.cancelled() != boost::asio::cancellation_type::none) {
      throw forge::asio::exceptions::canceled{"catalog operation canceled"};
   }
}

void catalog::impl::validate_state(db::job_state state) {
   switch (state) {
      case db::job_state::paused:
      case db::job_state::downloading:
      case db::job_state::seeding:
      case db::job_state::failed: return;
   }
   throw exceptions::invalid_metadata{"catalog job state is invalid"};
}

void catalog::impl::validate_settings(const job_settings& value) {
   validate_state(value.state);
   if (value.expires_at < 0) throw exceptions::invalid_metadata{"catalog expiry must be nonnegative Unix seconds"};
}

void catalog::impl::validate_locator(const std::string& value) {
   if (value.empty() || value.size() > 4096U || value.find('\0') != std::string::npos) {
      throw exceptions::invalid_metadata{"catalog locator must contain 1..4096 non-NUL bytes"};
   }
}

void catalog::impl::validate_row(const db::content& row) {
   validate_locator(row.locator);
   validate_settings({row.state, row.priority, row.expires_at});
   try {
      if (core::calculate_swarm_id(row.manifest) != row.swarm) {
         throw exceptions::invalid_metadata{"catalog descriptor does not match its identity"};
      }
      if (row.checkpoint) core::validate_checkpoint(*row.checkpoint, row.manifest);
   } catch (const forge::exceptions::context_error&) {
      throw exceptions::invalid_metadata{"catalog descriptor or checkpoint is inconsistent"};
   }
}

void catalog::impl::validate_page(const forge::db::core::page_request& page) {
   if (page.limit == 0U || page.limit > 32U) {
      throw exceptions::invalid_options{"catalog pages must request 1..32 rows"};
   }
}

boost::asio::awaitable<db::content> catalog::impl::validate(db::content row, std::stop_token token) {
   co_await check_stop(token);
   auto result = co_await config.cpu.execute({"swarm.catalog.validate", token},
      [row = std::move(row)](forge::asio::compute::context& context) mutable {
         context.throw_if_stop_requested();
         validate_row(row);
         context.throw_if_stop_requested();
         return std::move(row);
      });
   co_await check_stop(token);
   co_return result;
}

boost::asio::awaitable<void> catalog::impl::commit_flush(impl& self, forge::db::object::transaction& tx) {
   auto* backend = &tx.db_transaction();
   auto failure = std::exception_ptr{};
   try { co_await tx.commit(); }
   catch (...) { failure = std::current_exception(); }
   if (failure && backend->active()) std::rethrow_exception(failure);
   // An after-commit observer can fail after the actual commit. Restore durability anyway;
   // a flush failure is ambiguous committed state, not a transaction that can be rolled back.
   co_await self.objects.driver()->async_flush(true);
   if (failure) std::rethrow_exception(failure);
}

boost::asio::awaitable<std::shared_ptr<catalog>> catalog::impl::open(
   std::shared_ptr<impl> self, std::stop_token token) {
   // Descriptor checks only: registration remains an explicit composition operation.
   static_cast<void>(self->objects.index<db::schema_index, db::by_schema_id>());
   static_cast<void>(self->objects.index<db::content_index, db::by_id>());
   co_await mutate<void>(self, token, [token](forge::db::object::transaction& tx)
      -> boost::asio::awaitable<void> {
      const auto schema = co_await tx.index<db::schema_index, db::by_schema_id>()
         .lower_bound(db::schema_state::id_t{0}).page({.limit = 2});
      co_await check_stop(token);
      if (!schema.items.empty() || schema.next) {
         co_await require_schema(tx);
         co_return;
      }
      const auto contents = co_await tx.index<db::content_index, db::by_id>()
         .lower_bound(db::content::id_t{0}).page({.limit = 1});
      if (!contents.items.empty() || contents.next) {
         throw exceptions::schema_mismatch{"catalog content exists without its schema singleton"};
      }
      co_await check_stop(token);
      co_await tx.insert(db::schema_state{});
   }, true);
   co_return std::shared_ptr<catalog>{new catalog{std::move(self)}};
}

boost::asio::awaitable<db::content> catalog::impl::create(
   std::shared_ptr<impl> self, core::descriptor manifest, std::string locator,
   job_settings settings, std::stop_token token) {
   auto row = co_await self->config.cpu.execute({"swarm.catalog.create", token},
      [manifest = std::move(manifest), locator = std::move(locator), settings](
         forge::asio::compute::context& context) mutable {
         context.throw_if_stop_requested();
         validate_locator(locator);
         validate_settings(settings);
         auto result = db::content{};
         result.swarm = core::calculate_swarm_id(manifest);
         result.manifest = std::move(manifest);
         result.locator = std::move(locator);
         result.state = settings.state;
         result.priority = settings.priority;
         result.expires_at = settings.expires_at;
         context.throw_if_stop_requested();
         return result;
      });
   co_return co_await mutate<db::content>(self, token,
      [self, token, row = std::move(row)](forge::db::object::transaction& tx) mutable
         -> boost::asio::awaitable<db::content> {
         if (co_await self->lookup(tx, row.swarm, token)) {
            throw exceptions::conflict{"catalog identity is already present"};
         }
         auto existing = co_await tx.index<db::content_index, db::by_locator>().find(row.locator_key());
         if (existing) {
            static_cast<void>(co_await self->validate(std::move(*existing), token));
            throw exceptions::conflict{"catalog locator is already present"};
         }
         co_await check_stop(token);
         co_return co_await tx.create<db::content>([row = std::move(row)](db::content& value) mutable {
            const auto id = value.id;
            value = std::move(row);
            value.id = id;
         });
      });
}

boost::asio::awaitable<std::optional<db::content>> catalog::impl::find(
   std::shared_ptr<impl> self, core::swarm_id id, std::stop_token token) {
   co_await check_stop(token);
   auto read = co_await self->objects.begin_read();
   co_await require_schema(read);
   co_return co_await self->lookup(read, id, token);
}

boost::asio::awaitable<void> catalog::impl::update(
   std::shared_ptr<impl> self, core::swarm_id id, job_settings settings, std::stop_token token) {
   co_await self->config.cpu.execute({"swarm.catalog.settings", token}, [settings] { validate_settings(settings); });
   co_await mutate<void>(self, token, [self, id, settings, token](forge::db::object::transaction& tx)
      -> boost::asio::awaitable<void> {
      const auto row = co_await self->lookup(tx, id, token);
      if (!row) throw exceptions::missing_metadata{"catalog content is missing"};
      co_await check_stop(token);
      co_await tx.modify(row->id, [settings](db::content& current) {
         current.state = settings.state;
         current.priority = settings.priority;
         current.expires_at = settings.expires_at;
      });
   });
}

boost::asio::awaitable<void> catalog::impl::erase(
   std::shared_ptr<impl> self, core::swarm_id id, std::stop_token token) {
   co_await mutate<void>(self, token, [self, id, token](forge::db::object::transaction& tx)
      -> boost::asio::awaitable<void> {
      const auto row = co_await self->lookup(tx, id, token);
      if (!row) throw exceptions::missing_metadata{"catalog content is missing"};
      co_await check_stop(token);
      co_await tx.erase(row->id);
   });
}

boost::asio::awaitable<forge::db::object::object_page<db::content>> catalog::impl::by_state(
   std::shared_ptr<impl> self, db::job_state state, forge::db::core::page_request page, std::stop_token token) {
   validate_page(page);
   co_await self->config.cpu.execute({"swarm.catalog.state", token}, [state] { validate_state(state); });
   co_await check_stop(token);
   auto read = co_await self->objects.begin_read();
   co_await require_schema(read);
   co_await check_stop(token);
   auto result = co_await read.index<db::content_index, db::by_state_priority>()
      .equal_range(state).page(std::move(page));
   for (auto& row : result.items) {
      row = co_await self->validate(std::move(row), token);
      if (row.state != state) throw exceptions::invalid_metadata{"catalog state index is inconsistent"};
   }
   co_await check_stop(token);
   co_return result;
}

boost::asio::awaitable<forge::db::object::object_page<db::content>> catalog::impl::expiring_before(
   std::shared_ptr<impl> self, std::int64_t exclusive, forge::db::core::page_request page, std::stop_token token) {
   validate_page(page);
   co_await check_stop(token);
   auto read = co_await self->objects.begin_read();
   co_await require_schema(read);
   co_await check_stop(token);
   auto result = co_await read.index<db::content_index, db::by_expiry>()
      .range(std::numeric_limits<std::int64_t>::min(), exclusive).page(std::move(page));
   for (auto& row : result.items) {
      row = co_await self->validate(std::move(row), token);
      if (row.expires_at >= exclusive) throw exceptions::invalid_metadata{"catalog expiry index is inconsistent"};
   }
   co_await check_stop(token);
   co_return result;
}

} // namespace forge::swarm::storage
