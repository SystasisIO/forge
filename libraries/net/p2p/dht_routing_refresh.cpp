module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.crypto.digest.sha256;
import forge.net.p2p.dht;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;

#include "details/dht_routing_refresh.hxx"
#include "details/cancellation_latch.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p::detail {
namespace {

[[nodiscard]] std::size_t common_prefix_length(const dht::distance& value) noexcept {
   auto result = std::size_t{};
   for (const auto byte : value.bytes) {
      if (byte == 0) {
         result += 8;
         continue;
      }
      result += static_cast<std::size_t>(std::countl_zero(byte));
      break;
   }
   return std::min(result, std::size_t{255});
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
   for (auto shift = 56; shift >= 0; shift -= 8) {
      output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
   }
}

[[nodiscard]] std::uint64_t stable_hash(const protocol_id& protocol, std::uint64_t generation) noexcept {
   auto result = std::uint64_t{1469598103934665603ULL};
   for (const auto byte : protocol.value) {
      result ^= static_cast<std::uint8_t>(byte);
      result *= 1099511628211ULL;
   }
   result ^= generation;
   result *= 1099511628211ULL;
   return result;
}

[[nodiscard]] std::chrono::milliseconds saturating_milliseconds_add(std::chrono::milliseconds value,
                                                                      std::chrono::milliseconds addition) noexcept {
   if (addition <= std::chrono::milliseconds::zero()) {
      return value;
   }
   const auto available = (std::chrono::milliseconds::max)() - value;
   return addition >= available ? (std::chrono::milliseconds::max)() : value + addition;
}

[[nodiscard]] dht_routing_refresh::time_point saturating_deadline(dht_routing_refresh::time_point now,
                                                                    std::chrono::milliseconds delay) noexcept {
   if (delay <= std::chrono::milliseconds::zero() || now == dht_routing_refresh::time_point::max()) {
      return now;
   }
   // Converting milliseconds::max directly to steady_clock::duration can
   // overflow before the addition is checked. Compare in the coarser unit.
   const auto available = dht_routing_refresh::time_point::max() - now;
   const auto available_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(available);
   if (delay >= available_milliseconds) {
      return dht_routing_refresh::time_point::max();
   }
   return now + std::chrono::duration_cast<dht_routing_refresh::time_point::duration>(delay);
}

} // namespace

dht_routing_refresh::dht_routing_refresh(peer_id local, std::vector<profile> profiles, query_callback query,
                                         time_source time)
    : local_{std::move(local)}, query_{std::move(query)}, time_{std::move(time)},
      changed_{std::make_shared<lifecycle_wakeup>()} {
   if (!query_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "DHT routing refresh query callback is required");
   }
   if (!time_.now) {
      time_.now = [] { return std::chrono::steady_clock::now(); };
   }
   if (!time_.wait_until) {
      time_.wait_until = [](std::shared_ptr<lifecycle_wakeup> wakeup, std::uint64_t observed,
                            time_point deadline) -> boost::asio::awaitable<std::uint64_t> {
         if (deadline == time_point::max()) {
            co_return co_await wakeup->async_wait(observed);
         }
         co_return co_await wakeup->async_wait_until(observed, deadline);
      };
   }
   profiles_.reserve(profiles.size());
   for (auto& value : profiles) {
      if (value.routing == nullptr || value.interval <= std::chrono::milliseconds::zero() ||
          value.query_timeout <= std::chrono::milliseconds::zero()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "DHT routing refresh profile is invalid");
      }
      profiles_.push_back(profile_state{.config = std::move(value)});
   }
}

dht_routing_refresh::~dht_routing_refresh() {
   request_stop();
}

bool dht_routing_refresh::stopped() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return stopped_;
}

void dht_routing_refresh::notify_verified_server() noexcept {
   changed_->notify();
}

void dht_routing_refresh::request_stop() noexcept {
   auto stop = std::shared_ptr<worker_stop_bridge>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (stopped_) {
         return;
      }
      stopped_ = true;
      stop = active_query_stop_;
   }
   if (stop) {
      stop->request_stop();
   }
   changed_->notify();
}

