module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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
#include <string>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.crypto.core.random;
import forge.exceptions;
import forge.log.logger;
import forge.net.p2p.exceptions;
import forge.net.p2p.lifecycle;
import forge.net.p2p.identity;
import forge.multiformats.multiaddr;

#include "details/bootstrap_service.hxx"
#include "details/cancellation_latch.hxx"
#include "details/lifecycle_wakeup.hxx"

namespace forge::net::p2p::detail {
namespace {

[[nodiscard]] bool is_expected_cancellation(const std::exception_ptr& failure) noexcept {
   if (!failure) {
      return false;
   }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::exceptions::base& error) {
      return forge::net::p2p::exceptions::is(error, forge::net::p2p::exceptions::code::canceled) ||
             forge::net::p2p::exceptions::is(error, forge::net::p2p::exceptions::code::closed);
   } catch (const boost::system::system_error& error) {
      return error.code() == boost::asio::error::operation_aborted;
   } catch (...) {
      return false;
   }
}

[[nodiscard]] std::string failure_message(const std::exception_ptr& failure) {
   try {
      std::rethrow_exception(failure);
   } catch (const std::exception& error) {
      return error.what();
   } catch (...) {
      return "unknown P2P bootstrap failure";
   }
}

} // namespace

bootstrap_service::batch_state::batch_state(boost::asio::strand<boost::asio::any_io_executor> executor,
                                            std::vector<std::string> keys_value,
                                            std::optional<std::chrono::steady_clock::time_point> deadline_value,
                                            bool stop_after_connection_value, std::size_t workers)
    : keys{std::move(keys_value)}, deadline{deadline_value}, stop_after_connection{stop_after_connection_value},
      remaining_workers{workers}, completion{std::move(executor)} {}

bootstrap_service::bootstrap_service(boost::asio::any_io_executor executor, lifecycle_options options,
                                     callbacks callbacks_value)
    : executor_{std::move(executor)}, strand_{boost::asio::make_strand(executor_)}, options_{std::move(options)},
      callbacks_{std::move(callbacks_value)}, retry_wakeup_{std::make_shared<lifecycle_wakeup>()} {
   replace_bootstrap(options_.bootstrap);
}

std::string bootstrap_service::key_for(const bootstrap_peer& peer) {
   return peer.address.to_string();
}

std::chrono::milliseconds bootstrap_service::retry_delay(std::size_t failures) const {
   auto delay = options_.bootstrap_retry_initial_delay;
   for (auto attempt = std::size_t{1}; attempt < failures && delay < options_.bootstrap_retry_max_delay; ++attempt) {
      delay = delay > options_.bootstrap_retry_max_delay - delay ? options_.bootstrap_retry_max_delay : delay + delay;
   }
   delay = std::min(delay, options_.bootstrap_retry_max_delay);
   if (options_.bootstrap_retry_jitter == 0.0 || delay.count() == 0) {
      return delay;
   }

   try {
      const auto random = forge::crypto::core::random_bytes(2);
      const auto sample = (static_cast<std::uint16_t>(random[0]) << 8U) | random[1];
      const auto unit = static_cast<double>(sample) / static_cast<double>(std::numeric_limits<std::uint16_t>::max());
      const auto factor = 1.0 - options_.bootstrap_retry_jitter + (2.0 * options_.bootstrap_retry_jitter * unit);
      const auto jittered =
          static_cast<std::chrono::milliseconds::rep>(std::llround(static_cast<double>(delay.count()) * factor));
      return std::min(std::chrono::milliseconds{std::max<std::chrono::milliseconds::rep>(1, jittered)},
                      options_.bootstrap_retry_max_delay);
   } catch (...) {
      return delay;
   }
}

bool bootstrap_service::stopping() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return stopping_;
}

std::vector<std::string> bootstrap_service::all_keys() const {
   auto out = std::vector<std::string>{};
   const auto lock = std::scoped_lock{mutex_};
   out.reserve(entries_.size());
   for (const auto& [key, _] : entries_) {
      out.push_back(key);
   }
   return out;
}

std::vector<std::string> bootstrap_service::due_keys(std::chrono::steady_clock::time_point now) const {
   auto out = std::vector<std::string>{};
   const auto lock = std::scoped_lock{mutex_};
   out.reserve(entries_.size());
   for (const auto& [key, value] : entries_) {
      if (value.next_attempt <= now) {
         out.push_back(key);
      }
   }
   return out;
}

