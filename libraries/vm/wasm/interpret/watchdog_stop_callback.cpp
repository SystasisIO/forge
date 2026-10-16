#include "details/watchdog_stop_callback.hxx"

#include <utility>

namespace forge::vm::wasm::interpret::detail {

// Keep callback lifetime operations outside named modules: LLVM 22 can omit
// libc++'s inline constexpr lock helper when stop_callback is instantiated there.
watchdog_stop_callback::watchdog_stop_callback(std::stop_token stop, void (*notify)(void*) noexcept,
                                               void* context) noexcept
    : _callback(std::move(stop), notification{notify, context}) {}

watchdog_stop_callback::~watchdog_stop_callback() = default;

void watchdog_stop_callback::notification::operator()() const noexcept {
   notify(context);
}

} // namespace forge::vm::wasm::interpret::detail
