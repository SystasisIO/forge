module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/strand.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.asio.gate;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.relay;

#include "details/autorelay_manager.hxx"
#include "details/cancellation_latch.hxx"
#include "details/lifecycle_wakeup.hxx"

namespace forge::net::p2p::detail {
namespace {

void increment(std::uint64_t& value) noexcept {
   if (value != (std::numeric_limits<std::uint64_t>::max)()) { ++value; }
}

} // namespace

autorelay_manager::autorelay_manager(boost::asio::any_io_executor executor, relay::policy policy, callbacks value)
    : _strand(boost::asio::make_strand(std::move(executor))), _policy(policy), _callbacks(std::move(value)),
      _wakeup(std::make_shared<lifecycle_wakeup>()),
      _changed(std::make_shared<lifecycle_wakeup>()),
      _random(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1) {
   if (!_callbacks.current || !_callbacks.reserve || _policy.max_candidates_per_refresh == 0 ||
       _policy.max_parallel_reservations == 0 || _policy.target_reservations == 0 ||
       _policy.max_candidates_per_refresh > 4096 || _policy.target_reservations > _policy.max_candidates_per_refresh ||
       _policy.max_parallel_reservations > _policy.max_candidates_per_refresh ||
       _policy.candidate_backoff.count() <= 0 || _policy.candidate_backoff > std::chrono::hours{24} ||
       _policy.refresh_margin.count() <= 0 || _policy.refresh_margin > std::chrono::hours{24}) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P AutoRelay callbacks or bounds are invalid");
   }
   _stats.enabled = _policy.client_enabled && _policy.auto_discovery_enabled;
   _stats.max_candidates = _policy.max_candidates_per_refresh;
   _stats.max_parallel_reservations = _policy.max_parallel_reservations;
   _stats.target_reservations = _policy.target_reservations;
   if (!_callbacks.now) { _callbacks.now = [] { return std::chrono::steady_clock::now(); }; }
}

autorelay_manager::~autorelay_manager() { request_stop(); }

void autorelay_manager::start(lifecycle_tracker& tracker) {
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_stopping) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P AutoRelay is stopped"); }
      if (_started) { return; }
      _started = true;
      _stats.running = true;
   }
   try {
      auto self = shared_from_this();
      auto operation = tracker.track();
      if (!operation.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P AutoRelay lifecycle is closed"); }
      const auto stop_source = operation.stop_source();
      {
         const auto lock = std::scoped_lock{_mutex};
         _operation = std::move(operation);
      }
      auto subscription = lifecycle_tracker::subscribe_stop(stop_source, self);
      {
         const auto lock = std::scoped_lock{_mutex};
         _subscription = std::move(subscription);
      }
      boost::asio::co_spawn(_strand, run_owned(self),
          [self](std::exception_ptr error) noexcept {
             self->parent_complete(error);
          });
   } catch (...) {
      parent_complete(std::current_exception());
      throw;
   }
}

void autorelay_manager::parent_complete(std::exception_ptr error) noexcept {
   request_stop();
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_parent_done) { return; }
      _parent_done = true;
      _failure = error;
   }
   finish_if_ready();
}

void autorelay_manager::finish_if_ready() noexcept {
   auto operation = lifecycle_tracker::operation{};
   auto subscription = lifecycle_stop_subscription{};
   auto finished = false;
   {
      const auto lock = std::scoped_lock{_mutex};
      if (!_parent_done || _finished) { return; }
      // Parent completion has invalidated every claim; this reap cannot allocate.
      reap_locked(std::chrono::steady_clock::now(), {});
      if (_children == 0) {
         _finished = true;
         _stats.running = false;
         operation = std::move(_operation);
         subscription = std::move(_subscription);
         finished = true;
      }
   }
   if (finished) { subscription.reset(); }
   _wakeup->notify();
   _changed->notify();
   if (finished) { operation.release(); }
}

void autorelay_manager::notify() noexcept { _wakeup->notify(); }
void autorelay_manager::request_lifecycle_stop() noexcept { request_stop(); }

