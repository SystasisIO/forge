module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <forge/exceptions/macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

module forge.app.plugin_lifecycle;

import forge.app.exceptions;
import forge.asio.notification;

#include "details/plugin_lifecycle_impl.hxx"

namespace forge::app {

plugin_lifecycle::plugin_lifecycle() : impl_{std::make_shared<impl>()} {}
plugin_lifecycle::~plugin_lifecycle() = default;
plugin_lifecycle::plugin_lifecycle(const plugin_lifecycle&) noexcept = default;
plugin_lifecycle& plugin_lifecycle::operator=(const plugin_lifecycle&) noexcept = default;

lifecycle_observer plugin_lifecycle::observer() const noexcept {
   return lifecycle_observer{impl_};
}

lifecycle_snapshot plugin_lifecycle::snapshot() const {
   return impl_->snapshot();
}

boost::asio::awaitable<plugin_lifecycle::operation>
plugin_lifecycle::begin(lifecycle_operation value) const {
   return begin_owned(impl_, value, false);
}

boost::asio::awaitable<plugin_lifecycle::operation>
plugin_lifecycle::begin(lifecycle_operation value, std::stop_token stop) const {
   return begin_owned(impl_, value, false, std::move(stop));
}

boost::asio::awaitable<plugin_lifecycle::operation>
plugin_lifecycle::begin_joined(lifecycle_operation value) const {
   return begin_owned(impl_, value, true);
}

boost::asio::awaitable<plugin_lifecycle::operation>
plugin_lifecycle::begin_owned(std::shared_ptr<impl> owner, lifecycle_operation value, bool join_input,
                              std::optional<std::stop_token> stop) {
   if (static_cast<std::size_t>(value) >= owner->succeeded.size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "invalid lifecycle operation");
   }
   const auto accepts_input = value == lifecycle_operation::configure ||
                              value == lifecycle_operation::provide ||
                              value == lifecycle_operation::initialize;
   auto cleanup_owner = operation{};
   for (;;) {
      auto joined = std::shared_ptr<attempt>{};
      auto observed = std::uint64_t{};
      auto same_operation = false;
      {
         const auto lock = std::scoped_lock{owner->mutex};
         if (value == lifecycle_operation::shutdown) {
            if (owner->succeeded[static_cast<std::size_t>(value)]) {
               co_return operation{};
            }
            if (!owner->owner_alive) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_state, "lifecycle owner was destroyed");
            }
            if (!owner->stop_requested) {
               // Composition owners without a stop hook can begin cleanup
               // directly. Plugin wrappers reserve and finish the stop hook.
               owner->stop_requested = true;
               ++owner->generation;
               owner->phase = lifecycle_phase::stopping;
               owner->changed.notify();
            }
            if (!cleanup_owner && !owner->cleanup) {
               auto pending = std::make_shared<attempt>(attempt{
                   .value = value,
                   .id = ++owner->next_attempt,
                   .generation = owner->generation,
                   .abandoned_error = std::make_exception_ptr(
                       exceptions::invalid_state{"lifecycle operation was abandoned"}),
                   .stopped_error = std::make_exception_ptr(
                       exceptions::operation_stopped{"lifecycle operation was stopped"})});
               owner->cleanup = pending;
               cleanup_owner = operation{owner, std::move(pending)};
               owner->changed.notify();
            }
            if (cleanup_owner) {
               if (!owner->active && !owner->stop_hook_running) {
                  owner->active = owner->cleanup;
                  owner->changed.notify();
                  co_return std::move(cleanup_owner);
               }
            } else {
               joined = owner->cleanup;
               same_operation = true;
            }
         } else {
            if (!owner->owner_alive || owner->stop_requested || owner->failure) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_state, "lifecycle admission is closed");
            }
            if (owner->succeeded[static_cast<std::size_t>(value)]) {
               if (accepts_input && !join_input) {
                  FORGE_THROW_EXCEPTION(exceptions::invalid_state, "lifecycle input operation already completed");
               }
               co_return operation{};
            }
            if (owner->active) {
               joined = owner->active;
               same_operation = joined->value == value;
               if (same_operation && accepts_input && !join_input) {
                  FORGE_THROW_EXCEPTION(exceptions::invalid_state, "lifecycle input operation already in progress");
               }
            } else {
               const auto before_initialization = owner->phase == lifecycle_phase::created ||
                                                  owner->phase == lifecycle_phase::configured;
               const auto after_initialization = owner->phase == lifecycle_phase::initialized;
               if (((value == lifecycle_operation::configure || value == lifecycle_operation::provide ||
                     value == lifecycle_operation::initialize) && !before_initialization) ||
                   ((value == lifecycle_operation::after_initialize || value == lifecycle_operation::startup) &&
                    !after_initialization)) {
                  FORGE_THROW_EXCEPTION(exceptions::invalid_state, "invalid lifecycle operation order");
               }
               auto pending = std::make_shared<attempt>(attempt{
                   .value = value,
                   .id = ++owner->next_attempt,
                   .generation = owner->generation,
                   .abandoned_error = std::make_exception_ptr(
                       exceptions::invalid_state{"lifecycle operation was abandoned"}),
                   .stopped_error = std::make_exception_ptr(
                       exceptions::operation_stopped{"lifecycle operation was stopped"})});
               owner->active = pending;
               if (value == lifecycle_operation::initialize) {
                  owner->phase = lifecycle_phase::initializing;
               } else if (value == lifecycle_operation::startup) {
                  owner->phase = lifecycle_phase::starting;
               }
               owner->changed.notify();
               co_return operation{owner, std::move(pending)};
            }
         }
         observed = owner->changed.epoch();
      }

      if (cleanup_owner) {
         // Only the accepted owner drains independently. Low-level joined
         // callers remain cancellable and cannot alter the owned attempt.
         const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
         co_await boost::asio::this_coro::throw_if_cancelled(false);
         auto failure = std::exception_ptr{};
         try {
            co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor,
                drain_cleanup(owner, cleanup_owner.pending_),
                boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
         } catch (...) {
            failure = std::current_exception();
            cleanup_owner.complete(failure);
         }
         co_await boost::asio::this_coro::throw_if_cancelled(throws);
         if (failure) {
            std::rethrow_exception(failure);
         }
         co_return std::move(cleanup_owner);
      }
      try {
         if (!same_operation) {
            static_cast<void>(stop ? co_await owner->changed.async_wait(observed, *stop)
                                  : co_await owner->changed.async_wait(observed));
         } else {
            // Keep the joined attempt alive across later retries. Inspect the
            // terminal result under the same lock that publishes it.
            for (;;) {
               auto completed = false;
               auto canceled = false;
               auto error = std::exception_ptr{};
               {
                  const auto lock = std::scoped_lock{owner->mutex};
                  completed = joined->completed;
                  error = joined->error;
                  observed = owner->changed.epoch();
                  canceled = stop && stop->stop_requested();
               }
               if (completed) {
                  if (error) {
                     std::rethrow_exception(error);
                  }
                  co_return operation{};
               }
               if (canceled) {
                  throw boost::system::system_error{boost::asio::error::operation_aborted};
               }
               try {
                  static_cast<void>(stop ? co_await owner->changed.async_wait(observed, *stop)
                                        : co_await owner->changed.async_wait(observed));
               } catch (const boost::system::system_error& waiting_error) {
                  if (waiting_error.code() != boost::asio::error::operation_aborted) {
                     throw;
                  }
                  auto terminal = false;
                  auto terminal_error = std::exception_ptr{};
                  {
                     const auto lock = std::scoped_lock{owner->mutex};
                     terminal = joined->completed;
                     terminal_error = joined->error;
                  }
                  // A published attempt result wins cancellation, including
                  // cancellation during notification's executor restoration.
                  if (!terminal) {
                     throw;
                  }
                  if (terminal_error) {
                     std::rethrow_exception(terminal_error);
                  }
                  co_return operation{};
               }
            }
         }
      } catch (...) {
         cleanup_owner.complete(std::current_exception());
         throw;
      }
   }
}

