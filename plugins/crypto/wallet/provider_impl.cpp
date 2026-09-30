module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

module forge.plugins.crypto.wallet.provider;

import forge.api.core.exceptions;
import forge.asio.gate;
import forge.asio.exceptions;
import forge.asio.task;
import forge.asio.compute;
import forge.crypto.keystore.store;
import forge.crypto.wallet.exceptions;

#include "details/provider_impl.hxx"

namespace forge::plugins::crypto::wallet {
namespace management = forge::crypto::wallet;

provider::impl::impl(std::string value) : name{std::move(value)} {
   const auto safe = [](unsigned char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
   };
   if (name.empty() || name.size() > 64) {
      FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Invalid wallet name");
   }
   for (const auto ch : name) {
      if (!safe(static_cast<unsigned char>(ch))) {
         FORGE_THROW_EXCEPTION(management::exceptions::invalid_request, "Invalid wallet name");
      }
   }
}

void provider::impl::require_initialized() const {
   if (!scheduler || stopping.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(management::exceptions::unavailable, "Wallet provider is unavailable");
   }
}

void provider::impl::reserve() {
   require_initialized();
   if (pending.fetch_add(1, std::memory_order_acq_rel) >= max_pending) {
      pending.fetch_sub(1, std::memory_order_release);
      FORGE_THROW_EXCEPTION(management::exceptions::resource_exhausted, "Wallet queue is full");
   }
}

boost::asio::awaitable<forge::asio::gate::ticket> provider::impl::enter_reserved() {
   forge::asio::gate::ticket ticket;
   try {
      ticket = co_await gate.acquire();
   } catch (...) {
      pending.fetch_sub(1, std::memory_order_release);
      throw;
   }
   pending.fetch_sub(1, std::memory_order_release);
   co_return std::move(ticket);
}

boost::asio::awaitable<forge::asio::gate::ticket> provider::impl::enter() {
   reserve();
   auto ticket = co_await enter_reserved();
   require_initialized();
   expire_if_due();
   co_return std::move(ticket);
}

void provider::impl::require_unlocked() const {
   if (state == management::state::fault) {
      FORGE_THROW_EXCEPTION(management::exceptions::storage_error, "Wallet requires operator recovery");
   }
   if (state != management::state::unlocked || !store || !accepting_signatures.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(management::exceptions::locked, "Wallet is locked");
   }
}

management::wallet_status provider::impl::snapshot() const {
   return {.wallet = name, .status = state, .timeout_seconds = timeout};
}

void provider::impl::expire_if_due() {
   if (state == management::state::unlocked && timeout != 0 && std::chrono::steady_clock::now() >= deadline) {
      accepting_signatures.store(false, std::memory_order_release);
      lock_generation.fetch_add(1, std::memory_order_acq_rel);
      store.reset();
      state = management::state::locked;
      cancel_expiry();
   }
}

void provider::impl::cancel_expiry() {
   timer.cancel();
   ++timer_generation;
   timer_scheduled = false;
}

void provider::impl::refresh() {
   deadline = timeout == 0 ? std::chrono::steady_clock::time_point::max()
                           : std::chrono::steady_clock::now() + std::chrono::seconds{timeout};
   if (timeout == 0 && timer_scheduled) {
      cancel_expiry();
   }
   if (state == management::state::unlocked && timeout != 0 && (!timer_scheduled || deadline < timer_due)) {
      schedule_expiry();
   }
}

void provider::impl::schedule_expiry() {
   if (timer_scheduled) {
      timer.cancel();
   }
   const auto generation = ++timer_generation;
   const auto delay = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
   timer = scheduler->submit_after(
       forge::asio::task::awaitable{
           .name = "wallet.idle",
           .work = [weak = weak_from_this(),
                    generation](forge::asio::task::context& context) -> boost::asio::awaitable<void> {
              if (auto self = weak.lock(); self && !context.cancel_requested()) {
                 co_await self->expire(generation);
              }
           },
       },
       std::max(delay, std::chrono::milliseconds{0}));
   timer_scheduled = timer.accepted();
   timer_due = deadline;
   if (!timer_scheduled) {
      accepting_signatures.store(false, std::memory_order_release);
      lock_generation.fetch_add(1, std::memory_order_acq_rel);
      store.reset();
      state = management::state::locked;
      FORGE_THROW_EXCEPTION(management::exceptions::resource_exhausted, "Wallet idle task was rejected");
   }
}

boost::asio::awaitable<void> provider::impl::expire(std::uint64_t generation) {
   if (stopping.load(std::memory_order_acquire)) {
      co_return;
   }
   forge::asio::gate::ticket ticket;
   try {
      ticket = co_await gate.acquire();
   } catch (const forge::asio::exceptions::rejected&) {
      if (stopping.load(std::memory_order_acquire)) {
         co_return;
      }
      throw;
   }
   if (stopping.load(std::memory_order_acquire) || generation != timer_generation) {
      co_return;
   }
   timer_scheduled = false;
   if (state != management::state::unlocked || timeout == 0) {
      co_return;
   }
   expire_if_due();
   if (state == management::state::unlocked) {
      // Ordinary successful use only moves the deadline. Keep one pending task
      // instead of accumulating a canceled delayed task after every signature.
      schedule_expiry();
   }
}

} // namespace forge::plugins::crypto::wallet