void autorelay_manager::request_stop() noexcept {
   auto claims = std::map<peer_id, std::shared_ptr<work>>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      _stopping = true;
      _stats.stopping = true;
      _stats.permitted = false;
      for (auto& [_, value] : _candidates) {
         if (value.pending) { value.pending->invalidated = true; }
      }
      claims.swap(_pending_cancellations);
   }
   // Stable O(n) claims, no allocation, and no callback under the mutex.
   // Reentrant/idempotent stop sees an empty registry, never the same claim.
   for (const auto& [_, item] : claims) { item->cancellation->request_stop(); }
   _wakeup->notify();
   _changed->notify();
}

void autorelay_manager::cancel_peer(const peer_id& peer) noexcept {
   auto cancellation = std::shared_ptr<cancellation_latch>{};
   auto now = std::chrono::steady_clock::now();
   try { now = _callbacks.now(); } catch (...) {}
   {
      const auto lock = std::scoped_lock{_mutex};
      if (const auto found = _candidates.find(peer); found != _candidates.end()) {
         if (!_stopping) {
            found->second.acquisition_after = std::max(found->second.acquisition_after, now + _policy.candidate_backoff);
         }
         if (found->second.pending) {
            found->second.pending->invalidated = true;
            cancellation = found->second.pending->cancellation;
         }
      } else if (!_stopping) {
         if (const auto found = _backoffs.find(peer); found != _backoffs.end()) {
            found->second.acquisition_after = std::max(found->second.acquisition_after, now + _policy.candidate_backoff);
         }
      }
   }
   if (cancellation) { cancellation->request_stop(); }
   _wakeup->notify();
}

