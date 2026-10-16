module;

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

#include "details/watchdog_stop_callback.hxx"

module forge.vm.wasm.interpret.watchdog;

namespace forge::vm::wasm::interpret {

// The private native pair owns every std::stop_callback lifetime operation;
// no registration is instantiated in this named-module implementation.
struct watchdog::guard::stop_callbacks {
   stop_callbacks(interrupt_options options, guard& owner)
       : cancel(options.cancel, notify, &owner), external(options.external, notify, &owner) {}

   static void notify(void* owner) noexcept {
      static_cast<guard*>(owner)->request();
   }

   // Reverse destruction joins external before cancel, outside guard::_mutex.
   detail::watchdog_stop_callback cancel;
   detail::watchdog_stop_callback external;
};

watchdog::watchdog(std::chrono::steady_clock::time_point deadline, interrupt_options options)
    : _deadline(deadline), _options(options) {}

std::chrono::steady_clock::time_point watchdog::deadline() const noexcept {
   if (_deadline)
      return *_deadline;
   const auto start = std::chrono::steady_clock::now();
   if (_duration <= std::chrono::steady_clock::duration::zero())
      return start;
   if (_duration >= std::chrono::steady_clock::time_point::max() - start)
      return std::chrono::steady_clock::time_point::max();
   return start + _duration;
}

watchdog::guard::guard(std::chrono::steady_clock::time_point deadline, interrupt_options options,
                       std::function<void(interruption)> callback, delivery policy)
    : _callback(std::move(callback)), _deadline(deadline), _options(options), _policy(policy) {
   _stops = std::make_unique<stop_callbacks>(_options, *this);
   // Registration can synchronously run a callback. Start the existing single
   // timer thread only after every field it borrows has been initialized.
   _timer = std::thread(&guard::runner, this);
}

bool watchdog::guard::update(bool admission, bool timer_delivery) {
   auto lock = std::unique_lock(_mutex);
   if (_closed && _reason == interruption::none)
      return false;
   // One-shot, sticky claim. The original absolute deadline is never extended.
   if (_reason == interruption::none) {
      _reason = _options.cancel.stop_requested()                ? interruption::canceled
                : std::chrono::steady_clock::now() >= _deadline ? interruption::deadline
                : _options.external.stop_requested()            ? interruption::interrupted
                                                                : interruption::none;
      if (_reason == interruption::none)
         return admission;
   }
   const auto cause = _reason;
   // Canonical duration/stop only claims and wakes here: user code may call
   // request_stop while holding a mutex needed by its async callback.
   if (_delivered || (!timer_delivery && (_policy == delivery::timer || cause == interruption::deadline))) {
      lock.unlock();
      _cond.notify_all();
      return false;
   }
   _delivered = true;
   lock.unlock();
   _cond.notify_all();
   // Neither invocation nor unregister/join holds the callback's mutex.
   try {
      _callback(cause);
   } catch (...) {
      std::terminate();
   }
   return false;
}

bool watchdog::guard::enter() {
   return update(true);
}

void watchdog::guard::request(bool timer_delivery) noexcept {
   try {
      static_cast<void>(update(false, timer_delivery));
   } catch (...) {
      std::terminate();
   }
}

watchdog::guard::~guard() {
   // Claim expiry even if the timer did not run before Work returned, but
   // preserve the duration API's TIMER-thread delivery during destruction.
   request();
   {
      auto lock = std::unique_lock(_mutex);
      _closed = true;
   }
   _cond.notify_all();
   // A concurrent callback can still borrow backend/TLS state. Wait for both
   // stop handlers and the timer OUTSIDE _mutex before returning that state.
   _stops.reset();
   _timer.join();
}

void watchdog::guard::runner() {
   auto lock = std::unique_lock(_mutex);
   _cond.wait_until(lock, _deadline, [&] { return _closed || _reason != interruption::none; });
   lock.unlock();
   request(true);
}

} // namespace forge::vm::wasm::interpret
