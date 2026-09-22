module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <set>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.discovery;
import forge.net.p2p.exceptions;
import forge.net.p2p.topology;

#include "details/cancellation_latch.hxx"
#include "details/lifecycle_tracker.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/topology_manager.hxx"

namespace forge::net::p2p::detail {
namespace {

constexpr auto periodic_jitter_scale = std::uint64_t{1'000'000};

[[nodiscard]] std::chrono::milliseconds scale_periodic_delay(std::uint64_t base, std::uint64_t numerator) noexcept {
   const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
   const auto adjusted = (static_cast<unsigned __int128>(base) * numerator) / periodic_jitter_scale;
   if (adjusted >= maximum) {
      return std::chrono::milliseconds{(std::numeric_limits<std::int64_t>::max)()};
   }
   const auto bounded = static_cast<std::uint64_t>(adjusted);
   return std::chrono::milliseconds{static_cast<std::int64_t>(std::max<std::uint64_t>(1, bounded))};
}

[[nodiscard]] std::chrono::steady_clock::time_point
saturating_periodic_deadline(std::chrono::steady_clock::time_point now, std::chrono::milliseconds delay) noexcept {
   if (delay <= std::chrono::milliseconds::zero() || now == std::chrono::steady_clock::time_point::max()) {
      return now;
   }

   using steady_duration = std::chrono::steady_clock::duration;
   using conversion = std::ratio_divide<std::milli, steady_duration::period>;
   const auto increment = (static_cast<unsigned __int128>(delay.count()) * conversion::num) / conversion::den;
   const auto maximum = static_cast<unsigned __int128>((std::numeric_limits<steady_duration::rep>::max)());
   const auto base = static_cast<__int128>(now.time_since_epoch().count());
   const auto available = static_cast<__int128>(maximum) - base;
   if (available <= 0 || increment >= static_cast<unsigned __int128>(available)) {
      return std::chrono::steady_clock::time_point::max();
   }
   return std::chrono::steady_clock::time_point{
       steady_duration{static_cast<steady_duration::rep>(base + static_cast<__int128>(increment))}};
}

} // namespace

std::chrono::system_clock::time_point
saturating_topology_expiry(std::chrono::system_clock::time_point now, std::chrono::milliseconds interval) noexcept {
   if (interval <= std::chrono::milliseconds::zero() || now == std::chrono::system_clock::time_point::max()) {
      return now;
   }

   using system_duration = std::chrono::system_clock::duration;
   using conversion = std::ratio_divide<std::milli, system_duration::period>;
   const auto increment = (static_cast<unsigned __int128>(interval.count()) * conversion::num) / conversion::den;
   const auto maximum = static_cast<__int128>((std::numeric_limits<system_duration::rep>::max)());
   const auto base = static_cast<__int128>(now.time_since_epoch().count());
   const auto available = maximum - base;
   if (available <= 0 || increment >= static_cast<unsigned __int128>(available)) {
      return std::chrono::system_clock::time_point::max();
   }
   return std::chrono::system_clock::time_point{
       system_duration{static_cast<system_duration::rep>(base + static_cast<__int128>(increment))}};
}

topology_manager::topology_manager(topology::policy policy, callbacks callbacks_value, clocks clocks_value,
                                   std::uint64_t periodic_jitter_seed)
    : policy_{std::move(policy)}, callbacks_{std::move(callbacks_value)}, clocks_{std::move(clocks_value)},
      periodic_jitter_seed_{periodic_jitter_seed}, changed_{std::make_shared<lifecycle_wakeup>()} {
   forge::net::p2p::validate(policy_);
   if (!clocks_.steady_now) {
      clocks_.steady_now = [] { return std::chrono::steady_clock::now(); };
   }
   if (!clocks_.system_now) {
      clocks_.system_now = [] { return std::chrono::system_clock::now(); };
   }
   const auto point_limit = std::min(policy_.rendezvous_points.size(), policy_.max_rendezvous_points);
   for (auto point_index = std::size_t{}; point_index < point_limit; ++point_index) {
      const auto& point = policy_.rendezvous_points[point_index];
      if (!point.endpoint.peer) {
         continue;
      }
      for (const auto& namespace_name : point.namespaces) {
         rendezvous_clients_.try_emplace(rendezvous_key{
                                             .peer = *point.endpoint.peer,
                                             .namespace_name = namespace_name,
                                         },
                                         rendezvous_state{.point_index = point_index});
      }
   }
}

topology_manager::~topology_manager() {
   request_stop();
}

bool topology_manager::observation_key::operator<(const observation_key& other) const noexcept {
   if (peer != other.peer) {
      return peer < other.peer;
   }
   return static_cast<std::uint16_t>(source) < static_cast<std::uint16_t>(other.source);
}

bool topology_manager::rendezvous_key::operator<(const rendezvous_key& other) const noexcept {
   if (peer != other.peer) {
      return peer < other.peer;
   }
   return namespace_name < other.namespace_name;
}

bool topology_manager::stopping() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return phase_ == phase::stopping || phase_ == phase::stopped;
}