autorelay_manager::time_point autorelay_manager::backoff_locked(candidate_state& value, time_point now) {
   increment(value.failures);
   _random ^= _random << 13;
   _random ^= _random >> 7;
   _random ^= _random << 17;
   const auto cap = _policy.candidate_backoff.count();
   const auto base = std::min<std::int64_t>(cap, 1'000);
   const auto multiplier = std::uint64_t{1} << std::min<std::uint64_t>(value.failures - 1, 20);
   const auto delay = std::min<std::uint64_t>(static_cast<std::uint64_t>(cap),
                                            static_cast<std::uint64_t>(base) * multiplier);
   const auto jitter = delay / 4 == 0 ? 0 : _random % (delay / 4 + 1);
   return now + std::chrono::milliseconds{static_cast<std::int64_t>(delay - jitter)};
}

void autorelay_manager::reap_locked(time_point now, const std::vector<reservation>& reservations) {
   for (auto& [_, value] : _candidates) {
      const auto item = value.pending;
      if (!item || !item->done) { continue; }
      if (_stats.pending_reservations > 0) { --_stats.pending_reservations; }
      const auto live = std::ranges::find_if(reservations, [&](const reservation& current) {
         return current.automatic && current.info.relay_peer == item->source.peer && current.expires_at > now;
      });
      const auto invalidated = _stopping || item->invalidated || item->cancellation->stop_requested();
      const auto committed = _stats.permitted && !invalidated && !item->error && item->reservation_id &&
          live != reservations.end() && live->info.id == *item->reservation_id;
      if (invalidated || (!item->error && !committed)) {
         increment(_stats.invalidated_completions);
         // An orphan acquisition cools without changing genuine failure history.
         // A canceled renewal keeps the existing live lease's scheduling intact.
         if (!_stopping && (!invalidated || !item->renewal || live == reservations.end())) {
            value.acquisition_after = std::max(value.acquisition_after, now + _policy.candidate_backoff);
         }
      } else if (item->error) {
         increment(_stats.failures);
         _stats.last_error = item->error;
         value.retry_after = backoff_locked(value, now);
      } else {
         increment(_stats.successes);
         if (item->renewal) { increment(_stats.renewals); }
         value.failures = 0;
         value.retry_after = {};
         value.acquisition_after = {};
      }
      value.pending.reset();
   }
}

autorelay_manager::time_point autorelay_manager::tick() {
   const auto snapshot = _callbacks.current();
   const auto now = _callbacks.now();
   auto deadline = snapshot.next_expiry;
   auto launches = std::vector<std::shared_ptr<work>>{};
   const auto abandon = [this, &launches](void*) noexcept {
      for (const auto& item : launches) { complete(item, exceptions::code::internal); }
   };
   auto launch_guard = std::unique_ptr<void, decltype(abandon)>{this, abandon};
   auto cancellations = std::vector<std::shared_ptr<cancellation_latch>>{};
   auto sources = std::map<peer_id, candidate>{};
   auto priority = std::vector<peer_id>{};
   for (const auto& value : snapshot.candidates) {
      if (sources.size() == _policy.max_candidates_per_refresh) { break; }
      if (sources.emplace(value.peer, value).second) { priority.push_back(value.peer); }
   }
   {
      const auto lock = std::scoped_lock{_mutex};
      _stats.permitted = !_stopping && _stats.enabled && snapshot.permitted;
      for (auto& [peer, value] : _candidates) {
         const auto item = value.pending;
         if (item && (!snapshot.permitted || _stopping ||
             (!item->done && (item->generation != snapshot.generation ||
              (item->source.session_id != 0 && (!sources.contains(peer) ||
               sources.at(peer).session_id != item->source.session_id)))) ||
             (item->done && item->error == exceptions::code::canceled && item->generation != snapshot.generation))) {
            item->invalidated = true;
            if (!item->done) { cancellations.push_back(item->cancellation); }
         }
      }
      reap_locked(now, snapshot.reservations);
      const auto owned = [&](const peer_id& peer) {
         return std::ranges::any_of(snapshot.reservations, [&](const reservation& current) {
            return current.automatic && current.info.relay_peer == peer && current.expires_at > now;
         });
      };
      for (auto entry = _candidates.begin(); entry != _candidates.end();) {
         const auto& [peer, value] = *entry;
         if (value.pending || sources.contains(peer) || owned(peer)) { ++entry; continue; }
         const auto cooling = value.retry_after > now || value.acquisition_after > now;
         if (value.failures != 0 || value.retry_after != time_point{} || value.acquisition_after != time_point{}) {
            if (_backoffs.size() == _policy.max_candidates_per_refresh) {
               const auto expired = std::ranges::find_if(_backoffs, [&](const auto& history) {
                  return history.second.retry_after <= now && history.second.acquisition_after <= now;
               });
               if (expired != _backoffs.end()) { _backoffs.erase(expired); }
            }
            if (_backoffs.size() < _policy.max_candidates_per_refresh) {
               _backoffs.emplace(peer, retry_state{value.retry_after, value.acquisition_after, value.failures});
            } else if (cooling) {
               // No unexpired history may be evicted to make room for an unknown peer.
               ++entry;
               continue;
            }
         }
         entry = _candidates.erase(entry);
      }
      for (const auto& peer : priority) {
         const auto& value = sources.at(peer);
         if (const auto found = _candidates.find(peer); found != _candidates.end()) {
            found->second.source = value;
         } else {
            const auto previous = _backoffs.find(peer);
            auto displaced = _candidates.end();
            if (_candidates.size() == _policy.max_candidates_per_refresh) {
               if (previous == _backoffs.end()) { continue; }
               displaced = std::ranges::find_if(_candidates, [&](const auto& retained) {
                  return !retained.second.pending && !sources.contains(retained.first) && !owned(retained.first);
               });
               if (displaced == _candidates.end()) { continue; }
            }
            auto state = candidate_state{.source = value};
            if (previous != _backoffs.end()) {
               state.retry_after = previous->second.retry_after;
               state.acquisition_after = previous->second.acquisition_after;
               state.failures = previous->second.failures;
            }
            const auto inserted = _candidates.emplace(peer, std::move(state)).first;
            // Stage both transfers before erasing either retained history entry.
            // The temporary extra node stays under the mutex and rolls back on allocation failure.
            try {
               if (displaced != _candidates.end()) {
                  const auto& retained = displaced->second;
                  _backoffs.emplace(displaced->first,
                      retry_state{retained.retry_after, retained.acquisition_after, retained.failures});
               }
            } catch (...) {
               _candidates.erase(inserted);
               throw;
            }
            if (previous != _backoffs.end()) { _backoffs.erase(previous); }
            if (displaced != _candidates.end()) { _candidates.erase(displaced); }
         }
      }
      for (const auto& [_, value] : _backoffs) {
         if (value.retry_after > now) { deadline = std::min(deadline, value.retry_after); }
         if (value.acquisition_after > now) { deadline = std::min(deadline, value.acquisition_after); }
      }
      for (const auto& [_, value] : _candidates) {
         if (value.retry_after > now) { deadline = std::min(deadline, value.retry_after); }
         if (value.acquisition_after > now) { deadline = std::min(deadline, value.acquisition_after); }
      }
      _stats.candidates = _candidates.size();
      _stats.reservations = snapshot.reservations.size();
      _stats.automatic_reservations = std::ranges::count_if(snapshot.reservations, &reservation::automatic);
      auto new_pending = std::size_t{0};
      for (const auto& [_, value] : _candidates) {
         if (value.pending && !value.pending->renewal) { ++new_pending; }
      }
      for (const auto& value : snapshot.reservations) { deadline = std::min(deadline, value.expires_at); }
      // Preserve owner priorities, including retained work and fair live/hint selection.
      // Map key order must not override the bounded selection order.
      for (const auto& peer : priority) {
         const auto retained = _candidates.find(peer);
         if (retained == _candidates.end()) { continue; }
         auto& value = retained->second;
         if (!_stats.permitted || !sources.contains(peer) || value.pending) { continue; }
         const auto reserved = std::ranges::find_if(snapshot.reservations, [&](const reservation& current) {
            return current.info.relay_peer == peer && current.expires_at > now;
         });
         const auto renewal = reserved != snapshot.reservations.end();
         if (value.retry_after > now || (!renewal && value.acquisition_after > now)) { continue; }
         if (renewal) {
            if (!reserved->automatic) { continue; }
            if (reserved->renew_at > now) {
               deadline = std::min(deadline, reserved->renew_at);
               continue;
            }
         } else if (snapshot.reservations.size() + new_pending >= _policy.target_reservations) {
            continue;
         }
         if (_stats.pending_reservations >= _policy.max_parallel_reservations) { continue; }
         auto item = std::make_shared<work>();
         item->source = value.source;
         item->generation = snapshot.generation;
         item->cancellation = std::make_shared<cancellation_latch>();
         item->renewal = renewal;
         launches.push_back(item);
         _pending_cancellations.emplace(peer, item);
         value.pending = item;
         item->registered = true;
         ++_children;
         ++_stats.pending_reservations;
         if (!renewal) { ++new_pending; }
         increment(_stats.attempts);
      }
      increment(_round);
   }
   for (const auto& cancellation : cancellations) { cancellation->request_stop(); }
   for (const auto& item : launches) {
      try {
         const auto self = shared_from_this();
         boost::asio::co_spawn(_strand, reserve_owned(self, item),
             [self, item](std::exception_ptr error) noexcept {
                self->complete(item, error ? std::optional{exceptions::code::internal} : item->error);
             });
      } catch (...) { complete(item, exceptions::code::internal); }
   }
   static_cast<void>(launch_guard.release());
   _changed->notify();
   return deadline;
}

boost::asio::awaitable<void> autorelay_manager::reserve_owned(std::shared_ptr<autorelay_manager> self,
                                                            std::shared_ptr<work> item) {
   auto error = std::optional<exceptions::code>{};
   auto reservation_id = std::optional<std::uint64_t>{};
   try {
      if (item->cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P AutoRelay reservation canceled before admission");
      }
      reservation_id = (co_await self->_callbacks.reserve(item->source, item->generation, item->cancellation)).id;
   } catch (const forge::exceptions::base& failure) {
      error = exceptions::code_of(failure).value_or(exceptions::code::internal);
   } catch (...) { error = exceptions::code::internal; }
   {
      const auto lock = std::scoped_lock{self->_mutex};
      item->error = error;
      item->reservation_id = reservation_id;
   }
}

void autorelay_manager::complete(const std::shared_ptr<work>& item, std::optional<exceptions::code> error) noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      if (item->done) { return; }
      item->error = error;
      item->done = true;
      if (item->registered) { --_children; }
      const auto found = _pending_cancellations.find(item->source.peer);
      if (found != _pending_cancellations.end() && found->second == item) { _pending_cancellations.erase(found); }
   }
   _wakeup->notify();
   finish_if_ready();
}

