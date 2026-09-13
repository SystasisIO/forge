module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <atomic>
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
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.multiformats.multiaddr;

#include "details/cancellation_latch.hxx"
#include "details/host_addresses.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/reachability_manager.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p::detail {
namespace {

[[nodiscard]] std::chrono::steady_clock::time_point after(std::chrono::steady_clock::time_point now,
                                                        std::chrono::milliseconds delay) {
   const auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
   return now > std::chrono::steady_clock::time_point::max() - duration
       ? std::chrono::steady_clock::time_point::max() : now + duration;
}

[[nodiscard]] bool same_event(const host_event& left, const host_event& right) {
   return left.effective == right.effective && left.autonat_v1 == right.autonat_v1 &&
          std::ranges::equal(left.autonat_v2, right.autonat_v2, [](const auto& a, const auto& b) {
             return a.value == b.value && a.address.to_string() == b.address.to_string();
          });
}

[[nodiscard]] bool public_address(const endpoint& address) {
   return host_addresses::classify_endpoint_scope(address) == host_addresses::endpoint_scope::public_address;
}

} // namespace

reachability_manager::reachability_manager(boost::asio::any_io_executor executor, reachability_policy policy,
                                           callbacks value)
    : strand_(boost::asio::make_strand(std::move(executor))), policy_(policy), callbacks_(std::move(value)),
      state_(policy), wakeup_(std::make_shared<lifecycle_wakeup>()),
      cancellation_(std::make_shared<cancellation_latch>()) {
   validate(policy_);
   if (!callbacks_.observers || !callbacks_.candidates || !callbacks_.exchange || !callbacks_.changed ||
       (policy_.ping_enabled && !callbacks_.ping)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P reachability callbacks are incomplete");
   }
   if (!callbacks_.now) {
      callbacks_.now = [] { return std::chrono::steady_clock::now(); };
   }
}

reachability_manager::~reachability_manager() { request_stop(); }

void reachability_manager::start(lifecycle_tracker& tracker) {
   {
      const auto lock = std::scoped_lock{mutex_};
      if (stopping_) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability manager is stopped");
      }
      if (started_) {
         return;
      }
   }
   auto self = shared_from_this();
   auto operation = tracker.track();
   if (!operation.active()) {
      request_stop();
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability lifecycle is closed");
   }
   auto task = run_lifecycle(self, operation.stop_source());
   auto completion = [self, operation = std::move(operation)](std::exception_ptr error) mutable noexcept {
      self->close_results();
      try {
         if (self->callbacks_.finished) { self->callbacks_.finished(); }
      } catch (...) {
         if (!error) { error = std::current_exception(); }
      }
      operation.release();
      {
         const auto lock = std::scoped_lock{self->mutex_};
         self->finished_ = true;
         self->failure_ = error;
      }
      self->wakeup_->notify();
   };
   {
      const auto lock = std::scoped_lock{mutex_};
      if (stopping_) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability manager cannot be restarted");
      }
      if (started_) {
         return;
      }
      started_ = true;
   }
   try {
      boost::asio::co_spawn(strand_, std::move(task), std::move(completion));
   } catch (...) {
      request_stop();
      {
         const auto lock = std::scoped_lock{mutex_};
         finished_ = true;
         failure_ = std::current_exception();
      }
      wakeup_->notify();
      throw;
   }
}

void reachability_manager::request_stop() noexcept {
   close_results();
   {
      const auto lock = std::scoped_lock{mutex_};
      stopping_ = true;
   }
   cancellation_->request_stop();
   wakeup_->notify();
}

boost::asio::awaitable<void> reachability_manager::run_bridged(std::shared_ptr<reachability_manager> self,
    std::shared_ptr<worker_terminal_owner> terminal) {
   static_cast<void>(terminal->publish([self]() noexcept { self->request_stop(); }));
   co_await run_owned(std::move(self));
}

