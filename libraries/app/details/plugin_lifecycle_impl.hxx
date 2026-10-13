#pragma once

#include "plugin_lifecycle_attempt.hxx"

namespace forge::app {

struct plugin_lifecycle::impl {
   [[nodiscard]] lifecycle_snapshot snapshot() const;
   [[nodiscard]] bool admits() const noexcept;
   [[nodiscard]] bool request_stop() noexcept;
   void complete_stop() noexcept;
   void abandon_owner() noexcept;
   std::exception_ptr finish(const std::shared_ptr<attempt>& pending, std::exception_ptr error) noexcept;

   mutable std::mutex mutex;
   forge::asio::notification changed;
   lifecycle_phase phase = lifecycle_phase::created;
   std::uint64_t generation = 1;
   std::uint64_t next_attempt = 0;
   bool stop_requested = false;
   bool stop_hook_running = false;
   bool owner_alive = true;
   std::array<bool, 6> succeeded{};
   std::optional<lifecycle_failure> failure;
   std::shared_ptr<attempt> active;
   // Reserve cleanup before draining an active operation so all concurrent
   // shutdown callers join the same attempt, including its eventual failure.
   std::shared_ptr<attempt> cleanup;
};

} // namespace forge::app