std::chrono::milliseconds topology_manager::periodic_refresh_delay(std::uint64_t sequence) const noexcept {
   const auto base = static_cast<std::uint64_t>(policy_.refresh_interval.count());
   auto value = std::uint64_t{1469598103934665603ULL};
   value ^= periodic_jitter_seed_;
   value ^= sequence;
   value *= 1099511628211ULL;
   const auto jitter = static_cast<std::uint64_t>(std::llround(policy_.retry_jitter * periodic_jitter_scale));
   const auto sample = value % (periodic_jitter_scale + 1U);
   const auto numerator = periodic_jitter_scale - jitter +
                          ((2U * jitter * sample) / periodic_jitter_scale);
   return scale_periodic_delay(base, numerator);
}

bool topology_manager::rendezvous_refresh_due_locked(std::chrono::steady_clock::time_point steady_now,
                                                     std::chrono::system_clock::time_point system_now) const noexcept {
   if (!policy_.rendezvous_enabled) {
      return false;
   }
   for (const auto& [_, state] : rendezvous_clients_) {
      if (state.retry_after != std::chrono::steady_clock::time_point{}) {
         if (state.retry_after <= steady_now) {
            return true;
         }
         continue;
      }
      if (state.confirmed_registration && state.renew_after != std::chrono::system_clock::time_point{} &&
          state.renew_after <= system_now) {
         return true;
      }
   }
   return false;
}

std::chrono::steady_clock::time_point topology_manager::next_autonomous_wakeup() const {
   const auto steady_now = clocks_.steady_now();
   const auto system_now = clocks_.system_now();
   const auto saturating_deadline = [steady_now](std::chrono::system_clock::duration remaining) {
      if (remaining <= std::chrono::system_clock::duration::zero()) {
         return steady_now;
      }
      const auto available = std::chrono::steady_clock::time_point::max() - steady_now;
      if (remaining >= std::chrono::duration_cast<std::chrono::system_clock::duration>(available)) {
         return std::chrono::steady_clock::time_point::max();
      }
      return steady_now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining);
   };

   const auto lock = std::scoped_lock{mutex_};
   if (rendezvous_refresh_due_locked(steady_now, system_now)) {
      return steady_now;
   }
   auto deadline = next_periodic_refresh_ == std::chrono::steady_clock::time_point{}
                       ? steady_now
                       : next_periodic_refresh_;
   for (const auto& [_, state] : mdns_) {
      for (const auto& lease : state.leases) {
         deadline = std::min(deadline, lease.expires_at);
      }
      if (state.retry_after > steady_now) { deadline = std::min(deadline, state.retry_after); }
   }
   if (policy_.rendezvous_enabled) {
      for (const auto& [_, state] : rendezvous_clients_) {
         if (state.retry_after != std::chrono::steady_clock::time_point{}) {
            deadline = std::min(deadline, state.retry_after);
            continue;
         }
         if (state.confirmed_registration && state.renew_after != std::chrono::system_clock::time_point{}) {
            deadline = std::min(deadline, saturating_deadline(state.renew_after - system_now));
         }
      }
   }
   return deadline;
}

