#include "details/watchdog_stop_callback.hxx"

#include <utility>

namespace forge::vm::wasm::interpret::detail {

// Keep callback lifetime operations outside named modules: LLVM 22 can omit
// libc++'s inline constexpr lock helper when stop_callback is instantiated there.
watchdog_stop_callback::watchdog_stop_callback(std::stop_token stop, std::mutex& mutex,
                                             std::condition_variable& condition) noexcept
    : _callback(std::move(stop), notification{&mutex, &condition}) {}

watchdog_stop_callback::~watchdog_stop_callback() = default;

void watchdog_stop_callback::notification::operator()() const noexcept {
   // The wait mutex closes the gap between checking stop and entering the wait.
   const auto lock = std::lock_guard{*mutex};
   condition->notify_one();
}

} // namespace forge::vm::wasm::interpret::detail
