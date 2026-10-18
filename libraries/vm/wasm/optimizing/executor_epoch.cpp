module;

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

module forge.vm.wasm.optimizing.executor;

import forge.asio.notification;
import forge.asio.task;
import forge.vm.wasm.interpret.exceptions;
import forge.vm.wasm.optimizing.invocation_abi;

#include "details/executor_epoch.hxx"

namespace forge::vm::wasm::optimizing {

namespace {
// Check opportunity, not a new CPU accounting clock or a hard latency promise.
constexpr auto tick_quantum = std::chrono::milliseconds{1};
constexpr auto saturated = (std::numeric_limits<std::uint64_t>::max)();
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free &&
              std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::required_alignment <= 8 &&
              std::atomic_ref<std::uint32_t>::required_alignment <= 8);
} // namespace

executor_epoch::executor_epoch(asio::task::scheduler& scheduler) : _scheduler(scheduler) {}

executor_epoch::~executor_epoch() {
   // Freeing a driver with a live coroutine or guest backing pointer is an
   // owner-lifetime defect. Do not silently abandon a task or resurrect a pool.
   if (!_drained || _active.load(std::memory_order_seq_cst) != 0)
      std::terminate();
}

std::uint64_t executor_epoch::value() const noexcept {
   return std::atomic_ref{_epoch}.load(std::memory_order_acquire);
}

bool executor_epoch::ready() const noexcept {
   return _phase.load(std::memory_order_seq_cst) == phase::running && value() != saturated;
}

bool executor_epoch::failed() const noexcept {
   return _failed.load(std::memory_order_acquire) || value() == saturated;
}

void executor_epoch::bump() noexcept {
   auto word = std::atomic_ref{_epoch};
   auto observed = word.load(std::memory_order_acquire);
   while (observed != saturated) {
      if (word.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel, std::memory_order_acquire))
         return;
   }
   // Never reset/wrap: MAX forces every live generated comparison slow and
   // prevents new admission. Stop publication remains independent of ticks.
}

boost::asio::awaitable<void> executor_epoch::tick(asio::task::context& context) {
   auto error = std::exception_ptr{};
   try {
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      auto starting = phase::starting;
      if (!_phase.compare_exchange_strong(starting, phase::running, std::memory_order_seq_cst)) {
         _changed.notify();
         co_return;
      }
      _changed.notify();
      while (!context.cancel_requested() && !_stop.stop_requested()) {
         timer.expires_after(tick_quantum);
         auto timer_error = boost::system::error_code{};
         co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, timer_error));
         if (context.cancel_requested() || _stop.stop_requested())
            break;
         if (timer_error)
            throw boost::system::system_error{timer_error};
         bump();
         {
            const auto lock = std::lock_guard{_completion_mutex};
            const auto now = std::chrono::steady_clock::now();
            for (auto* active = _activations; active; active = active->_next)
               active->observe_deadline(now);
         }
      }
   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      _failed.store(true, std::memory_order_release);
      _phase.store(phase::failed, std::memory_order_seq_cst);
      static_cast<void>(_stop.request_stop());
      bump();
   } else {
      _phase.store(phase::stopped, std::memory_order_seq_cst);
   }
   _changed.notify();
   // The owned task completes normally even on driver failure. Lifecycle code
   // reports the published operational failure only after proving task drain.
}

boost::asio::awaitable<void> executor_epoch::start(std::chrono::steady_clock::time_point deadline,
                                                   std::stop_token stop) {
   auto idle = phase::idle;
   if (!_phase.compare_exchange_strong(idle, phase::starting, std::memory_order_seq_cst))
      throw interpret::exceptions::execution_unavailable{"executor epoch lifecycle was already started"};
   _drained = false;
   auto failure = std::exception_ptr{};
   try {
      _ticker = _scheduler.submit(asio::task::awaitable{
          .priority = asio::task::priority::max(),
          .name = "wasm.optimizing.epoch",
          .work = [this](asio::task::context& context) { return tick(context); },
      });
      if (!_ticker.accepted())
         throw interpret::exceptions::execution_unavailable{"executor epoch task was not admitted"};
      for (;;) {
         const auto observed = _changed.epoch();
         if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
            throw interpret::exceptions::execution_unavailable{"executor epoch startup expired or was stopped"};
         if (ready())
            co_return;
         if (_phase.load(std::memory_order_seq_cst) != phase::starting)
            throw interpret::exceptions::execution_unavailable{"executor epoch did not become ready"};
         static_cast<void>(co_await _changed.async_wait_until(observed, deadline, stop));
      }
   } catch (...) {
      failure = std::current_exception();
   }
   // A cancelled parent can reject a nested await before shutdown() enters its
   // own body. Enter mandatory cleanup in THIS coroutine before that await.
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   try {
      co_await shutdown();
   } catch (...) {
      if (!_drained)
         throw; // No completion proof: preserve the owner for another drain.
   }
   static_cast<void>(failure);
   throw interpret::exceptions::execution_unavailable{"executor epoch was unavailable during startup"};
}