boost::asio::awaitable<void> plugin_lifecycle::drain_cleanup(std::shared_ptr<impl> owner,
                                                           std::shared_ptr<attempt> pending) {
   for (;;) {
      auto observed = std::uint64_t{};
      {
         const auto lock = std::scoped_lock{owner->mutex};
         if (!owner->owner_alive) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_state, "lifecycle owner was destroyed");
         }
         if (owner->cleanup != pending || pending->completed) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_state, "lifecycle cleanup attempt is no longer current");
         }
         if (!owner->active && !owner->stop_hook_running) {
            owner->active = owner->cleanup;
            owner->changed.notify();
            co_return;
         }
         observed = owner->changed.epoch();
      }
      static_cast<void>(co_await owner->changed.async_wait(observed));
   }
}

bool plugin_lifecycle::request_stop() const noexcept {
   return impl_->request_stop();
}

void plugin_lifecycle::complete_stop() const noexcept {
   impl_->complete_stop();
}

void plugin_lifecycle::abandon_owner() const noexcept {
   impl_->abandon_owner();
}

bool plugin_lifecycle::try_admit(std::uint64_t generation,
                                const std::function<void()>& install) const {
   const auto lock = std::scoped_lock{impl_->mutex};
   if (generation != impl_->generation || !impl_->admits()) {
      return false;
   }
   install();
   return true;
}