boost::asio::awaitable<void> autorelay_manager::run_owned(std::shared_ptr<autorelay_manager> self) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto failure = std::exception_ptr{};
   while (true) {
      const auto observed = self->_wakeup->epoch();
      auto deadline = time_point::max();
      auto stopping = false;
      {
         const auto lock = std::scoped_lock{self->_mutex};
         stopping = self->_stopping;
      }
      // Child terminal callbacks, not another allocating waiter, own the drain.
      if (stopping) { break; }
      try {
         deadline = self->tick();
         if (deadline != time_point::max()) {
            const auto delay = std::max(time_point::duration::zero(), deadline - self->_callbacks.now());
            deadline = std::chrono::steady_clock::now() + delay;
         }
      } catch (...) {
         failure = std::current_exception();
         self->request_stop();
         continue;
      }
      if (deadline == time_point::max()) { co_await self->_wakeup->async_wait(observed); }
      else {
         co_await self->_wakeup->async_wait_until(observed, deadline);
      }
   }
   if (failure) { std::rethrow_exception(failure); }
}

boost::asio::awaitable<void> autorelay_manager::async_join() {
   const auto self = shared_from_this();
   while (true) {
      const auto observed = _changed->epoch();
      {
         const auto lock = std::scoped_lock{_mutex};
         if (!_started || _finished) {
            if (_failure) { std::rethrow_exception(_failure); }
            co_return;
         }
      }
      co_await _changed->async_wait(observed);
   }
}