boost::asio::awaitable<void> executor_epoch::shutdown() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   if (_drained)
      co_return;
   {
      const auto lock = std::lock_guard{_completion_mutex};
      if (_phase.load(std::memory_order_seq_cst) != phase::failed)
         _phase.store(phase::stopping, std::memory_order_seq_cst);
   }
   // Synchronous callbacks publish their sticky reason THEN an epoch RMW,
   // even if the scheduler/ticker already stopped. No timer is needed to exit
   // infinite generated code during compute.shutdown().
   static_cast<void>(_stop.request_stop());
   static_cast<void>(_ticker.cancel());
   try {
      if (_ticker.valid())
         co_await _ticker.wait();
   } catch (const asio::exceptions::canceled&) {
      // Pending cancellation never ran the ticker; active cancellation is
      // observed at its existing bounded timer wait, then completes normally.
   } catch (const asio::exceptions::rejected&) {
      if (_ticker.accepted())
         throw;
      // submit returned an already-completed rejected handle, no ticker ran.
   }
   // Other wait exceptions (including allocation/awaiter failure) are NOT
   // proof of completion. They propagate with _drained still false.
   for (;;) {
      // Read the notification epoch before the completion gate, without
      // retaining its internal lock. A retirement either precedes our zero
      // observation, or changes this epoch before async_wait subscribes.
      const auto observed = _changed.epoch();
      {
         const auto lock = std::lock_guard{_completion_mutex};
         if (_active.load(std::memory_order_seq_cst) == 0) {
            // Last retirement's entire notify tail already left this gate.
            // Stopping prevents a later admission and its late notification.
            _drained = true;
            break;
         }
      }
      static_cast<void>(co_await _changed.async_wait(observed));
   }
   if (_phase.load(std::memory_order_seq_cst) != phase::failed)
      _phase.store(phase::stopped, std::memory_order_seq_cst);
   if (failed())
      throw interpret::exceptions::execution_unavailable{"executor epoch driver failed or saturated"};
}

void executor_epoch::admit() {
   auto admitted = false;
   {
      const auto lock = std::lock_guard{_completion_mutex};
      if (ready()) {
         _active.fetch_add(1, std::memory_order_seq_cst);
         admitted = true;
      }
   }
   if (!admitted)
      throw interpret::exceptions::execution_unavailable{"executor epoch is not ready for native admission"};
}

void executor_epoch::retire() noexcept {
   // This short gate covers zero publication AND the final runtime-notify
   // access. shutdown observes zero under the same gate, not just an atomic
   // snapshot. notification never invokes user work inline; nested lock order
   // is completion -> notification, with no inverse retained lock anywhere.
   const auto lock = std::lock_guard{_completion_mutex};
   _active.fetch_sub(1, std::memory_order_seq_cst);
   _changed.notify();
}

executor_epoch::activation::activation(executor_epoch& owner, std::chrono::steady_clock::time_point deadline,
                                       std::stop_token caller_stop, std::stop_token compute_stop)
    : _owner(owner), _deadline(deadline) {
   _owner.admit();
   try {
      _caller_stop.emplace(caller_stop, stop_handler{this});
      _compute_stop.emplace(compute_stop, stop_handler{this});
      _executor_stop.emplace(_owner._stop.get_token(), stop_handler{this});
      {
         const auto lock = std::lock_guard{_owner._completion_mutex};
         _next = _owner._activations;
         _owner._activations = this;
         _linked = true;
      }
      static_cast<void>(poll());
   } catch (...) {
      _executor_stop.reset();
      _compute_stop.reset();
      _caller_stop.reset();
      _owner.retire();
      throw;
   }
}

executor_epoch::activation::~activation() {
   static_cast<void>(finish());
   _owner.retire();
}

trap_reason executor_epoch::activation::finish() noexcept {
   if (_finished)
      return reason();
   {
      const auto lock = std::lock_guard{_owner._completion_mutex};
      if (_linked) {
         auto** current = &_owner._activations;
         while (*current != this)
            current = &(*current)->_next;
         *current = _next;
         _linked = false;
      }
   }
   _executor_stop.reset();
   _compute_stop.reset();
   _caller_stop.reset();
   const auto result = poll();
   _finished = true;
   return result;
}

void executor_epoch::activation::stop_handler::operator()() const noexcept {
   owner->publish(trap_reason::cancelled);
}

trap_reason executor_epoch::activation::reason() const noexcept {
   return static_cast<trap_reason>(std::atomic_ref{_reason}.load(std::memory_order_acquire));
}

void executor_epoch::activation::publish(trap_reason reason) noexcept {
   auto expected = static_cast<std::uint32_t>(trap_reason::none);
   if (std::atomic_ref{_reason}.compare_exchange_strong(expected, static_cast<std::uint32_t>(reason),
                                                        std::memory_order_release, std::memory_order_relaxed)) {
      _owner.bump();
      static_cast<void>(_guest_stop.request_stop());
   }
}

std::stop_token executor_epoch::activation::guest_stop() const noexcept {
   return _guest_stop.get_token();
}

void executor_epoch::activation::observe_deadline(std::chrono::steady_clock::time_point now) noexcept {
   if (now >= _deadline)
      publish(trap_reason::deadline);
}

trap_reason executor_epoch::activation::poll() noexcept {
   // Load the epoch first. A stop published before this snapshot is visible
   // through its acquired RMW; a later bump satisfies the new snapshot+1.
   const auto snapshot = _owner.value();
   auto current = reason();
   if (current == trap_reason::none) {
      if (snapshot == saturated || _owner._stop.stop_requested())
         publish(trap_reason::cancelled);
      else if (std::chrono::steady_clock::now() >= _deadline)
         publish(trap_reason::deadline);
      current = reason();
   }
   return current;
}

} // namespace forge::vm::wasm::optimizing