std::chrono::milliseconds bootstrap_service::next_maintenance_delay(std::chrono::steady_clock::time_point now) const {
   auto next = now + options_.maintenance_interval;
   const auto lock = std::scoped_lock{mutex_};
   for (const auto& [_, value] : entries_) {
      next = std::min(next, value.next_attempt);
   }
   if (next <= now) {
      return std::chrono::milliseconds{1};
   }
   return std::max(std::chrono::milliseconds{1}, std::chrono::duration_cast<std::chrono::milliseconds>(next - now));
}

boost::asio::awaitable<bool> bootstrap_service::async_attempt(const std::string& key,
                                                              std::chrono::milliseconds timeout) {
   auto configured = bootstrap_peer{};
   auto generation = std::uint64_t{};
   auto known_peer = std::optional<peer_id>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = entries_.find(key);
      if (stopping_ || found == entries_.end()) {
         co_return false;
      }
      configured = found->second.configured;
      generation = found->second.generation;
      known_peer = found->second.connected_peer ? found->second.connected_peer : found->second.configured_peer;
   }

   if (known_peer && callbacks_.connected(configured, *known_peer)) {
      auto should_protect = false;
      {
         const auto lock = std::scoped_lock{mutex_};
         const auto found = entries_.find(key);
         if (found == entries_.end() || found->second.generation != generation) {
            co_return false;
         }
         found->second.failures = 0;
         found->second.next_attempt = std::chrono::steady_clock::now() + options_.maintenance_interval;
         should_protect = found->second.protected_peer != known_peer;
         found->second.protected_peer = known_peer;
      }
      if (should_protect) {
         callbacks_.protect(*known_peer);
      }
      co_return true;
   }

   auto cancellation = std::make_shared<cancellation_latch>();
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = entries_.find(key);
      if (stopping_ || found == entries_.end() || found->second.generation != generation) {
         co_return false;
      }
      if (found->second.active_cancellation) {
         co_return false;
      }
      found->second.active_cancellation = cancellation;
   }

   auto connected = std::optional<peer_id>{};
   auto failure = std::exception_ptr{};
   try {
      connected = co_await callbacks_.connect(configured, timeout, cancellation);
   } catch (...) {
      failure = std::current_exception();
   }
   static_cast<void>(cancellation->finish());
   const auto expected_cancellation = is_expected_cancellation(failure);
   const auto failure_text = failure && !expected_cancellation ? failure_message(failure) : std::string{};

   auto should_protect = false;
   auto previously_protected = std::optional<peer_id>{};
   auto should_unprotect_previous = false;
   auto current = false;
   auto suppress_failure_log = false;
   auto failures = std::size_t{};
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = entries_.find(key);
      current = found != entries_.end() && found->second.generation == generation;
      suppress_failure_log = stopping_ || !current || expected_cancellation;
      if (current) {
         found->second.active_cancellation.reset();
         if (connected) {
            found->second.connected_peer = connected;
            found->second.failures = 0;
            found->second.next_attempt = std::chrono::steady_clock::now() + options_.maintenance_interval;
            should_protect = found->second.protected_peer != connected;
            if (should_protect) {
               previously_protected = found->second.protected_peer;
            }
            found->second.protected_peer = connected;
            if (previously_protected) {
               should_unprotect_previous = std::ranges::none_of(entries_, [&](const auto& item) {
                  return item.first != key && item.second.protected_peer == previously_protected;
               });
            }
         } else if (!expected_cancellation) {
            if (!failure_text.empty() && !stopping_) {
               last_failure_ = failure_text;
            }
            if (found->second.failures < std::numeric_limits<std::size_t>::max()) {
               ++found->second.failures;
            }
            found->second.next_attempt = std::chrono::steady_clock::now() + retry_delay(found->second.failures);
            failures = found->second.failures;
         }
      }
   }

   if (should_protect) {
      callbacks_.protect(*connected);
      if (previously_protected && should_unprotect_previous) {
         callbacks_.unprotect(*previously_protected);
      }
   }
   // The existing retry ordinal bounds diagnostics without retaining another peer map.
   if (failure && !suppress_failure_log && failures != 0 && (failures & (failures - 1)) == 0) {
      dlog("forge.net.p2p.discovery", "P2P bootstrap connection failed",
           ("event", "bootstrap.connect_failed")("failures", failures)("timeout_ms", timeout.count()));
   } else if (connected && current) {
      dlog("forge.net.p2p.discovery", "P2P bootstrap connection established",
           ("event", "bootstrap.connected")("generation", generation));
   }
   co_return connected.has_value() && current;
}

