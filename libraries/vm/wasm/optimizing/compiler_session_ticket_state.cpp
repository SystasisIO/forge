module;

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

module forge.vm.wasm.optimizing.compiler_session;

#include "details/compiler_session_ticket_state.hxx"

namespace forge::vm::wasm::optimizing {

// Terminal outcome storage never creates an exception or an owning buffer.
// Existing shared artifact pins and timestamps are nonthrowing value copies;
// notification::notify() owns its noexcept, allocation-free wake path.
static_assert(std::is_nothrow_copy_constructible_v<compiler_session::completion>);
static_assert(std::is_nothrow_move_constructible_v<compiler_session::completion>);
static_assert(std::is_nothrow_move_assignable_v<std::exception_ptr>);

compiler_session::ticket::state::state() = default;

compiler_session::ticket::state::~state() = default;

bool compiler_session::ticket::state::cancel() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      if (completed)
         return false;
      cancelled = true;
      completed = true;
   }
   changed.notify();
   return true;
}

void compiler_session::ticket::state::complete(completion value) noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      if (completed)
         return;
      result.value.emplace(std::move(value));
      completed = true;
   }
   changed.notify();
}

void compiler_session::ticket::state::fail(std::exception_ptr error) noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      if (completed)
         return;
      result.error = std::move(error);
      completed = true;
   }
   changed.notify();
}

} // namespace forge::vm::wasm::optimizing
