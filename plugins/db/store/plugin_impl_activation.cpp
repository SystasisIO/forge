module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <exception>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <atomic>

module forge.plugins.db.store.plugin;

import forge.asio.compute;
import forge.app.plugin_lifecycle;
import forge.plugins.db.store.api;
import forge.plugins.db.store.types;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.object.store;
import forge.db.blob.store;
import forge.db.revision.store;
import forge.exceptions;

#include "details/plugin_impl.hxx"
#include "details/activation_state.hxx"

namespace forge::plugins::db::store {

boost::asio::awaitable<void> plugin::impl::close_for_activation(std::string name) {
   auto self = shared_from_this();
   auto record = std::shared_ptr<managed_store>{};
   auto candidate = std::shared_ptr<activation_state>{};
   auto root = std::optional<std::filesystem::path>{};
   {
      const auto lock = std::scoped_lock{mutex};
      const auto found = stores.find(name);
      if (found == stores.end())
         FORGE_THROW_EXCEPTION(exceptions::unknown_store, "activation store is not registered");
      record = found->second;
      if (!record->activation) {
         if (current.load() != phase::ready || !record->opened || !record->started || !record->owns_driver ||
             !record->driver || record->driver_name != "mdbx" || record->durability != "durable-sync" ||
             !compute.valid() || !std::filesystem::path{record->path}.is_absolute())
            FORGE_THROW_EXCEPTION(exceptions::unsupported_activation,
                                  "strict activation requires an owned durable-sync MDBX store and App compute");
         root = record->path;
      }
   }
   if (root)
      candidate = std::make_shared<activation_state>(std::move(*root));
   {
      const auto lock = std::scoped_lock{mutex};
      if (record->strict_failure)
         std::rethrow_exception(record->strict_failure);
      if (record->activation_claimed || record->strict_running || record->activation_io)
         FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "activation store already has an owner");
      if (record->strict_closed)
         co_return; // SAME completed close, not a second backend close.
   }
   auto first = false;
   const auto generation = lifecycle.snapshot().generation;
   const auto admitted = [&] {
      const auto lock = std::scoped_lock{mutex};
      if (record->strict_failure)
         std::rethrow_exception(record->strict_failure);
      if (record->strict_running || record->strict_closed || record->activation_claimed || record->activation_io)
         FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "activation store already has an owner");
      if (!record->activation) {
         if (current.load() != phase::ready || !record->opened || !record->started || !record->owns_driver)
            FORGE_THROW_EXCEPTION(exceptions::stopped, "activation store admission is closed");
         record->activation = candidate;
         record->opened = false;
         record->started = false;
         first = true;
      }
      record->strict_running = true;
   };
   // Existing strict close retries are cleanup of the SAME retained owner,
   // allowed after stop; first admission must still pass canonical lifecycle.
   if (candidate) {
      if (!lifecycle.try_admit(generation, admitted))
         FORGE_THROW_EXCEPTION(exceptions::stopped, "activation was stopped before admission");
   } else {
      admitted();
   }
   auto error = std::exception_ptr{};
   try {
      if (first) {
         const auto state = record->activation;
         // P1 execute contract: throwing setup precedes submission; delivery
         // follows callback AND capture retirement. No detached/borrowed work.
         co_await compute.execute({.name = "db-activation-prepare"}, [state] { state->prepare(); });
      }
      co_await record->driver->async_close(); // NEVER flush before this retry.
   } catch (...) {
      error = std::current_exception();
   }
   auto retryable = false;
   if (error) {
      try {
         std::rethrow_exception(error);
      } catch (const forge::exceptions::base& value) {
         retryable = value.code() == forge::exceptions::make_error_code(forge::db::core::exceptions::code::driver_busy);
      } catch (...) {
      }
   }
   auto retired = opened_store{};
   {
      const auto lock = std::scoped_lock{mutex};
      record->strict_running = false;
      if (error) {
         if (!retryable)
            record->strict_failure = error;
      } else {
         record->strict_closed = true;
         retired.objects = std::move(record->objects);
         retired.blobs = std::move(record->blobs);
         retired.revisions = std::move(record->revisions);
      }
   }
   // Retire closed layers only outside the mutex; keep exact driver/record.
   if (error)
      std::rethrow_exception(error);
}

void plugin::impl::claim_activation(const std::string& name) {
   const auto lock = std::scoped_lock{mutex};
   const auto found = stores.find(name);
   if (found == stores.end() || !found->second->strict_closed || found->second->strict_failure ||
       found->second->activation_claimed)
      FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "closed store capability is unavailable");
   found->second->activation_claimed = true;
}

boost::asio::awaitable<void> plugin::impl::activate_closed(std::string name,
                                                           std::optional<std::filesystem::path> destination) {
   auto self = shared_from_this();
   auto record = std::shared_ptr<managed_store>{};
   const auto generation = lifecycle.snapshot().generation;
   if (!lifecycle.try_admit(generation, [&] {
          const auto lock = std::scoped_lock{mutex};
          const auto found = stores.find(name);
          if (found == stores.end())
             FORGE_THROW_EXCEPTION(exceptions::unknown_store, "activation store is not registered");
          record = found->second;
          if (!record->strict_closed || !record->activation_claimed || record->activation_io ||
              record->strict_failure || current.load() != phase::ready)
             FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "closed store operation is unavailable");
          record->activation_io = true;
       }))
      FORGE_THROW_EXCEPTION(exceptions::stopped, "closed store operation admission is closed");
   const auto state = record->activation;
   auto error = std::exception_ptr{};
   try {
      co_await compute.execute({.name = destination ? "db-activation-publish" : "db-activation-confirm"},
                               [state, destination = std::move(destination)]() mutable {
                                  if (destination)
                                     state->publish(std::move(*destination));
                                  else
                                     state->confirm();
                               });
   } catch (...) {
      error = std::current_exception();
   }
   {
      const auto lock = std::scoped_lock{mutex};
      // execute's sole completion is terminal, including cancellation/throw.
      record->activation_io = false;
   }
   if (error)
      std::rethrow_exception(error);
}

activation_status plugin::impl::closed_state(const std::string& name) const {
   auto state = std::shared_ptr<activation_state>{};
   {
      const auto lock = std::scoped_lock{mutex};
      const auto found = stores.find(name);
      if (found == stores.end() || !found->second->strict_closed)
         FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "store has no proven strict close");
      state = found->second->activation;
   }
   return state->snapshot();
}

bool plugin::impl::activation_inflight(const std::string& name) const noexcept {
   const auto lock = std::scoped_lock{mutex};
   const auto found = stores.find(name);
   return found != stores.end() && (found->second->strict_running || found->second->activation_io);
}

} // namespace forge::plugins::db::store