boost::asio::awaitable<void> bootstrap_service::async_batch_worker(std::shared_ptr<batch_state> batch) {
   try {
      while (!batch->connected && batch->next < batch->keys.size() && !stopping()) {
         const auto key = batch->keys[batch->next++];
         auto timeout = options_.connect_timeout;
         if (batch->deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= *batch->deadline) {
               break;
            }
            timeout = std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(*batch->deadline - now));
            timeout = std::max(timeout, std::chrono::milliseconds{1});
         }
         const auto connected = co_await async_attempt(key, timeout);
         if (connected && batch->stop_after_connection) {
            batch->connected = true;
            cancel_attempts(batch->keys, key);
         }
      }
   } catch (...) {
      if (!batch->failure) {
         batch->failure = std::current_exception();
      }
      cancel_attempts(batch->keys);
   }
   if (--batch->remaining_workers == 0) {
      batch->completion.cancel();
   }
}

boost::asio::awaitable<bool>
bootstrap_service::async_run_batch_on_strand(std::vector<std::string> keys,
                                             std::optional<std::chrono::steady_clock::time_point> deadline,
                                             bool stop_after_connection) {
   if (keys.empty() || stopping()) {
      co_return false;
   }
   const auto workers = std::min(options_.max_parallel_bootstrap, keys.size());
   auto batch = std::make_shared<batch_state>(strand_, std::move(keys), deadline, stop_after_connection, workers);
   auto launched = std::size_t{};
   for (auto worker = std::size_t{}; worker < workers; ++worker) {
      try {
         auto self = shared_from_this();
         boost::asio::co_spawn(
             strand_,
             [self = std::move(self), batch]() -> boost::asio::awaitable<void> {
                co_await self->async_batch_worker(batch);
             },
             boost::asio::detached);
         ++launched;
      } catch (...) {
         batch->failure = std::current_exception();
         batch->remaining_workers -= workers - launched;
         cancel_attempts(batch->keys);
         if (batch->remaining_workers == 0) {
            batch->completion.cancel();
         }
         break;
      }
   }

   auto cancellation_failure = std::exception_ptr{};
   while (batch->remaining_workers != 0) {
      batch->completion.expires_at(boost::asio::steady_timer::time_point::max());
      auto error = boost::system::error_code{};
      try {
         co_await batch->completion.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      } catch (...) {
         if (!cancellation_failure) {
            cancellation_failure = std::current_exception();
            cancel_attempts(batch->keys);
         }
      }
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none && !cancellation_failure) {
         cancellation_failure =
             std::make_exception_ptr(boost::system::system_error{boost::asio::error::operation_aborted});
         cancel_attempts(batch->keys);
      }
      if (cancellation_failure) {
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      }
   }
   if (cancellation_failure) {
      std::rethrow_exception(cancellation_failure);
   }
   if (batch->failure) {
      std::rethrow_exception(batch->failure);
   }
   co_return batch->connected;
}

boost::asio::awaitable<bool>
bootstrap_service::async_run_batch(std::vector<std::string> keys,
                                   std::optional<std::chrono::steady_clock::time_point> deadline,
                                   bool stop_after_connection) {
   co_return co_await boost::asio::co_spawn(strand_,
                                            async_run_batch_on_strand(std::move(keys), deadline, stop_after_connection),
                                            boost::asio::use_awaitable);
}

boost::asio::awaitable<void> bootstrap_service::async_wait_for_retry(std::chrono::steady_clock::time_point deadline) {
   auto observed = retry_wakeup_->epoch();
   const auto now = std::chrono::steady_clock::now();
   if (now >= deadline || stopping()) {
      co_return;
   }

   const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
   const auto delay = std::min(next_maintenance_delay(now), std::max(remaining, std::chrono::milliseconds{1}));
   {
      const auto lock = std::scoped_lock{mutex_};
      if (stopping_) {
         co_return;
      }
   }
   static_cast<void>(co_await retry_wakeup_->async_wait_until(observed, now + delay));
}