std::optional<dht_routing_refresh::profile_status> dht_routing_refresh::status(const protocol_id& protocol) const {
   const auto now = time_.now();
   auto lock = std::scoped_lock{mutex_};
   const auto current =
       std::ranges::find_if(profiles_, [&](const auto& value) { return value.config.protocol == protocol; });
   if (current == profiles_.end()) {
      return std::nullopt;
   }
   return profile_status{
       .startup_lookup_pending = current->startup_lookup_pending,
       .in_flight = current->in_flight,
       .failures = current->failures,
       .next_attempt_in = current->next_attempt > now
                              ? std::chrono::duration_cast<std::chrono::milliseconds>(current->next_attempt - now)
                              : std::chrono::milliseconds{0},
   };
}

dht::key dht_routing_refresh::refresh_target(const profile_state& state, std::size_t common_prefix_length_value) const {
   if (common_prefix_length_value > max_refresh_common_prefix_length) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "DHT routing refresh CPL exceeds preimage limit");
   }

   auto seed = local_.to_bytes();
   seed.insert(seed.end(), state.config.protocol.value.begin(), state.config.protocol.value.end());
   append_u64(seed, common_prefix_length_value);
   append_u64(seed, state.generation);
   for (auto attempt = std::size_t{}; attempt < max_preimage_attempts; ++attempt) {
      auto input = seed;
      append_u64(input, attempt);
      const auto digest_value = forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{input});
      const auto digest = digest_value.to_uint8_span();
      auto preimage = std::vector<std::uint8_t>{0x12U, 0x20U};
      preimage.insert(preimage.end(), digest.begin(), digest.end());
      if (common_prefix_length(distance_between(local_.to_bytes(), preimage)) == common_prefix_length_value) {
         return dht::key{.bytes = std::move(preimage)};
      }
   }
   FORGE_THROW_EXCEPTION(exceptions::internal, "DHT routing refresh exhausted bounded Peer ID preimage search");
}

std::chrono::milliseconds dht_routing_refresh::regular_delay(const profile_state& state) const noexcept {
   const auto span = std::max(std::chrono::milliseconds{1}, state.config.interval / 20);
   const auto width = static_cast<std::uint64_t>(span.count()) + 1U;
   const auto offset = static_cast<std::int64_t>(stable_hash(state.config.protocol, state.generation) % width);
   return saturating_milliseconds_add(state.config.interval, std::chrono::milliseconds{offset});
}

std::chrono::milliseconds dht_routing_refresh::retry_delay(const profile_state& state) const noexcept {
   const auto exponent = std::min<std::uint32_t>(state.failures > 0 ? state.failures - 1U : 0U, 6U);
   const auto base = std::chrono::seconds{std::uint64_t{1} << exponent};
   const auto cap = std::max(
       std::chrono::seconds{1},
       std::min(std::chrono::seconds{60}, std::chrono::duration_cast<std::chrono::seconds>(state.config.interval / 4)));
   const auto bounded = std::min(base, cap);
   const auto jitter_bound = std::max<std::int64_t>(1, bounded.count() / 5);
   const auto jitter = stable_hash(state.config.protocol, state.generation + state.failures) %
                       static_cast<std::uint64_t>(jitter_bound + 1);
   return std::chrono::duration_cast<std::chrono::milliseconds>(bounded + std::chrono::seconds{jitter});
}

void dht_routing_refresh::publish_status(profile_state& state, bool in_flight) {
   auto lock = std::scoped_lock{mutex_};
   state.in_flight = in_flight;
}

boost::asio::awaitable<bool> dht_routing_refresh::async_query(protocol_id protocol, dht::key target,
                                                               std::chrono::milliseconds timeout) {
   auto stop = std::make_shared<worker_stop_bridge>();
   {
      const auto lock = std::scoped_lock{mutex_};
      if (stopped_) {
         co_return false;
      }
      active_query_stop_ = stop;
   }

   const auto clear_stop = [this, &stop] {
      const auto lock = std::scoped_lock{mutex_};
      if (active_query_stop_ == stop) {
         active_query_stop_.reset();
      }
   };
   try {
      auto result = std::make_shared<std::optional<bool>>();
      auto cancellation = std::make_shared<cancellation_latch>();
      auto query = worker_stop_work{
          [this, protocol = std::move(protocol), target = std::move(target), timeout, result,
           cancellation](std::shared_ptr<worker_terminal_owner> terminal) mutable -> boost::asio::awaitable<void> {
             static_cast<void>(terminal->publish(
                 worker_terminal_owner::callback{[cancellation]() noexcept { cancellation->request_stop(); }}));
             if (cancellation->stop_requested()) {
                co_return;
             }
             *result = co_await query_(std::move(protocol), std::move(target), timeout, cancellation);
          },
      };
      co_await async_run_with_stop_bridge(stop, std::move(query));
      clear_stop();
      co_return !stop->stop_requested() && result->value_or(false);
   } catch (...) {
      clear_stop();
      throw;
   }
}