bool topology_manager::queue_due_refresh_locked(std::chrono::steady_clock::time_point steady_now,
                                                std::chrono::system_clock::time_point system_now) {
   if (phase_ != phase::running || refresh_queued_ || refresh_running_) {
      return false;
   }

   const auto periodic_due = next_periodic_refresh_ == std::chrono::steady_clock::time_point{} ||
                             next_periodic_refresh_ <= steady_now;
   if (!periodic_due && !rendezvous_refresh_due_locked(steady_now, system_now)) {
      return false;
   }
   if (periodic_due) {
      const auto maximum = (std::chrono::steady_clock::time_point::max)();
      do {
         ++periodic_refresh_sequence_;
         const auto delay = periodic_refresh_delay(periodic_refresh_sequence_);
         next_periodic_refresh_ = saturating_periodic_deadline(next_periodic_refresh_, delay);
      } while (next_periodic_refresh_ <= steady_now && next_periodic_refresh_ != maximum);
   }
   static_cast<void>(queue_refresh_locked());
   return true;
}

topology_manager::status topology_manager::current() const {
   const auto now = clocks_.steady_now();
   const auto lock = std::scoped_lock{mutex_};
   auto waiting = std::size_t{};
   for (const auto& [_, value] : waiters_) {
      waiting += value.count;
   }
   return status{
       .lifecycle_phase = phase_,
       .refresh_queued = refresh_queued_,
       .refresh_in_flight = refresh_running_,
       .observations = observations_.size(),
       .mdns_observations = static_cast<std::size_t>(std::ranges::count_if(mdns_, [&](const auto& entry) {
          return std::ranges::any_of(entry.second.leases, [&](const auto& lease) { return lease.expires_at > now; });
       })),
       .active_operations = active_cancellations_.size(),
       .waiting_refreshes = waiting,
       .completed_refreshes = completed_refreshes_,
       .failed_refreshes = failed_refreshes_,
   };
}

std::uint64_t topology_manager::queue_refresh_locked() {
   if (refresh_running_) {
      return running_generation_;
   }
   if (refresh_queued_) {
      return queued_generation_;
   }
   refresh_queued_ = true;
   queued_generation_ = next_generation_++;
   return queued_generation_;
}

void topology_manager::release_waiter(std::uint64_t generation) noexcept {
   try {
      const auto lock = std::scoped_lock{mutex_};
      const auto waiter = waiters_.find(generation);
      if (waiter == waiters_.end() || waiter->second.count == 0) {
         return;
      }
      if (--waiter->second.count == 0) {
         waiters_.erase(waiter);
      }
   } catch (...) {
      // Waiter cancellation must not prevent the caller from receiving its original error.
   }
}

void topology_manager::start(lifecycle_tracker& lifecycle) {
   if (policy_.operating_mode == topology::mode::static_only) {
      const auto lock = std::scoped_lock{mutex_};
      if (phase_ == phase::stopping || phase_ == phase::stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "cannot start P2P topology manager after shutdown");
      }
      if (started_) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P topology manager is already running");
      }
      // Static mode owns no lifecycle operation but remains observable as idle until shutdown is requested.
      started_ = true;
      parent_finished_ = true;
      return;
   }

   auto operation = lifecycle.track();
   if (!operation.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "cannot start P2P topology manager after shutdown");
   }
   lifecycle_ = std::addressof(lifecycle);
   {
      const auto lock = std::scoped_lock{mutex_};
      if (phase_ == phase::stopping || phase_ == phase::stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "cannot start P2P topology manager after shutdown");
      }
      if (started_) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P topology manager is already running");
      }
      started_ = true;
      phase_ = phase::running;
      periodic_refresh_sequence_ = 0;
      next_periodic_refresh_ =
          saturating_periodic_deadline(clocks_.steady_now(), periodic_refresh_delay(periodic_refresh_sequence_));
      static_cast<void>(queue_refresh_locked());
   }

   const auto executor = operation.executor();
   auto self = shared_from_this();
   try {
      boost::asio::co_spawn(
          executor, [self] { return self->async_run(); },
          [self = std::move(self), operation = std::move(operation)](std::exception_ptr error) mutable {
             self->finish_parent(error);
             operation.release();
          });
   } catch (...) {
      {
         const auto lock = std::scoped_lock{mutex_};
         phase_ = phase::idle;
         started_ = false;
         refresh_queued_ = false;
         queued_generation_ = 0;
      }
      operation.release();
      throw;
   }
   changed_->notify();
}