boost::asio::awaitable<void> reachability_manager::run_lifecycle(std::shared_ptr<reachability_manager> self,
    std::shared_ptr<lifecycle_stop_source> stop) {
   auto failure = std::exception_ptr{};
   try {
      co_await async_run_with_stop_bridge(std::make_shared<worker_stop_bridge>(),
          [self](std::shared_ptr<worker_terminal_owner> terminal) {
             return run_bridged(self, std::move(terminal));
          }, {.lifecycle_stop = std::move(stop)});
   } catch (...) {
      failure = std::current_exception();
   }
   // The bridge may skip its work branch when stopped before first execution.
   // Admission and already accepted children still belong to this tracked owner.
   self->request_stop();
   co_await run_owned(self);
   if (failure) { std::rethrow_exception(failure); }
}

bool reachability_manager::stopping() const {
   const auto lock = std::scoped_lock{mutex_};
   return stopping_;
}

void reachability_manager::notify_addresses_changed() noexcept { wakeup_->notify(); }

host_event reachability_manager::current() const { return state_.snapshot(callbacks_.now()); }

reachability_manager::statistics reachability_manager::stats() const {
   const auto lock = std::scoped_lock{mutex_};
   return statistics_;
}

std::uint64_t reachability_manager::set_addresses(std::span<const endpoint> addresses) {
   return state_.set_addresses(addresses);
}

reachability_state::candidate_snapshot reachability_manager::candidates() const { return state_.candidates(); }
void reachability_manager::invalidate_addresses() noexcept { state_.invalidate(); }
void reachability_manager::close_results() noexcept { state_.close(); }

reachability_state::candidate_snapshot reachability_manager::reconcile() {
   auto snapshot = callbacks_.candidates();
   if (snapshot.addresses.size() > policy_.max_candidates) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P reachability candidate source exceeds its bound");
   }
   // Only the source owner mutates addresses. A returned snapshot may already
   // be stale; never install it back into the state or recover its generation.
   return snapshot;
}

void reachability_manager::publish(time_point now) {
   auto event = state_.snapshot(now);
   if (!published_ || !same_event(*published_, event)) {
      published_ = event;
      callbacks_.changed(std::move(event));
   }
}

void reachability_manager::reap_locked(peer_state& peer, time_point now) {
   if (peer.probe && peer.probe->done) {
      if (peer.probe->error) {
         const auto maximum = std::max(policy_.refresh_interval, policy_.observation_ttl);
         peer.backoff = peer.backoff == std::chrono::milliseconds::zero() ? policy_.refresh_interval
             : peer.backoff > maximum - peer.backoff ? maximum : std::min(maximum, peer.backoff + peer.backoff);
      } else {
         peer.backoff = policy_.refresh_interval;
      }
      peer.next_probe = after(now, peer.backoff);
      peer.probe.reset();
   }
   if (peer.ping && peer.ping->done) {
      peer.next_ping = after(now, policy_.ping_interval);
      peer.ping.reset();
   }
}

std::shared_ptr<reachability_manager::work> reachability_manager::launch(observer value, bool ping,
                                                                       time_point now, bool waiter) {
   if (!ping && !(value.v1 && policy_.client_v1_enabled) && !(value.v2 && policy_.client_v2_enabled)) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P observer has no enabled AutoNAT version");
   }
   if (!valid_peer_id(value.peer) || !host_addresses::observer_group(value.remote) ||
       (value.remote.peer && *value.remote.peer != value.peer)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P reachability observer is not an authenticated numeric peer");
   }
   auto self = shared_from_this();
   auto item = std::make_shared<work>();
   item->source = std::move(value);
   item->ping = ping;
   item->completed = std::make_shared<lifecycle_wakeup>();
   auto task = worker_owned(self, item);
   auto handler = [self, item](std::exception_ptr error) noexcept { self->complete(item, error); };
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!started_ || stopping_) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability manager is not running");
      }
      if (waiter && statistics_.waiters == policy_.max_observers) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P reachability waiter limit reached");
      }
      auto position = peers_.find(item->source.peer);
      if (position == peers_.end()) {
         if ((ping ? statistics_.pending_pings : statistics_.pending_probes) ==
             (ping ? policy_.max_parallel_pings : policy_.max_pending_probes)) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P reachability pending limit reached");
         }
         if (peers_.size() == policy_.max_observers) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P reachability observer limit reached");
         }
         position = peers_.try_emplace(item->source.peer).first;
      }
      reap_locked(position->second, now);
      auto& slot = ping ? position->second.ping : position->second.probe;
      if (slot) {
         statistics_.waiters += waiter;
         return slot;
      }
      auto& active = ping ? statistics_.pending_pings : statistics_.pending_probes;
      const auto limit = ping ? policy_.max_parallel_pings : policy_.max_pending_probes;
      if (active == limit) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P reachability pending limit reached");
      }
      slot = item;
      statistics_.waiters += waiter;
      ++active;
   }
   try {
      boost::asio::co_spawn(strand_, std::move(task), std::move(handler));
   } catch (...) {
      complete(item, std::current_exception());
      if (waiter) {
         const auto lock = std::scoped_lock{mutex_};
         --statistics_.waiters;
      }
      throw;
   }
   wakeup_->notify();
   return item;
}