bool plugin_lifecycle::try_admit(std::uint64_t generation, lifecycle_phase required_phase,
                                const std::function<void()>& install) const {
   const auto lock = std::scoped_lock{impl_->mutex};
   if (generation != impl_->generation || !impl_->admits() || impl_->phase != required_phase) {
      return false;
   }
   install();
   return true;
}

plugin_lifecycle::admission plugin_lifecycle::admit(std::uint64_t generation, std::atomic_size_t& active,
                                                  std::size_t limit, forge::asio::notification& changed) const {
   return admit_owned(generation, std::nullopt, active, limit, changed);
}

plugin_lifecycle::admission plugin_lifecycle::admit(std::uint64_t generation, lifecycle_phase required_phase,
                                                  std::atomic_size_t& active, std::size_t limit,
                                                  forge::asio::notification& changed) const {
   return admit_owned(generation, required_phase, active, limit, changed);
}

plugin_lifecycle::admission plugin_lifecycle::admit_owned(std::uint64_t generation,
                                                        std::optional<lifecycle_phase> required_phase,
                                                        std::atomic_size_t& active, std::size_t limit,
                                                        forge::asio::notification& changed) const {
   const auto lock = std::scoped_lock{impl_->mutex};
   if (generation != impl_->generation || !impl_->admits() || (required_phase && impl_->phase != *required_phase)) {
      return admission{admission_status::closed};
   }
   if (active.load() >= limit) {
      return admission{admission_status::exhausted};
   }
   active.fetch_add(1);
   return admission{admission_status::accepted, &active, &changed};
}

plugin_lifecycle::admission::admission() noexcept = default;
plugin_lifecycle::admission::~admission() {
   release();
}

plugin_lifecycle::admission::admission(admission_status status, std::atomic_size_t* active,
                                      forge::asio::notification* changed) noexcept
    : status_{status}, active_{active}, changed_{changed} {}

plugin_lifecycle::admission::admission(admission&& other) noexcept
    : status_{std::exchange(other.status_, admission_status::closed)},
      active_{std::exchange(other.active_, nullptr)}, changed_{std::exchange(other.changed_, nullptr)} {}

plugin_lifecycle::admission& plugin_lifecycle::admission::operator=(admission&& other) noexcept {
   if (this != &other) {
      release();
      status_ = std::exchange(other.status_, admission_status::closed);
      active_ = std::exchange(other.active_, nullptr);
      changed_ = std::exchange(other.changed_, nullptr);
   }
   return *this;
}

