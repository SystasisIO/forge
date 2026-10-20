module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
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

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"
#include "details/call_scope.hxx"
#include "details/peer_session.hxx"

namespace forge::swarm::runtime::detail {

operation_state::operation_state(std::uint64_t id, std::uint64_t picker_seed, std::uint64_t choker_seed,
   core::descriptor value, std::shared_ptr<core::store> storage,
   std::string scope, forge::asio::task::scheduler& tasks, forge::asio::compute::executor executor,
   std::shared_ptr<admission> access, std::shared_ptr<capabilities> capabilities, engine::options limits,
   std::shared_ptr<resource_budget> resources, bool downloading,
   std::optional<std::chrono::steady_clock::time_point> deadline)
   : number{id}, picker_seed{picker_seed}, choker_seed{choker_seed},
     manifest{std::move(value)}, realm{std::move(scope)}, store{std::move(storage)},
     scheduler{tasks}, cpu{executor}, policy{std::move(access)}, providers{std::move(capabilities)},
     config{limits}, budget{std::move(resources)}, downloading{downloading}, total_deadline{deadline} {}
void operation_state::cancel() noexcept {
   std::shared_ptr<forge::asio::task::handle> task;
   capabilities::registration* advertisement;
   { auto lock = std::scoped_lock{mutex}; state.canceled = true; task = worker; advertisement = registration.get(); }
   stop.request_stop();
   if (advertisement) advertisement->cancel();
   if (task) task->cancel();
   changed.notify();
}
void operation_state::check() const {
   if (total_deadline && std::chrono::steady_clock::now() >= *total_deadline) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::deadline_exceeded, "Swarm download total deadline exceeded");
   }
   if (stop.stop_requested()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Swarm operation canceled");
   }
}
operation_status operation_state::inspect() const {
   auto lock = std::scoped_lock{mutex};
   auto result = state;
   result.upload_candidates = candidates.size(); result.unchoked_peers = allowed.size();
   return result;
}
void operation_state::add_fact(std::uint64_t facts::* field, std::uint64_t amount) {
   auto lock = std::scoped_lock{mutex};
   auto& value = state.traffic.*field;
   value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}