void reachability_manager::complete(const std::shared_ptr<work>& item, std::exception_ptr error) noexcept {
   {
      const auto lock = std::scoped_lock{mutex_};
      if (item->done) {
         return;
      }
      item->error = error;
      item->done = true;
      if (item->ping) {
         --statistics_.pending_pings;
         auto& counter = error ? statistics_.ping_failures : statistics_.ping_successes;
         if (counter != std::numeric_limits<std::uint64_t>::max()) {
            ++counter;
         }
      } else {
         --statistics_.pending_probes;
         const auto failure = error ? error : item->partial_error;
         if (failure) {
            statistics_.last_probe_error = failure;
            if (statistics_.probe_errors != std::numeric_limits<std::uint64_t>::max()) {
               ++statistics_.probe_errors;
            }
         }
      }
   }
   item->completed->notify();
   wakeup_->notify();
}

boost::asio::awaitable<void> reachability_manager::worker_owned(std::shared_ptr<reachability_manager> self,
                                                               std::shared_ptr<work> item) {
   auto first_error = std::exception_ptr{};
   auto exchanged = false;
   auto completed = false;
   for (const auto v2 : {false, true}) {
      if (item->ping && v2) {
         break;
      }
      if (!item->ping && !(v2 ? self->policy_.client_v2_enabled && item->source.v2
                              : self->policy_.client_v1_enabled && item->source.v1)) {
         continue;
      }
      try {
         if (self->stopping()) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P reachability probe stopped");
         }
         auto snapshot = item->ping ? reachability_state::candidate_snapshot{} : self->reconcile();
         const auto generation = snapshot.generation;
         auto addresses = std::move(snapshot.addresses);
         if (!item->ping && !v2) {
            std::erase_if(addresses, [](const auto& value) { return !public_address(value); });
         }
         if (!item->ping && addresses.empty()) {
            continue;
         }
         if (!item->ping && v2) {
            auto selected = std::size_t{};
            {
               const auto lock = std::scoped_lock{self->mutex_};
               auto& peer = self->peers_.at(item->source.peer);
               if (peer.candidate_generation != generation) {
                  peer.candidate_generation = generation;
                  peer.next_candidate = 0;
               }
               selected = peer.next_candidate % addresses.size();
               peer.next_candidate = (selected + 1) % addresses.size();
            }
            // Keep wire index zero for pinned Rust, but rotate the actual
            // candidate across logical probes, including failed exchanges.
            auto candidate = std::move(addresses[selected]);
            addresses.clear();
            addresses.push_back(std::move(candidate));
         }
         auto cancellation = std::make_shared<cancellation_latch>();
         auto subscription = cancellation_latch::subscribe(self->cancellation_, [cancellation]() noexcept {
            cancellation->request_stop();
         });
         const auto deadline = after(self->callbacks_.now(), item->ping ? self->policy_.ping_timeout : self->policy_.timeout);
         {
            const auto lock = std::scoped_lock{self->mutex_};
            item->cancellation = cancellation;
            item->deadline = deadline;
            item->timed_out = false;
         }
         self->wakeup_->notify();
         if (cancellation->stop_requested()) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P reachability probe canceled before exchange");
         }
         auto response = probe_result{};
         auto error = std::exception_ptr{};
         try {
            exchanged = true;
            if (item->ping) {
               co_await self->callbacks_.ping(item->source.peer, cancellation);
            } else {
               response = co_await self->callbacks_.exchange(item->source, v2, addresses, cancellation);
            }
         } catch (...) {
            error = std::current_exception();
         }
         const auto now = self->callbacks_.now();
         auto timed_out = false;
         {
            const auto lock = std::scoped_lock{self->mutex_};
            timed_out = item->timed_out || now >= deadline;
            item->cancellation.reset();
            item->deadline = time_point::max();
         }
         if (self->stopping()) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P reachability exchange stopped");
         }
         if (timed_out) {
            FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P reachability exchange timed out");
         }
         if (error) {
            std::rethrow_exception(error);
         }
         if (!item->ping) {
            // Another exchange or address-change wakeup can have advanced the
            // generation while this exchange was suspended. Never reuse it.
            static_cast<void>(self->reconcile());
            if (v2 && response.result.observed) {
               auto reported = *response.result.observed;
               reported.peer.reset();
               if (reported.to_string() == addresses.front().to_string()) {
                  static_cast<void>(self->state_.record_v2(item->source.peer, item->source.remote, generation,
                      std::move(reported), response.result.value, response.verified_dialback, now));
               }
            } else if (!v2) {
               static_cast<void>(self->state_.record_v1(item->source.peer, item->source.remote, generation,
                   response.result.value, response.internet_scope && std::ranges::all_of(addresses, public_address), now));
            }
            item->result = std::move(response.result);
            completed = true;
            self->publish(now);
         }
      } catch (...) {
         if (!first_error) {
            first_error = std::current_exception();
         }
      }
   }
   item->partial_error = first_error;
   if (first_error && !completed) {
      std::rethrow_exception(first_error);
   }
   if (!exchanged) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P reachability observer has no eligible probe");
   }
}

