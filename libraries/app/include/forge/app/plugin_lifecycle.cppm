module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

export module forge.app.plugin_lifecycle;

import forge.asio.notification;

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

enum class admission_status { accepted, closed, exhausted };

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
   class admission;

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
   // Shutdown-owner draining is protected without resetting caller cancellation
   // state or throw policy; low-level joiners remain cancellable. A composition
   // owner must isolate its whole stop/cleanup hook, not just this admission.
   boost::asio::awaitable<operation> begin(lifecycle_operation value) const;
   // Isolated cleanup facades can forward only waiter cancellation. An accepted
   // shutdown owner ignores this token while draining; joiners remain cancellable.
   boost::asio::awaitable<operation> begin(lifecycle_operation value, std::stop_token stop) const;
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

   // Bounded service work, with no waiting or product error policy. Counter and
   // notification are borrowed and must outlive every accepted token. Use this
   // lifecycle for all admissions to the counter; only token release decrements it.
   // The no-phase overload admits setup work like try_admit(generation, install);
   // the exact-phase overload additionally requires that phase. Count/limit use
   // size_t without narrowing the owner's configured service limit.
   [[nodiscard]] admission admit(std::uint64_t generation, std::atomic_size_t& active,
                                 std::size_t limit, forge::asio::notification& changed) const;
   [[nodiscard]] admission admit(std::uint64_t generation, lifecycle_phase required_phase,
                                 std::atomic_size_t& active, std::size_t limit,
                                 forge::asio::notification& changed) const;

 private:
   struct impl;
   struct attempt;
   std::shared_ptr<impl> impl_;

   [[nodiscard]] admission admit_owned(std::uint64_t generation, std::optional<lifecycle_phase> required_phase,
                                       std::atomic_size_t& active, std::size_t limit,
                                       forge::asio::notification& changed) const;

   static boost::asio::awaitable<operation> begin_owned(std::shared_ptr<impl> owner,
       lifecycle_operation value, bool join_input, std::optional<std::stop_token> stop = {});
   static boost::asio::awaitable<void> drain_cleanup(std::shared_ptr<impl> owner,
                                                    std::shared_ptr<attempt> pending);

   friend class lifecycle_observer;
};

class plugin_lifecycle::admission {
 public:
   admission() noexcept;
   ~admission();
   admission(admission&& other) noexcept;
   admission& operator=(admission&& other) noexcept;
   admission(const admission&) = delete;
   admission& operator=(const admission&) = delete;

   // Result of this admission attempt; release does not rewrite the result.
   [[nodiscard]] admission_status status() const noexcept;
   [[nodiscard]] bool active() const noexcept;
   void release() noexcept;

 private:
   admission(admission_status status, std::atomic_size_t* active = nullptr,
              forge::asio::notification* changed = nullptr) noexcept;

   admission_status status_ = admission_status::closed;
   std::atomic_size_t* active_ = nullptr;
   forge::asio::notification* changed_ = nullptr;

   friend class plugin_lifecycle;
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