boost::asio::awaitable<std::size_t> bootstrap_service::async_initial_bootstrap() {
   const auto deadline = std::chrono::steady_clock::now() + options_.startup_budget;
   if (options_.requirement == bootstrap_requirement::allow_disconnected) {
      static_cast<void>(co_await async_run_batch(all_keys(), deadline, false));
      co_return connected_count();
   }

   while (!stopping() && std::chrono::steady_clock::now() < deadline) {
      if (connected_count() != 0) {
         break;
      }
      const auto keys = due_keys(std::chrono::steady_clock::now());
      if (!keys.empty()) {
         static_cast<void>(co_await async_run_batch(keys, deadline, true));
         if (connected_count() != 0) {
            break;
         }
      }
      co_await async_wait_for_retry(deadline);
   }
   co_return connected_count();
}

boost::asio::awaitable<void> bootstrap_service::async_set_bootstrap(std::vector<bootstrap_peer> peers) {
   co_await boost::asio::dispatch(strand_, boost::asio::use_awaitable);
   if (stopping()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "cannot update bootstrap peers after P2P node shutdown");
   }
   replace_bootstrap(std::move(peers));
   wake_retry_wait();
}

void bootstrap_service::start_maintenance(lifecycle_tracker& tracker) {
   auto operation = tracker.track();
   if (!operation.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "cannot start bootstrap maintenance after P2P node shutdown");
   }
   {
      const auto lock = std::scoped_lock{mutex_};
      if (maintenance_started_) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P bootstrap maintenance is already running");
      }
      maintenance_started_ = true;
   }
   const auto executor = operation.executor();
   auto self = shared_from_this();
   try {
      boost::asio::co_spawn(
          executor, async_maintenance(),
          [self = std::move(self), operation = std::move(operation)](std::exception_ptr error) mutable {
             static_cast<void>(error);
             static_cast<void>(self);
             operation.release();
          });
   } catch (...) {
      const auto lock = std::scoped_lock{mutex_};
      maintenance_started_ = false;
      throw;
   }
}

boost::asio::awaitable<void> bootstrap_service::async_maintenance() {
   auto degraded = false;
   // Health sampling copies endpoint state and calls an owner predicate. Its
   // diagnostic failure must not terminate the existing maintenance operation.
   try {
      degraded = configured_count() != 0 && connected_count() == 0;
      if (degraded && !stopping()) {
         ilog("forge.net.p2p", "P2P bootstrap connectivity degraded", ("event", "lifecycle.degraded"));
      }
   } catch (...) {}
   try {
      while (!stopping()) {
         const auto now = std::chrono::steady_clock::now();
         static_cast<void>(co_await async_run_batch(due_keys(now), std::nullopt, false));
         if (stopping()) {
            break;
         }
         try {
            const auto configured = configured_count();
            const auto connected = connected_count();
            const auto now_degraded = configured != 0 && connected == 0;
            if (now_degraded != degraded) {
               degraded = now_degraded;
               ilog("forge.net.p2p", "P2P bootstrap connectivity changed",
                    ("event", degraded ? "lifecycle.degraded" : "lifecycle.recovered")
                    ("configured_bootstrap", configured)("connected_bootstrap", connected));
            }
         } catch (...) {}
         try {
            co_await callbacks_.prune_peer_state();
         } catch (...) {
            if (!stopping()) {
               tlog("forge.net.p2p.discovery", "P2P peer persistence maintenance failed",
                    ("event", "maintenance.persistence_failed"));
            }
         }

         const auto observed = retry_wakeup_->epoch();
         const auto wait_started = std::chrono::steady_clock::now();
         const auto deadline = wait_started + next_maintenance_delay(wait_started);
         {
            const auto lock = std::scoped_lock{mutex_};
            if (stopping_) {
               break;
            }
         }
         static_cast<void>(co_await retry_wakeup_->async_wait_until(observed, deadline));
      }
   } catch (...) {
      if (!stopping() && !is_expected_cancellation(std::current_exception())) {
         wlog("forge.net.p2p.discovery", "P2P bootstrap maintenance stopped unexpectedly",
              ("event", "maintenance.stopped_unexpectedly"));
      }
   }
   const auto lock = std::scoped_lock{mutex_};
   maintenance_started_ = false;
}