void topology_manager::request_stop() noexcept {
   auto cancellations = std::vector<std::shared_ptr<cancellation_latch>>{};
   try {
      {
         const auto lock = std::scoped_lock{mutex_};
         if (phase_ == phase::stopping || phase_ == phase::stopped) {
            return;
         }
         phase_ = phase::stopping;
         refresh_queued_ = false;
         queued_generation_ = 0;
         // request_stop() is noexcept. Transfer the existing cancellation
         // owners instead of copying a vector on the shutdown path.
         cancellations.swap(active_cancellations_);
         if (policy_.operating_mode == topology::mode::static_only) {
            parent_finished_ = true;
            phase_ = phase::stopped;
         }
      }
      for (const auto& cancellation : cancellations) {
         cancellation->request_stop();
      }
      changed_->notify();
   } catch (...) {
      // Shutdown must continue even when an ancillary notification fails.
   }
}

void topology_manager::finish_parent(std::exception_ptr failure) noexcept {
   if (clocks_.before_parent_completion) {
      try {
         clocks_.before_parent_completion();
      } catch (...) {
         // Completion publication cannot depend on ancillary diagnostics or test hooks.
      }
   }

   auto should_log = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      should_log = failure && phase_ != phase::stopping && phase_ != phase::stopped;
      if (refresh_running_) {
         if (const auto waiter = waiters_.find(running_generation_); waiter != waiters_.end()) {
            // The waiter slot was allocated before this generation could start.
            waiter->second.completed.emplace(completion{.failure = std::move(failure)});
         }
      }
      refresh_running_ = false;
      refresh_queued_ = false;
      parent_finished_ = true;
      phase_ = phase::stopped;
   }
   changed_->notify();
   if (should_log) {
      try {
         forge::exceptions::capture_and_log("P2P topology manager stopped unexpectedly");
      } catch (...) {
         // Completion handlers must remain noexcept so lifecycle tracking is released.
      }
   }
}

void topology_manager::finish_refresh(std::uint64_t generation, std::vector<discovery::result> results,
                                      std::exception_ptr failure) noexcept {
   if (clocks_.before_refresh_completion) {
      try {
         clocks_.before_refresh_completion();
      } catch (...) {
         // Completion publication cannot depend on ancillary diagnostics or test hooks.
      }
   }

   {
      const auto lock = std::scoped_lock{mutex_};
      refresh_running_ = false;
      running_generation_ = 0;
      ++completed_refreshes_;
      if (failure) {
         ++failed_refreshes_;
      }
      if (const auto waiter = waiters_.find(generation); waiter != waiters_.end()) {
         // The waiter slot was allocated before this generation could start.
         waiter->second.completed.emplace(completion{.results = std::move(results), .failure = std::move(failure)});
      }
   }
   changed_->notify();
}

void topology_manager::add_cancellation(const std::shared_ptr<cancellation_latch>& cancellation) {
   auto cancel_now = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      active_cancellations_.push_back(cancellation);
      cancel_now = phase_ == phase::stopping || phase_ == phase::stopped;
   }
   if (cancel_now) {
      cancellation->request_stop();
   }
}

void topology_manager::remove_cancellation(const std::shared_ptr<cancellation_latch>& cancellation) noexcept {
   try {
      const auto lock = std::scoped_lock{mutex_};
      std::erase(active_cancellations_, cancellation);
   } catch (...) {
   }
}

boost::asio::awaitable<std::vector<discovery::result>> topology_manager::async_refresh() {
   if (policy_.operating_mode == topology::mode::static_only) {
      co_return std::vector<discovery::result>{};
   }

   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (phase_ == phase::stopping || phase_ == phase::stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P topology manager is stopped");
      }
      if (!started_) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P topology refresh requires a started node");
      }
      generation = queue_refresh_locked();
      auto [waiter, _] = waiters_.try_emplace(generation);
      ++waiter->second.count;
   }
   changed_->notify();

   while (true) {
      const auto observed = changed_->epoch();
      auto result = std::optional<completion>{};
      auto stopped = false;
      try {
         const auto lock = std::scoped_lock{mutex_};
         if (const auto waiter = waiters_.find(generation); waiter != waiters_.end() && waiter->second.completed) {
            result = *waiter->second.completed;
         } else {
            stopped = phase_ == phase::stopping || phase_ == phase::stopped;
         }
      } catch (...) {
         release_waiter(generation);
         throw;
      }
      if (result) {
         release_waiter(generation);
         if (result->failure) {
            std::rethrow_exception(result->failure);
         }
         co_return std::move(result->results);
      }
      if (stopped) {
         release_waiter(generation);
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P topology manager stopped during refresh");
      }
      try {
         static_cast<void>(co_await changed_->async_wait(observed));
      } catch (...) {
         release_waiter(generation);
         throw;
      }
   }
}