reachability_manager::time_point reachability_manager::tick(time_point now) {
   // Address expiry refresh is independent of NAT-change publication.
   static_cast<void>(reconcile());
   state_.expire(now);
   publish(now);
   auto sources = callbacks_.observers();
   if (sources.size() > policy_.max_observers) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P reachability observer source exceeds its bound");
   }
   auto present = std::set<peer_id>{};
   for (const auto& source : sources) {
      present.insert(source.peer);
   }
   auto cancel = std::vector<std::shared_ptr<cancellation_latch>>{};
   auto groups = std::set<std::string>{};
   auto next = after(now, std::min({policy_.refresh_interval, policy_.observation_ttl, policy_.ping_interval}));
   {
      const auto lock = std::scoped_lock{mutex_};
      for (auto& [peer, entry] : peers_) {
         reap_locked(entry, now);
         for (const auto& item : {entry.probe, entry.ping}) {
            if (!item) {
               continue;
            }
            if (!item->ping) {
               if (auto group = host_addresses::observer_group(item->source.remote)) {
                  groups.insert(std::move(*group));
               }
            }
            if (item->cancellation && item->deadline <= now) {
               item->timed_out = true;
               item->deadline = time_point::max();
               cancel.push_back(item->cancellation);
            }
            next = std::min(next, item->deadline);
         }
      }
      std::erase_if(peers_, [&](const auto& pair) {
         return !present.contains(pair.first) && !pair.second.probe && !pair.second.ping;
      });
   }
   for (const auto& item : cancel) {
      item->request_stop();
   }
   for (const auto& source : sources) {
      const auto group = host_addresses::observer_group(source.remote);
      if (!valid_peer_id(source.peer) || !group || (source.remote.peer && *source.remote.peer != source.peer)) {
         continue;
      }
      auto probe = false;
      auto ping = false;
      {
         const auto lock = std::scoped_lock{mutex_};
         if (stopping_) {
            break;
         }
         auto position = peers_.find(source.peer);
         if (position == peers_.end()) {
            if (peers_.size() == policy_.max_observers) {
               continue;
            }
            position = peers_.try_emplace(source.peer).first;
         }
         const auto& entry = position->second;
         const auto eligible = (source.v1 && policy_.client_v1_enabled) || (source.v2 && policy_.client_v2_enabled);
         probe = eligible && !entry.probe && entry.next_probe <= now &&
                 statistics_.pending_probes < policy_.max_pending_probes && !groups.contains(*group);
         ping = policy_.ping_enabled && source.ping && !entry.ping && entry.next_ping <= now &&
                statistics_.pending_pings < policy_.max_parallel_pings;
         if (eligible && entry.next_probe > now) {
            next = std::min(next, entry.next_probe);
         }
         if (policy_.ping_enabled && source.ping && entry.next_ping > now) {
            next = std::min(next, entry.next_ping);
         }
      }
      try {
         if (probe) {
            static_cast<void>(launch(source, false, now, false));
            groups.insert(*group);
         }
         if (ping) {
            static_cast<void>(launch(source, true, now, false));
         }
      } catch (const forge::exceptions::base& error) {
         // Manual admission can fill the bound between selection and launch.
         if (exceptions::code_of(error) != exceptions::code::backpressure_rejected &&
             !(exceptions::code_of(error) == exceptions::code::closed && stopping())) {
            throw;
         }
      }
   }
   return next;
}