void bootstrap_service::replace_bootstrap(std::vector<bootstrap_peer> peers) {
   auto replacements = std::map<std::string, entry>{};
   for (auto& peer : peers) {
      const auto key = key_for(peer);
      auto expected = std::optional<peer_id>{};
      const auto& components = peer.address.components();
      if (!components.empty() && components.back().code == forge::multiformats::protocol_code::p2p) {
         expected = peer_id::from_string(components.back().value);
      }
      replacements.emplace(key, entry{.configured = std::move(peer), .configured_peer = std::move(expected)});
   }

   auto cancellations = std::vector<std::shared_ptr<cancellation_latch>>{};
   auto unprotect = std::set<peer_id>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (stopping_) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "cannot update bootstrap peers after P2P node shutdown");
      }
      auto next_generation = next_generation_;
      for (auto& [key, value] : replacements) {
         const auto existing = entries_.find(key);
         if (existing != entries_.end()) {
            value.connected_peer = existing->second.connected_peer;
            value.protected_peer = existing->second.protected_peer;
            value.next_attempt = existing->second.next_attempt;
            value.failures = existing->second.failures;
            value.generation = existing->second.generation;
            value.active_cancellation = existing->second.active_cancellation;
         } else {
            value.generation = next_generation++;
         }
      }
      auto retained_peers = std::set<peer_id>{};
      for (const auto& [_, value] : replacements) {
         if (value.protected_peer) {
            retained_peers.insert(*value.protected_peer);
         } else if (value.configured_peer) {
            retained_peers.insert(*value.configured_peer);
         }
      }
      for (const auto& [key, value] : entries_) {
         if (replacements.contains(key)) {
            continue;
         }
         if (value.active_cancellation) {
            cancellations.push_back(value.active_cancellation);
         }
         if (value.protected_peer && !retained_peers.contains(*value.protected_peer)) {
            unprotect.insert(*value.protected_peer);
         } else if (value.protected_peer) {
            for (auto& [_, replacement] : replacements) {
               if (replacement.configured_peer == value.protected_peer) {
                  replacement.protected_peer = value.protected_peer;
                  break;
               }
            }
         }
      }
      auto configured = std::vector<bootstrap_peer>{};
      configured.reserve(replacements.size());
      for (const auto& [_, value] : replacements) {
         configured.push_back(value.configured);
      }
      // All copying/allocation precedes this no-throw publication.
      entries_.swap(replacements);
      options_.bootstrap.swap(configured);
      next_generation_ = next_generation;
   }
   for (const auto& cancellation : cancellations) {
      cancellation->request_stop();
   }
   for (const auto& peer : unprotect) {
      callbacks_.unprotect(peer);
   }
}

void bootstrap_service::cancel_attempts(const std::vector<std::string>& keys,
                                        std::optional<std::string> except) noexcept {
   auto cancellations = std::vector<std::shared_ptr<cancellation_latch>>{};
   try {
      {
         const auto lock = std::scoped_lock{mutex_};
         for (const auto& key : keys) {
            if (except && key == *except) {
               continue;
            }
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.active_cancellation) {
               cancellations.push_back(found->second.active_cancellation);
            }
         }
      }
      for (const auto& cancellation : cancellations) {
         cancellation->request_stop();
      }
   } catch (...) {
      // Node-level teardown remains the final cancellation fallback.
   }
}

void bootstrap_service::wake_retry_wait() noexcept {
   retry_wakeup_->notify();
}

void bootstrap_service::request_stop() noexcept {
   auto cancellations = std::vector<std::shared_ptr<cancellation_latch>>{};
   try {
      {
         const auto lock = std::scoped_lock{mutex_};
         if (stopping_) {
            return;
         }
         stopping_ = true;
         for (const auto& [_, value] : entries_) {
            if (value.active_cancellation) {
               cancellations.push_back(value.active_cancellation);
            }
         }
      }
      for (const auto& cancellation : cancellations) {
         cancellation->request_stop();
      }
   } catch (...) {
      // Node-level teardown remains the final cancellation fallback.
   }
   wake_retry_wait();
}

std::size_t bootstrap_service::connected_count() const {
   auto entries = std::vector<std::pair<bootstrap_peer, peer_id>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      entries.reserve(entries_.size());
      for (const auto& [_, value] : entries_) {
         const auto& peer = value.connected_peer ? value.connected_peer : value.configured_peer;
         if (peer) {
            entries.emplace_back(value.configured, *peer);
         }
      }
   }
   return static_cast<std::size_t>(std::ranges::count_if(
       entries, [&](const auto& value) { return callbacks_.connected(value.first, value.second); }));
}

std::size_t bootstrap_service::configured_count() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return entries_.size();
}

std::string bootstrap_service::last_failure() const {
   const auto lock = std::scoped_lock{mutex_};
   return last_failure_;
}

} // namespace forge::net::p2p::detail