boost::asio::awaitable<void> topology_manager::async_join() {
   while (true) {
      const auto observed = changed_->epoch();
      {
         const auto lock = std::scoped_lock{mutex_};
         if (!started_ || parent_finished_ || policy_.operating_mode == topology::mode::static_only) {
            co_return;
         }
      }
      static_cast<void>(co_await changed_->async_wait(observed));
   }
}

boost::asio::awaitable<void> topology_manager::async_run() {
   try {
      while (true) {
         const auto observed = changed_->epoch();
         auto generation = std::uint64_t{};
         auto reconcile = false;
         const auto now = clocks_.steady_now();
         {
            const auto lock = std::scoped_lock{mutex_};
            if (phase_ != phase::running) {
               break;
            }
            for (auto it = mdns_.begin(); it != mdns_.end();) {
               const auto removed = std::erase_if(it->second.leases, [&](const auto& lease) { return lease.expires_at <= now; });
               reconcile_pending_ = reconcile_pending_ || removed != 0;
               if (it->second.leases.empty()) { it = mdns_.erase(it); }
               else {
                  if (it->second.retry_after != std::chrono::steady_clock::time_point{} && it->second.retry_after <= now) {
                     it->second.retry_after = {};
                     reconcile_pending_ = true;
                  }
                  ++it;
               }
            }
            if (refresh_queued_) {
               generation = queued_generation_;
               refresh_queued_ = false;
               queued_generation_ = 0;
               refresh_running_ = true;
               running_generation_ = generation;
            }
            if (reconcile_pending_) {
               reconcile_pending_ = false;
               reconcile = true;
            }
         }

         if (generation != 0) {
            co_await async_refresh_generation(generation);
            continue;
         }
         if (reconcile) {
            try { co_await async_reconcile_sessions(); }
            catch (...) {
               if (!stopping()) { forge::exceptions::capture_and_log("P2P mDNS reconciliation failed"); }
            }
            continue;
         }

         const auto deadline = next_autonomous_wakeup();
         if (clocks_.before_idle_wait) {
            clocks_.before_idle_wait();
         }
         try {
            if (clocks_.idle_wait) {
               co_await clocks_.idle_wait(deadline);
            } else {
               static_cast<void>(co_await changed_->async_wait_until(observed, deadline));
            }
         } catch (...) {
            if (!stopping()) {
               forge::exceptions::capture_and_log("P2P topology manager timer failed");
            }
         }
         {
            const auto steady_now = clocks_.steady_now();
            const auto system_now = clocks_.system_now();
            const auto lock = std::scoped_lock{mutex_};
            static_cast<void>(queue_due_refresh_locked(steady_now, system_now));
         }
      }
   } catch (...) {
      if (!stopping()) {
         forge::exceptions::capture_and_log("P2P topology manager failed");
      }
   }
   try {
      co_await async_unregister_rendezvous();
   } catch (...) {
      forge::exceptions::capture_and_log("P2P topology rendezvous unregister failed");
   }
}

boost::asio::awaitable<void> topology_manager::async_refresh_generation(std::uint64_t generation) {
   auto results = std::vector<discovery::result>{};
   auto failure = std::exception_ptr{};
   try {
      results = co_await async_collect_discovery();
      merge_observations(results);
      co_await async_reconcile_sessions();
   } catch (...) {
      failure = std::current_exception();
      if (!stopping()) {
         forge::exceptions::capture_and_log("P2P topology refresh failed");
      }
   }
   finish_refresh(generation, std::move(results), std::move(failure));
}

} // namespace forge::net::p2p::detail
