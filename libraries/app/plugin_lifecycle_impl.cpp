module;

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

module forge.app.plugin_lifecycle;

import forge.asio.notification;

#include "details/plugin_lifecycle_impl.hxx"

namespace forge::app {

lifecycle_snapshot plugin_lifecycle::impl::snapshot() const {
   const auto lock = std::scoped_lock{mutex};
   auto completed_operations = std::uint8_t{};
   for (auto index = std::size_t{}; index < succeeded.size(); ++index) {
      if (succeeded[index]) {
         completed_operations |= static_cast<std::uint8_t>(1U << index);
      }
   }
   return {.phase = phase,
           .epoch = changed.epoch(),
           .generation = generation,
           .stop_requested = stop_requested,
           .admission_open = admits(),
           .owner_alive = owner_alive,
           .completed_operations = completed_operations,
           .in_progress = active ? std::optional{active->value}
                                 : cleanup ? std::optional{cleanup->value} : std::nullopt,
           .failure = failure};
}

bool plugin_lifecycle::impl::admits() const noexcept {
   return owner_alive && !stop_requested && !failure;
}

bool plugin_lifecycle::impl::request_stop() noexcept {
   const auto lock = std::scoped_lock{mutex};
   if (stop_requested) {
      return false;
   }
   stop_requested = true;
   stop_hook_running = true;
   ++generation;
   phase = lifecycle_phase::stopping;
   changed.notify();
   return true;
}

void plugin_lifecycle::impl::complete_stop() noexcept {
   const auto lock = std::scoped_lock{mutex};
   if (stop_hook_running) {
      stop_hook_running = false;
      changed.notify();
   }
}

void plugin_lifecycle::impl::abandon_owner() noexcept {
   const auto lock = std::scoped_lock{mutex};
   if (!owner_alive) {
      return;
   }
   owner_alive = false;
   if (!stop_requested) {
      stop_requested = true;
      ++generation;
   }
   if (phase != lifecycle_phase::stopped) {
      phase = lifecycle_phase::stopping;
   }
   changed.notify();
}

std::exception_ptr plugin_lifecycle::impl::finish(const std::shared_ptr<attempt>& pending,
                                                 std::exception_ptr error) noexcept {
   auto retired_failure = std::optional<lifecycle_failure>{};
   auto retired_active = std::shared_ptr<attempt>{};
   auto retired_cleanup = std::shared_ptr<attempt>{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (pending->completed) {
         return pending->error;
      }
      if (!error && pending->value != lifecycle_operation::shutdown &&
          (pending->generation != generation || stop_requested || !owner_alive)) {
         error = pending->stopped_error;
      }
      pending->error = std::move(error);
      pending->completed = true;
      if (pending->error) {
         retired_failure = std::move(failure);
         failure = lifecycle_failure{.operation = pending->value,
                                     .attempt = pending->id,
                                     .exception = pending->error};
      } else {
         succeeded[static_cast<std::size_t>(pending->value)] = true;
         if (pending->value == lifecycle_operation::shutdown) {
            phase = lifecycle_phase::stopped;
         } else if (!stop_requested && owner_alive) {
            switch (pending->value) {
            case lifecycle_operation::configure:
               phase = lifecycle_phase::configured;
               break;
            case lifecycle_operation::initialize:
               phase = lifecycle_phase::initialized;
               break;
            case lifecycle_operation::startup:
               phase = lifecycle_phase::started;
               break;
            default:
               break;
            }
         }
      }
      if (active == pending) {
         retired_active.swap(active);
      }
      if (cleanup == pending) {
         retired_cleanup.swap(cleanup);
      }
      // notification only cancels registered Asio timers; it invokes no inline
      // continuations. Publish the snapshot and attempt error before waking.
      changed.notify();
      error = pending->error;
   }
   // Arbitrary exception destructors can re-enter the lifecycle or logger.
   // Retire the old failure and attempt ownership after releasing the lock.
   return error;
}

} // namespace forge::app