boost::asio::awaitable<std::vector<relay::reservation::info>>
autorelay_manager::async_refresh(std::chrono::milliseconds timeout) {
   const auto self = shared_from_this();
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   auto round = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_stopping || !_started) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P AutoRelay is not running"); }
      if (_stats.waiting_refreshes >= _policy.max_candidates_per_refresh) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P AutoRelay refresh waiter bound reached");
      }
      ++_stats.waiting_refreshes;
      increment(_stats.refreshes);
      round = _round;
   }
   const auto release = [self](void*) noexcept {
      const auto lock = std::scoped_lock{self->_mutex};
      --self->_stats.waiting_refreshes;
   };
   const auto waiter = std::unique_ptr<void, decltype(release)>{this, release};
   _wakeup->notify();
   while (true) {
      const auto observed = _changed->epoch();
      auto ready = false;
      {
         const auto lock = std::scoped_lock{_mutex};
         if (_stopping) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P AutoRelay refresh stopped"); }
         ready = _round > round && (!_stats.permitted || _stats.pending_reservations == 0 ||
                                   _stats.reservations >= _policy.target_reservations);
      }
      if (ready) {
         const auto current = _callbacks.current();
         auto result = std::vector<relay::reservation::info>{};
         if (current.permitted) {
            for (const auto& value : current.reservations) {
               if (result.size() == _policy.target_reservations) { break; }
               result.push_back(value.info);
            }
         }
         co_return result;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P AutoRelay refresh timed out");
      }
      co_await _changed->async_wait_until(observed, deadline);
   }
}

diagnostics::autorelay_state autorelay_manager::stats() const {
   const auto lock = std::scoped_lock{_mutex};
   return _stats;
}

autorelay_manager::selection_state autorelay_manager::selection() const {
   const auto now = _callbacks.now();
   const auto lock = std::scoped_lock{_mutex};
   auto result = selection_state{};
   for (const auto& [peer, value] : _candidates) {
      const auto item = value.pending;
      if (item && !item->done && !item->invalidated) { result.pending.push_back(value.source); }
      if (value.retry_after > now || value.acquisition_after > now || (item && item->done)) {
         result.unavailable.push_back(peer);
      }
   }
   for (const auto& [peer, value] : _backoffs) {
      if (value.retry_after > now || value.acquisition_after > now) { result.unavailable.push_back(peer); }
   }
   return result;
}

} // namespace forge::net::p2p::detail