void operation_state::enter() {
   auto lock = std::scoped_lock{mutex};
   check();
   if (state.closed) { FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Swarm operation closed"); }
   ++state.active_calls;
}
void operation_state::leave() noexcept {
   { auto lock = std::scoped_lock{mutex}; --state.active_calls; }
   changed.notify();
}
std::uint64_t operation_state::next_peer() { auto lock = std::scoped_lock{mutex}; return peer_sequence++; }
std::shared_ptr<operation_state::piece_work> operation_state::piece(core::piece_id id, const std::string& peer) {
   // Called under the short picker lock; no store call or await here.
   if (auto found = pieces.find(id); found != pieces.end()) return found->second;
   auto work = std::make_shared<piece_work>();
   work->capacity = budget->reserve(number, peer, {0, 1, 2ULL * core::piece_length(manifest, id), 1});
   pieces.emplace(id, work);
   return work;
}
void operation_state::upload_select_locked() {
   auto inventory = std::vector<core::choker::candidate>{};
   inventory.reserve(candidates.size());
   for (const auto& [id, candidate] : candidates) inventory.push_back(candidate);
   const auto now = std::chrono::steady_clock::now();
   const auto round_time = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
   if (round_time <= upload_time) {
      upload_next = upload_time + std::chrono::milliseconds{1};
      return;
   }
   upload_time = round_time;
   const auto plan = upload->select(inventory, upload_time, true);
   allowed = std::set<core::peer_id>{plan.unchoked.begin(), plan.unchoked.end()};
   upload_next = now + config.upload_round;
}
void operation_state::upload_enter(core::peer_id id) {
   {
      auto lock = std::scoped_lock{mutex};
      if (candidates.size() >= config.upload.peers) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm upload candidate registry full");
      }
      candidates.emplace(id, core::choker::candidate{.peer = id, .interested = false});
      upload_select_locked();
   }
   changed.notify();
}
void operation_state::upload_leave(core::peer_id id) noexcept {
   {
      auto lock = std::scoped_lock{mutex};
      candidates.erase(id); allowed.erase(id);
      // Recompute on the next surviving writer's wake, not during noexcept teardown.
      upload_next = {};
   }
   changed.notify();
}
void operation_state::upload_interest(core::peer_id id, bool interested) {
   {
      auto lock = std::scoped_lock{mutex};
      if (auto found = candidates.find(id); found != candidates.end()) found->second.interested = interested;
      if (!interested) allowed.erase(id);
      upload_select_locked();
   }
   changed.notify();
}
void operation_state::upload_served(core::peer_id id, std::uint64_t bytes) {
   auto lock = std::scoped_lock{mutex};
   if (auto found = candidates.find(id); found != candidates.end()) {
      auto& value = found->second.served_bytes;
      value += std::min(bytes, std::numeric_limits<std::uint64_t>::max() - value);
   }
}
bool operation_state::upload_allowed(core::peer_id id) {
   auto lock = std::scoped_lock{mutex};
   if (std::chrono::steady_clock::now() >= upload_next) upload_select_locked();
   return allowed.contains(id) && candidates.contains(id) && candidates.at(id).interested;
}
boost::asio::awaitable<void> operation_state::wait(std::shared_ptr<operation_state> self) {
   if (!self) { FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Swarm download handle empty"); }
   auto ticket = co_await self->join_gate.acquire();
   while (true) {
      const auto epoch = self->changed.epoch();
      bool ready;
      { auto lock = std::scoped_lock{self->mutex}; ready = !self->initializing; }
      if (ready) break;
      co_await self->changed.async_wait(epoch);
   }
   if (!self->joined && self->worker) {
      try { co_await self->worker->wait(); }
      catch (...) { if (!self->failure) self->failure = std::current_exception(); }
      self->joined = true;
   }
   if (self->failure && !self->inspect().complete) self->cancel();
   while (true) {
      const auto epoch = self->changed.epoch();
      bool done;
      { auto lock = std::scoped_lock{self->mutex}; done = self->deadline_watch_done; }
      if (done) break;
      co_await self->changed.async_wait(epoch);
   }
   if (self->inspect().complete) co_return;
   if (self->timer_failure) std::rethrow_exception(self->timer_failure);
   if (self->total_deadline && std::chrono::steady_clock::now() >= *self->total_deadline) self->check();
   if (self->failure) std::rethrow_exception(self->failure);
   self->check();
}
boost::asio::awaitable<void> operation_state::close(std::shared_ptr<operation_state> self) {
   if (!self) co_return;
   self->cancel();
   auto ticket = co_await self->join_gate.acquire();
   while (true) {
      const auto epoch = self->changed.epoch();
      bool ready;
      { auto lock = std::scoped_lock{self->mutex}; ready = !self->initializing; }
      if (ready) break;
      co_await self->changed.async_wait(epoch);
   }
   if (self->inspect().closed) co_return;
   auto failure = std::exception_ptr{};
   if (self->registration) {
      self->registration->cancel();
      try { co_await self->registration->async_close(); } catch (...) { failure = std::current_exception(); }
   }
   if (self->worker && !self->joined) {
      self->worker->cancel();
      try { co_await self->worker->wait(); } catch (...) { /* Expected operation cancellation; async_wait reports worker errors. */ }
      self->joined = true;
   }
   // A canceled pending task may never enter its callable. Retained callable
   // captures must not retain capacity past this explicit join/release fence.
   if (self->orchestration_capacity) self->orchestration_capacity->release();
   while (true) {
      const auto epoch = self->changed.epoch();
      bool timer_done;
      { auto lock = std::scoped_lock{self->mutex}; timer_done = self->deadline_watch_done; }
      if (self->inspect().active_calls == 0 && timer_done) break;
      co_await self->changed.async_wait(epoch);
   }
   self->pieces.clear();
   if (!failure && self->timer_failure) failure = self->timer_failure;
   // Store is caller-owned. Never request_stop/close it, even on failure.
   if (self->lease.active()) {
      try { static_cast<void>(co_await self->store->async_checkpoint()); }
      catch (...) { if (!failure) failure = std::current_exception(); }
   }
   self->lease.release();
   {
      auto lock = std::scoped_lock{self->mutex};
      self->picker.reset(); self->upload.reset(); self->candidates.clear(); self->allowed.clear();
      self->sessions.clear(); self->manifest = core::descriptor{};
   }
   self->metadata_capacity.release();
   self->algorithm_capacity.release();
   self->timer_capacity.release();
   { auto lock = std::scoped_lock{self->mutex}; self->worker.reset(); self->state.closed = true; }
   self->changed.notify();
   if (failure) std::rethrow_exception(failure);
}
boost::asio::awaitable<void> operation_state::run(std::shared_ptr<operation_state> self, std::vector<std::string> routes) {
   self->check();
   if (co_await publish_if_complete(self)) co_return;
   if (!self->providers) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm download needs connection capability");
   }
   if (routes.empty()) {
      auto scope = std::make_shared<call_scope>(self, "lookup:" + std::to_string(self->number));
      scope->bind({}); scope->start();
      std::exception_ptr failure;
      try {
         routes = co_await self->providers->async_lookup(self->content, self->realm, self->config.peers,
            scope->stop.get_token(), scope->deadline);
      } catch (...) { failure = std::current_exception(); }
      co_await scope->finish();
      if (failure) std::rethrow_exception(failure);
      scope->check();
   }
   if (routes.empty() || routes.size() > self->config.peers) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm provider lookup empty or exceeds bound");
   }
   for (const auto& route : routes) {
      if (route.empty() || route.size() > 1024) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Swarm provider route exceeds bound");
      }
   }
   auto attempts_without_progress = 0U;
   while (attempts_without_progress < self->config.attempts) {
      {
         auto lock = std::scoped_lock{self->mutex};
         if (self->integrity_failures >= self->config.integrity_failures) {
            FORGE_THROW_EXCEPTION(core::exceptions::integrity_failure, "Swarm download integrity failure budget exhausted");
         }
      }
      self->check();
      std::uint64_t progress_before;
      { auto lock = std::scoped_lock{self->mutex}; progress_before = self->useful_progress; }
      // Bounded children use the already admitted orchestration slot, not
      // scheduler slots whose parents are waiting for them.
      auto remaining = std::make_shared<std::atomic_size_t>(0);
      auto wake = std::make_shared<forge::asio::notification>();
      const auto executor = co_await boost::asio::this_coro::executor;
      std::exception_ptr launch_failure;
      try {
         for (const auto& route : routes) {
            ++*remaining;
            try {
               boost::asio::co_spawn(executor, peer_session::connect(self, route),
                  [remaining, wake](std::exception_ptr) { --*remaining; wake->notify(); });
            } catch (...) { --*remaining; throw; }
         }
      } catch (...) { launch_failure = std::current_exception(); self->cancel(); }
      while (true) {
         const auto epoch = wake->epoch();
         if (remaining->load() == 0) break;
         co_await wake->async_wait(epoch);
      }
      if (launch_failure) std::rethrow_exception(launch_failure);
      std::exception_ptr storage_failure;
      std::exception_ptr cleanup_failure;
      { auto lock = std::scoped_lock{self->mutex};
        storage_failure = self->storage_failure; cleanup_failure = self->cleanup_failure; }
      if (storage_failure) std::rethrow_exception(storage_failure);
      if (cleanup_failure) std::rethrow_exception(cleanup_failure);
      {
         auto lock = std::scoped_lock{self->mutex};
         if (self->integrity_failures >= self->config.integrity_failures) {
            FORGE_THROW_EXCEPTION(core::exceptions::integrity_failure, "Swarm download integrity failure budget exhausted");
         }
      }
      self->check();
      if (co_await publish_if_complete(self)) co_return;
      { auto lock = std::scoped_lock{self->mutex};
        attempts_without_progress = self->useful_progress != progress_before ? 0 : attempts_without_progress + 1; }
   }
   FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm bounded peer attempts exhausted");
}
boost::asio::awaitable<bool> operation_state::publish_if_complete(std::shared_ptr<operation_state> self) {
   { auto lock = std::scoped_lock{self->mutex}; if (!self->picker->inspect().complete) co_return false; }
   self->check();
   co_await self->store->async_publish(self->stop.get_token());
   { auto lock = std::scoped_lock{self->mutex}; self->check(); self->state.complete = true; }
   self->changed.notify();
   co_return true;
}
boost::asio::awaitable<void> operation_state::watch_deadline(std::shared_ptr<operation_state> self) {
   while (true) {
      const auto epoch = self->changed.epoch();
      bool expired;
      {
         auto lock = std::scoped_lock{self->mutex};
         if (self->stop.stop_requested() || self->state.complete) co_return;
         expired = std::chrono::steady_clock::now() >= *self->total_deadline;
      }
      if (expired) { self->cancel(); co_return; }
      co_await self->changed.async_wait_until(epoch, *self->total_deadline);
   }
}

} // namespace forge::swarm::runtime::detail
