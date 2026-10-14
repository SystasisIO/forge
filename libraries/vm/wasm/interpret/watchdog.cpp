module;

#include "details/watchdog_stop_callback.hxx"

#include <condition_variable>
#include <mutex>
#include <stop_token>

module forge.vm.wasm.interpret.watchdog;

namespace forge::vm::wasm::interpret {

watchdog::guard::~guard() {
   {
      auto lock = std::unique_lock(_mutex);
      if (_run_state == running && !_stop.stop_requested() && std::chrono::steady_clock::now() < deadline()) {
         _run_state = stopped;
      }
   }
   _cond.notify_one();
   _timer.join();
}

void watchdog::guard::runner() {
   // Register before locking: an already requested stop invokes this inline.
   detail::watchdog_stop_callback wake_on_stop{_stop, _mutex, _cond};
   auto lock = std::unique_lock(_mutex);
   _cond.wait_until(lock, deadline(), [&]() { return _run_state != running || _stop.stop_requested(); });
   if (_run_state == running) {
      _run_state = interrupted;
      const auto reason = _stop.stop_requested() ? interruption::canceled : interruption::deadline;
      lock.unlock();
      _callback(reason);
   }
}

std::chrono::steady_clock::time_point watchdog::guard::deadline() const noexcept {
   if (_duration <= duration_type::zero())
      return _start;
   if (_duration >= time_point_type::max() - _start)
      return time_point_type::max();
   return _start + _duration;
}

} // namespace forge::vm::wasm::interpret
