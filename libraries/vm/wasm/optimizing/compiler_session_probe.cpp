#include "details/compiler_session_probe.hxx"

#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace forge::vm::wasm::optimizing::detail {

struct compiler_session_probe::state {
   state(point value, std::chrono::steady_clock::time_point limit) : selected(value), deadline(limit) {}
   point selected;
   std::chrono::steady_clock::time_point deadline;
   std::mutex mutex;
   std::condition_variable changed;
   observation observed;
};

std::mutex compiler_session_probe::_mutex;
std::shared_ptr<compiler_session_probe::state> compiler_session_probe::_active;

compiler_session_probe::compiler_session_probe(point selected, std::chrono::steady_clock::time_point deadline) {
   if (deadline <= std::chrono::steady_clock::now() || deadline == std::chrono::steady_clock::time_point::max())
      throw std::invalid_argument{"Compiler session probe requires a finite future deadline"};
   _state = std::make_shared<state>(selected, deadline);
   const auto lock = std::scoped_lock{_mutex};
   if (_active)
      throw std::logic_error{"Compiler session probe requires an exclusive test subprocess"};
   _active = _state;
}

compiler_session_probe::~compiler_session_probe() {
   close();
}

compiler_session_probe::observation compiler_session_probe::wait(std::size_t expected) {
   if (expected == 0 || expected > _state->observed.snapshots.size())
      throw std::invalid_argument{"Compiler session probe supports one or two paused jobs"};
   auto lock = std::unique_lock{_state->mutex};
   if (!_state->changed.wait_until(lock, _state->deadline,
                                   [&] { return _state->observed.arrived >= expected || _state->observed.released; }) ||
       std::chrono::steady_clock::now() >= _state->deadline) {
      _state->observed.timed_out = true;
      _state->observed.released = true;
      _state->changed.notify_all();
   }
   return _state->observed;
}

void compiler_session_probe::close() noexcept {
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_active == _state)
         _active.reset();
   }
   {
      const auto lock = std::scoped_lock{_state->mutex};
      _state->observed.released = true;
   }
   _state->changed.notify_all();
}

void compiler_session_probe::reservations_held(snapshot value) noexcept {
   pause(point::reservations_held, value);
}
void compiler_session_probe::prepared_before_return(snapshot value) noexcept {
   pause(point::prepared_before_return, value);
}

void compiler_session_probe::pause(point reached, snapshot value) noexcept {
   // The default is null. Pin before touching state: scope close may detach and
   // destroy the controller while this worker is still entering or leaving.
   auto observer = std::shared_ptr<state>{};
   {
      const auto lock = std::scoped_lock{_mutex};
      observer = _active;
   }
   if (!observer || observer->selected != reached)
      return;
   auto lock = std::unique_lock{observer->mutex};
   auto& observed = observer->observed;
   if (observed.released)
      return;
   if (observed.arrived == observed.snapshots.size()) {
      observed.overflow = true;
      observed.released = true;
      observer->changed.notify_all();
      return;
   }
   observed.snapshots[observed.arrived++] = value;
   ++observed.paused;
   observer->changed.notify_all();
   if (!observer->changed.wait_until(lock, observer->deadline, [&] { return observed.released; })) {
      observed.timed_out = true;
      observed.released = true;
      observer->changed.notify_all();
   }
   --observed.paused;
}

} // namespace forge::vm::wasm::optimizing::detail
