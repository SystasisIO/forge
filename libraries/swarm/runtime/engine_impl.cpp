module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.swarm.runtime.engine;

import forge.asio.gate;
import forge.asio.notification;
import forge.asio.runtime;
import forge.crypto.core.random;

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"
#include "details/call_scope.hxx"
#include "details/peer_session.hxx"
#include "details/engine_impl.hxx"

namespace forge::swarm::runtime {

engine::impl::impl(forge::asio::task::scheduler& tasks, forge::asio::compute::executor executor,
   std::shared_ptr<admission> access, std::shared_ptr<capabilities> capabilities, options limits)
   : scheduler{tasks}, cpu{executor}, policy{std::move(access)}, providers{std::move(capabilities)}, config{limits},
     budget{std::make_shared<detail::resource_budget>(config.global, config.per_operation, config.per_peer)},
     selection_seed{initial_seed(config.selection_seed)} {
   if (!cpu.valid() || !config.operations || !config.peers || !config.attempts || !config.integrity_failures ||
       config.idle_timeout.count() <= 0 || config.callback_timeout.count() <= 0 || config.upload_round.count() <= 0) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Swarm engine needs bounded execution configuration");
   }
}
std::uint64_t engine::impl::initial_seed(std::optional<std::uint64_t> configured) {
   if (configured) return *configured;
   const auto bytes = forge::crypto::core::random_array<8>();
   auto seed = std::uint64_t{};
   for (auto index = 0U; index < bytes.size(); ++index) seed |= std::uint64_t{bytes[index]} << (8U * index);
   return seed;
}
std::uint64_t engine::impl::mix_seed(std::uint64_t value) noexcept {
   value += 0x9e3779b97f4a7c15ULL;
   value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
   value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
   return value ^ (value >> 31U);
}
void engine::impl::cancel() noexcept {
   { auto lock = std::scoped_lock{mutex}; stopped = true; }
   // Once stopped, create cannot prune/append the registry. Traverse without
   // allocation, and invoke cancellation callbacks outside the engine lock.
   for (auto index = std::size_t{};; ++index) {
      std::shared_ptr<detail::operation_state> operation;
      { auto lock = std::scoped_lock{mutex}; if (index == operations.size()) break; operation = operations[index]; }
      operation->cancel();
   }
}
std::shared_ptr<detail::operation_state> engine::impl::find(core::swarm_id id, const std::string& realm) {
   auto lock = std::scoped_lock{mutex};
   if (!stopped) for (const auto& operation : operations) {
      auto state_lock = std::scoped_lock{operation->mutex};
      if (!operation->initializing && !operation->state.closed &&
          !operation->stop.stop_requested() && operation->content == id && operation->realm == realm)
         return operation;
   }
   FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm content not admitted for sharing");
}
boost::asio::awaitable<std::shared_ptr<detail::operation_state>> engine::impl::create(
   std::shared_ptr<impl> self, core::descriptor manifest, std::shared_ptr<core::store> store,
   std::string realm, bool downloading, std::vector<std::unique_ptr<capabilities::route>> routes,
   std::optional<std::chrono::steady_clock::time_point> total_deadline) {
   protocol::validate_realm(realm);
   if (!store || routes.size() > self->config.peers) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Swarm operation needs store and bounded providers");
   }
   std::shared_ptr<detail::operation_state> operation;
   {
      auto lock = std::scoped_lock{self->mutex};
      if (self->stopped) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm engine admission stopped");
      }
      std::erase_if(self->operations, [](const auto& value) { return value->inspect().closed; });
      if (self->stopped || self->operations.size() >= self->config.operations) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm operation admission closed or full");
      }
      for (const auto& value : self->operations) {
         if (value->store == store && downloading && value->downloading) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm store already owned by active writer");
         }
      }
      const auto operation_seed = mix_seed(self->selection_seed + self->seed_sequence++);
      operation = std::make_shared<detail::operation_state>(self->sequence++,
         mix_seed(operation_seed ^ 0x7069636b6572ULL), mix_seed(operation_seed ^ 0x63686f6b6572ULL),
         std::move(manifest), std::move(store),
         std::move(realm), self->scheduler, self->cpu, self->policy, self->providers, self->config, self->budget, downloading, total_deadline);
      self->operations.push_back(operation);
   }
   std::exception_ptr failure;
   try {
      operation->lease = operation->store->acquire_lease();
      if (total_deadline) {
         operation->timer_capacity = self->budget->reserve(operation->number,
            "local:" + std::to_string(operation->number), {0, 1, 1024, 0});
         { auto lock = std::scoped_lock{operation->mutex}; operation->deadline_watch_done = false; }
         try {
            boost::asio::co_spawn(self->scheduler.runtime_context().context(), detail::operation_state::watch_deadline(operation),
               [operation](std::exception_ptr failure) {
                  if (failure) {
                     { auto lock = std::scoped_lock{operation->mutex}; operation->timer_failure = failure; }
                     operation->cancel();
                  }
                  { auto lock = std::scoped_lock{operation->mutex}; operation->deadline_watch_done = true; }
                  operation->changed.notify();
               });
         } catch (...) {
            { auto lock = std::scoped_lock{operation->mutex}; operation->deadline_watch_done = true; }
            throw;
         }
      }
      operation->metadata_capacity = self->budget->reserve(operation->number, "local:" + std::to_string(operation->number),
         {0, 1, core::max_metadata_size, 0});
      const auto identity = co_await self->cpu.execute({.name = "swarm-descriptor", .parent_stop_token = operation->stop.get_token()},
         [operation](forge::asio::compute::context& context) {
            context.throw_if_stop_requested();
            const auto summary = core::validate_descriptor(operation->manifest);
            return std::pair{core::calculate_swarm_id(operation->manifest), summary};
         });
      operation->content = identity.first;
      operation->metadata_size = identity.second.metadata_size;
      auto checkpoint = co_await operation->store->async_checkpoint(operation->stop.get_token());
      co_await self->cpu.execute({.name = "swarm-store-binding", .parent_stop_token = operation->stop.get_token()},
         [operation, checkpoint](forge::asio::compute::context& context) {
            context.throw_if_stop_requested();
            core::validate_checkpoint(checkpoint, operation->manifest);
         });
      operation->upload = std::make_unique<core::choker>(operation->config.upload, operation->choker_seed);
      if (downloading) {
         // Only verified storage state resumes picking. Stale unfinished blocks
         // are overwritten through a new writer; never trust remote checkpoint.
         auto local = core::resume_state{.id = operation->content, .verified = std::move(checkpoint.verified)};
         operation->algorithm_capacity = self->budget->reserve(operation->number,
            "local:" + std::to_string(operation->number), {0, 1, 128ULL * identity.second.total_pieces +
               2ULL * identity.second.metadata_size, 0});
         operation->picker = co_await self->cpu.execute({.name = "swarm-picker", .parent_stop_token = operation->stop.get_token()},
            [operation, local = std::move(local)](forge::asio::compute::context& context) mutable {
               context.throw_if_stop_requested();
               return std::make_unique<core::transfer>(operation->manifest, operation->config.transfer,
                  operation->picker_seed, std::move(local));
            });
         auto capacity = std::make_shared<detail::resource_budget::ticket>(self->budget->reserve(operation->number,
            "local:" + std::to_string(operation->number), {0, 1, 4096, 0}));
         auto owned_worker = std::make_shared<forge::asio::task::handle>();
         auto owned_routes = std::make_shared<std::vector<std::unique_ptr<capabilities::route>>>(std::move(routes));
         operation->orchestration_capacity = capacity;
         auto worker = self->scheduler.submit(forge::asio::task::awaitable{
            .name = "swarm-download", .work = [operation, owned_routes, capacity](forge::asio::task::context& context) mutable
               -> boost::asio::awaitable<void> {
               std::stop_callback cancel{context.stop_token(), [operation] { operation->cancel(); }};
               try { co_await detail::operation_state::run(operation, std::move(*owned_routes)); }
               catch (...) { operation->failure = std::current_exception(); }
               // Task completion may precede destruction of this callable.
               capacity->release();
               operation->changed.notify();
            }});
         if (!worker.accepted()) {
            owned_routes->clear();
            capacity->release();
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm scheduler rejected worker");
         }
         *owned_worker = std::move(worker);
         { auto lock = std::scoped_lock{operation->mutex}; operation->worker = owned_worker; }
         if (operation->stop.stop_requested()) owned_worker->cancel();
      } else if (operation->providers) {
         operation->availability_capacity = self->budget->reserve(operation->number,
            "local:" + std::to_string(operation->number), {0, 1, 4096, 0});
         { auto lock = std::scoped_lock{operation->mutex}; operation->availability_watch_done = false; }
         try {
            boost::asio::co_spawn(self->scheduler.runtime_context().context(), detail::operation_state::watch_availability(operation),
               [operation](std::exception_ptr failure) {
                  if (failure) {
                     { auto lock = std::scoped_lock{operation->mutex}; operation->availability_failure = failure; }
                     operation->cancel();
                  }
                  operation->availability_capacity.release();
                  { auto lock = std::scoped_lock{operation->mutex}; operation->availability_watch_done = true; }
                  operation->changed.notify();
               });
         } catch (...) {
            { auto lock = std::scoped_lock{operation->mutex}; operation->availability_watch_done = true; }
            throw;
         }
         while (true) {
            const auto epoch = operation->changed.epoch();
            bool ready;
            std::exception_ptr registration_failure;
            { auto lock = std::scoped_lock{operation->mutex};
              ready = operation->availability_ready || operation->availability_watch_done;
              registration_failure = operation->availability_failure; }
            if (registration_failure) std::rethrow_exception(registration_failure);
            if (ready) break;
            co_await operation->changed.async_wait(epoch);
         }
      }
      operation->check();
   } catch (...) { failure = std::current_exception(); }
   { auto lock = std::scoped_lock{operation->mutex}; operation->initializing = false; }
   operation->changed.notify();
   if (failure) {
      try { co_await detail::operation_state::close(operation); } catch (...) { /* Preserve initialization failure. */ }
      std::rethrow_exception(failure);
   }
   co_return operation;
}
boost::asio::awaitable<void> engine::impl::close(std::shared_ptr<impl> self) {
   self->cancel();
   std::exception_ptr failure;
   for (auto index = std::size_t{};; ++index) {
      std::shared_ptr<detail::operation_state> operation;
      { auto lock = std::scoped_lock{self->mutex}; if (index == self->operations.size()) break; operation = self->operations[index]; }
      try { co_await detail::operation_state::close(operation); }
      catch (...) { if (!failure) failure = std::current_exception(); }
   }
   if (failure) std::rethrow_exception(failure);
}

} // namespace forge::swarm::runtime