boost::asio::awaitable<void> dht_routing_refresh::async_refresh_profile(profile_state& state) {
   publish_status(state, true);
   try {
      auto failed = false;
      auto should_run_startup_lookup = false;
      {
         auto lock = std::scoped_lock{mutex_};
         should_run_startup_lookup = state.startup_lookup_pending;
      }
      if (should_run_startup_lookup) {
         auto startup_lookup_pending = true;
         try {
            startup_lookup_pending =
                !co_await async_query(state.config.protocol, make_dht_key(local_), state.config.query_timeout);
            failed = startup_lookup_pending;
         } catch (...) {
            failed = true;
         }
         {
            auto lock = std::scoped_lock{mutex_};
            state.startup_lookup_pending = startup_lookup_pending;
         }
      }

      const auto planned_at = time_.now();
      const auto plan = state.config.routing->plan_refresh(planned_at, state.config.interval);
      for (const auto& bucket : plan) {
         if (stopped()) {
            publish_status(state, false);
            co_return;
         }
         if (bucket.common_prefix_length > max_refresh_common_prefix_length) {
            continue;
         }
         try {
            const auto target = refresh_target(state, bucket.common_prefix_length);
            if (co_await async_query(state.config.protocol, target, state.config.query_timeout)) {
               static_cast<void>(state.config.routing->mark_refreshed(bucket, time_.now()));
            } else {
               failed = true;
            }
         } catch (...) {
            failed = true;
         }
      }

      {
         auto lock = std::scoped_lock{mutex_};
         ++state.generation;
         if (failed) {
            state.failures = std::min<std::uint32_t>(state.failures + 1U, 7U);
            state.next_attempt = saturating_deadline(time_.now(), retry_delay(state));
         } else {
            state.failures = 0;
            state.next_attempt = saturating_deadline(time_.now(), regular_delay(state));
         }
         state.in_flight = false;
      }
   } catch (...) {
      publish_status(state, false);
      throw;
   }
}

boost::asio::awaitable<void> dht_routing_refresh::async_run() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto observed = changed_->epoch();
   while (!stopped()) {
      const auto now = time_.now();
      auto ran = false;
      for (auto& state : profiles_) {
         if (stopped()) {
            co_return;
         }
         if (state.config.routing->status().active == 0) {
            continue;
         }
         auto due = false;
         {
            auto lock = std::scoped_lock{mutex_};
            due = state.next_attempt == std::chrono::steady_clock::time_point{} ||
                  (state.next_attempt != std::chrono::steady_clock::time_point::max() && state.next_attempt <= now);
         }
         if (due) {
            co_await async_refresh_profile(state);
            ran = true;
         }
      }
      if (ran) {
         observed = changed_->epoch();
         continue;
      }

      auto deadline = std::chrono::steady_clock::time_point::max();
      for (const auto& state : profiles_) {
         if (state.config.routing->status().active != 0) {
            auto lock = std::scoped_lock{mutex_};
            if (state.next_attempt != std::chrono::steady_clock::time_point{}) {
               deadline = std::min(deadline, state.next_attempt);
            }
         }
      }
      if (deadline == std::chrono::steady_clock::time_point::max()) {
         observed = co_await time_.wait_until(changed_, observed, time_point::max());
      } else {
         observed = co_await time_.wait_until(changed_, observed, deadline);
      }

      const auto wake_now = time_.now();
      for (auto& state : profiles_) {
         const auto active = state.config.routing->status().active != 0;
         auto lock = std::scoped_lock{mutex_};
         if (state.startup_lookup_pending && active && state.next_attempt == std::chrono::steady_clock::time_point{}) {
            state.next_attempt = wake_now;
         }
      }
   }
}

} // namespace forge::net::p2p::detail