admission_status plugin_lifecycle::admission::status() const noexcept {
   return status_;
}

bool plugin_lifecycle::admission::active() const noexcept {
   return active_ != nullptr;
}

void plugin_lifecycle::admission::release() noexcept {
   auto* active = std::exchange(active_, nullptr);
   auto* changed = std::exchange(changed_, nullptr);
   if (active != nullptr) {
      active->fetch_sub(1);
      changed->notify();
   }
}

plugin_lifecycle::operation::operation() noexcept = default;
plugin_lifecycle::operation::~operation() {
   abandon();
}

plugin_lifecycle::operation::operation(operation&& other) noexcept
    : owner_{std::move(other.owner_)}, pending_{std::move(other.pending_)} {}

plugin_lifecycle::operation& plugin_lifecycle::operation::operator=(operation&& other) noexcept {
   if (this != &other) {
      abandon();
      owner_ = std::move(other.owner_);
      pending_ = std::move(other.pending_);
   }
   return *this;
}

plugin_lifecycle::operation::operation(std::shared_ptr<impl> owner,
                                      std::shared_ptr<attempt> pending) noexcept
    : owner_{std::move(owner)}, pending_{std::move(pending)} {}

plugin_lifecycle::operation::operator bool() const noexcept {
   return static_cast<bool>(pending_);
}

std::uint64_t plugin_lifecycle::operation::generation() const noexcept {
   return pending_ ? pending_->generation : 0;
}

std::exception_ptr plugin_lifecycle::operation::complete(std::exception_ptr error) noexcept {
   if (pending_) {
      error = owner_->finish(pending_, std::move(error));
      pending_.reset();
      owner_.reset();
   }
   return error;
}

void plugin_lifecycle::operation::abandon() noexcept {
   if (pending_) {
      complete(pending_->abandoned_error);
   }
}

lifecycle_observer::lifecycle_observer(std::shared_ptr<plugin_lifecycle::impl> owner) noexcept
    : owner_{std::move(owner)} {}

lifecycle_snapshot lifecycle_observer::snapshot() const {
   return owner_->snapshot();
}

boost::asio::awaitable<lifecycle_snapshot>
lifecycle_observer::async_wait(std::uint64_t observed_epoch, std::stop_token stop) const {
   return wait_owned(owner_, observed_epoch, std::chrono::steady_clock::time_point::max(), std::move(stop));
}

boost::asio::awaitable<lifecycle_snapshot> lifecycle_observer::async_wait_until(
    std::uint64_t observed_epoch, std::chrono::steady_clock::time_point deadline,
    std::stop_token stop) const {
   return wait_owned(owner_, observed_epoch, deadline, std::move(stop));
}

boost::asio::awaitable<lifecycle_snapshot> lifecycle_observer::wait_owned(
    std::shared_ptr<plugin_lifecycle::impl> owner, std::uint64_t observed_epoch,
    std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   if (const auto current = owner->snapshot(); !current.owner_alive || current.phase == lifecycle_phase::stopped) {
      co_return current;
   }
   // An explicit token opts into stop_token cancellation; otherwise preserve
   // Boost.Asio associated coroutine cancellation through notification.
   try {
      if (stop.stop_possible()) {
         static_cast<void>(co_await owner->changed.async_wait_until(observed_epoch, deadline, std::move(stop)));
      } else {
         static_cast<void>(co_await owner->changed.async_wait_until(observed_epoch, deadline));
      }
   } catch (const boost::system::system_error& error) {
      if (error.code() == boost::asio::error::operation_aborted) {
         const auto current = owner->snapshot();
         if (!current.owner_alive || current.phase == lifecycle_phase::stopped) {
            co_return current;
         }
      }
      throw;
   }
   const auto current = owner->snapshot();
   if (current.epoch == observed_epoch && current.owner_alive && current.phase != lifecycle_phase::stopped) {
      throw boost::system::system_error{boost::asio::error::timed_out};
   }
   co_return current;
}

} // namespace forge::app