boost::asio::awaitable<void> reachability_manager::run_owned(std::shared_ptr<reachability_manager> self) {
   auto error = std::exception_ptr{};
   try {
      while (!self->stopping()) {
         const auto epoch = self->wakeup_->epoch();
         const auto now = self->callbacks_.now();
         const auto next = self->tick(now);
         if (self->stopping()) {
            break;
         }
         // Fake clocks drive state; tests advance them and notify the same
         // wakeup. Production waits use only the existing steady timer path.
         const auto delay = next > now ? next - now : std::chrono::steady_clock::duration::zero();
         const auto real_now = std::chrono::steady_clock::now();
         const auto deadline = real_now > time_point::max() - delay ? time_point::max() : real_now + delay;
         static_cast<void>(co_await self->wakeup_->async_wait_until(epoch, deadline));
      }
   } catch (...) {
      error = std::current_exception();
   }
   self->request_stop();
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   for (;;) {
      const auto epoch = self->wakeup_->epoch();
      {
         const auto lock = std::scoped_lock{self->mutex_};
         if (self->statistics_.pending_probes == 0 && self->statistics_.pending_pings == 0) {
            break;
         }
      }
      static_cast<void>(co_await self->wakeup_->async_wait(epoch));
   }
   if (error) {
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<reachability::result> reachability_manager::async_probe(observer value) {
   return probe_owned(shared_from_this(), std::move(value));
}

boost::asio::awaitable<reachability::result>
reachability_manager::probe_owned(std::shared_ptr<reachability_manager> self, observer value) {
   auto item = self->launch(std::move(value), false, self->callbacks_.now(), true);
   auto result = reachability::result{};
   auto error = std::exception_ptr{};
   try {
      for (;;) {
         const auto epoch = item->completed->epoch();
         {
            const auto lock = std::scoped_lock{self->mutex_};
            if (item->done) {
               error = item->error;
               result = item->result;
               break;
            }
         }
         static_cast<void>(co_await item->completed->async_wait(epoch));
      }
   } catch (...) {
      error = std::current_exception();
   }
   {
      const auto lock = std::scoped_lock{self->mutex_};
      --self->statistics_.waiters;
   }
   self->wakeup_->notify();
   if (error) {
      std::rethrow_exception(error);
   }
   co_return result;
}

boost::asio::awaitable<void> reachability_manager::async_join() { return join_owned(shared_from_this()); }

boost::asio::awaitable<void> reachability_manager::join_owned(std::shared_ptr<reachability_manager> self) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   for (;;) {
      const auto epoch = self->wakeup_->epoch();
      auto error = std::exception_ptr{};
      auto done = false;
      {
         const auto lock = std::scoped_lock{self->mutex_};
         done = !self->started_ || (self->finished_ && self->statistics_.waiters == 0 &&
                 self->statistics_.pending_probes == 0 && self->statistics_.pending_pings == 0);
         error = self->failure_;
      }
      if (done) {
         if (error) {
            std::rethrow_exception(error);
         }
         co_return;
      }
      static_cast<void>(co_await self->wakeup_->async_wait(epoch));
   }
}

} // namespace forge::net::p2p::detail
