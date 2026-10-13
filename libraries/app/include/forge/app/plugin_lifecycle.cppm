module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

export module forge.app.plugin_lifecycle;

export namespace forge::app {

enum class lifecycle_phase {
   created,
   configured,
   initializing,
   initialized,
   starting,
   started,
   stopping,
   stopped,
};

enum class lifecycle_operation {
   configure,
   provide,
   initialize,
   after_initialize,
   startup,
   shutdown,
};

struct lifecycle_failure {
   lifecycle_operation operation;
   std::uint64_t attempt = 0;
   std::exception_ptr exception;
};

struct lifecycle_snapshot {
   lifecycle_phase phase = lifecycle_phase::created;
   std::uint64_t epoch = 0;
   std::uint64_t generation = 1;
   bool stop_requested = false;
   bool admission_open = true;
   bool owner_alive = true;
   std::uint8_t completed_operations = 0;
   std::optional<lifecycle_operation> in_progress;
   // Historical last failure; successful cleanup does not erase its cause.
   std::optional<lifecycle_failure> failure;

   [[nodiscard]] constexpr bool completed(lifecycle_operation value) const noexcept {
      const auto index = static_cast<unsigned>(value);
      return index < 6 && (completed_operations & (1U << index)) != 0;
   }
};

class lifecycle_observer;

// Copy this shared handle into the owner's private implementation. Concrete
// resources and rollback remain the owner's responsibility, not the lifecycle's.
class plugin_lifecycle {
 public:
   class operation;

   plugin_lifecycle();
   ~plugin_lifecycle();
   plugin_lifecycle(const plugin_lifecycle&) noexcept;
   plugin_lifecycle& operator=(const plugin_lifecycle&) noexcept;

   [[nodiscard]] lifecycle_observer observer() const noexcept;
   [[nodiscard]] lifecycle_snapshot snapshot() const;

   // configure/provide/initialize are one-shot: repeated or concurrent calls
   // are rejected rather than silently ignoring different inputs. Concurrent
   // after_initialize/startup/shutdown join an attempt; a joined or already
   // successful operation returns an inactive token and runs no hook.
   boost::asio::awaitable<operation> begin(lifecycle_operation value) const;
   // Owner-only ensure for immutable bootstrap inputs. Unlike the strict
   // plugin input entrypoints, this atomically joins/skips an existing attempt.
   // Never use it to accept a new document, provider or plugin context.
   boost::asio::awaitable<operation> begin_joined(lifecycle_operation value) const;

   // The first caller must call complete_stop() after its synchronous stop
   // hook. Shutdown waits for that hook as well as the active async operation.
   [[nodiscard]] bool request_stop() const noexcept;
   void complete_stop() const noexcept;

   // Destruction is not cleanup: invalidate and wake observers without
   // claiming stopped. Explicit async shutdown is required before destruction.
   void abandon_owner() const noexcept;

   // Admission includes setup in created/configured; backend API readiness is
   // a separate owner concern. Only synchronous installation is permitted:
   // no await, lifecycle re-entry,
   // virtual calls, teardown or user callbacks inside install.
   [[nodiscard]] bool try_admit(std::uint64_t generation,
                                const std::function<void()>& install) const;
   [[nodiscard]] bool try_admit(std::uint64_t generation, lifecycle_phase required_phase,
                                const std::function<void()>& install) const;

 private:
   struct impl;
   struct attempt;
   std::shared_ptr<impl> impl_;

   static boost::asio::awaitable<operation> begin_owned(std::shared_ptr<impl> owner,
                                                        lifecycle_operation value, bool join_input);

   friend class lifecycle_observer;
};

class plugin_lifecycle::operation {
 public:
   operation() noexcept;
   ~operation();
   operation(operation&& other) noexcept;
   operation& operator=(operation&& other) noexcept;
   operation(const operation&) = delete;
   operation& operator=(const operation&) = delete;

   [[nodiscard]] explicit operator bool() const noexcept;
   [[nodiscard]] std::uint64_t generation() const noexcept;
   // Publish the result before releasing any coalesced callers. Completing a
   // failed shutdown leaves stopping and permits a later cleanup attempt.
   // Late success after stop is normalized to operation_stopped (invalid_state).
   // The owner must
   // propagate the returned error, which coalesced callers also receive.
   std::exception_ptr complete(std::exception_ptr error = {}) noexcept;

 private:
   operation(std::shared_ptr<impl> owner, std::shared_ptr<attempt> pending) noexcept;
   void abandon() noexcept;

   std::shared_ptr<impl> owner_;
   std::shared_ptr<attempt> pending_;

   friend class plugin_lifecycle;
};

// Observation confers no transition, stop or resource-admission authority.
class lifecycle_observer {
 public:
   [[nodiscard]] lifecycle_snapshot snapshot() const;
   // A terminal snapshot (stopped or destroyed owner) completes immediately,
   // even at its current epoch. Terminal completion wins cancellation.
   boost::asio::awaitable<lifecycle_snapshot> async_wait(
       std::uint64_t observed_epoch, std::stop_token stop = {}) const;
   boost::asio::awaitable<lifecycle_snapshot> async_wait_until(
       std::uint64_t observed_epoch, std::chrono::steady_clock::time_point deadline,
       std::stop_token stop = {}) const;

 private:
   explicit lifecycle_observer(std::shared_ptr<plugin_lifecycle::impl> owner) noexcept;
   static boost::asio::awaitable<lifecycle_snapshot> wait_owned(
       std::shared_ptr<plugin_lifecycle::impl> owner, std::uint64_t observed_epoch,
       std::chrono::steady_clock::time_point deadline, std::stop_token stop);

   std::shared_ptr<plugin_lifecycle::impl> owner_;

   friend class plugin_lifecycle;
};

} // namespace forge::app
