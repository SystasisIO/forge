module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <setjmp.h>
#include <signal.h>
#include <span>
#include <type_traits>
#include <utility>

export module forge.vm.wasm.interpret.backend:signals;

import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.debug_info;
import forge.vm.wasm.interpret.exceptions;
import forge.vm.wasm.interpret.host_function;
import forge.vm.wasm.interpret.options;
import forge.vm.wasm.interpret.types;
import forge.vm.wasm.interpret.watchdog;

export namespace forge::vm::wasm::interpret {

enum class native_signal_fault : std::uint8_t { none, memory, stack, arithmetic, code, explicit_exit };

struct native_signal_ranges {
   std::span<const std::byte> code;
   std::span<const std::byte> memory;
   std::span<const std::byte> stack_guard;
};

enum class signal_boundary_stage : std::uint8_t {
   none,
   entry,
   platform,
   signal_stack,
   active_stack,
   block_entry,
   query_stack,
   install_stack,
   acquire_handlers,
   enter_mask,
   block_exit,
   restore_stack,
   release_handlers,
   restore_mask,
   thread_prepare,
   thread_release,
   thread_capacity,
   recovery_not_ready,
   fork_child,
   mach_service,
   mach_routes
};

struct signal_boundary_status {
   signal_boundary_stage stage = signal_boundary_stage::none;
   int error = 0;
   std::uint32_t rollback_failed = 0;
};

struct native_signal_result {
   native_signal_fault fault = native_signal_fault::none;
   int signal = 0;
   signal_boundary_status status;
};

using signal_recovery_entry = void (*)(void*) noexcept;

/// Cold calling-thread lifecycle. On Darwin ARM64 the reserve remains borrowed until release/thread exit;
/// production compute worker hooks prepare before opening writer-held execution attempts.
/// Linux preparation/release are no-ops: native entry still uses its supplied actual POSIX alternate stack.
[[nodiscard]] signal_boundary_status prepare_signal_recovery_thread(std::span<std::byte> recovery_stack) noexcept;
[[nodiscard]] signal_boundary_status release_signal_recovery_thread() noexcept;

/// Enter destructor-free native code using explicit fault ownership and a caller-owned recovery stack.
/// Darwin ARM64 requires explicit preparation and the same prepared reserve (also for nested entry).
/// Linux uses the supplied reserve as its actual per-invocation POSIX alternate signal stack.
/// Empty code ranges never recover foreign faults. Linux SIGBUS is not managed by this owner.
[[nodiscard]] native_signal_result invoke_native_with_signals(native_signal_ranges ranges,
                                                              std::span<std::byte> signal_stack,
                                                              signal_recovery_entry entry, void* state) noexcept;

/// Leave generated code only after all C++ callbacks have returned. The invocation owner holds the trap reason.
[[noreturn]] void leave_native_with_signals() noexcept;

/// Suspend VM recovery, not OS signals. Construct and destroy host/numeric RAII inside callback.
/// The complete noexcept callback catches into caller-owned storage while recovery is NULL.
/// Failed OS restoration is fail-stop: borrowed signal stacks cannot be freed while still referenced.
[[nodiscard]] signal_boundary_status invoke_without_signal_recovery(signal_recovery_entry entry, void* state) noexcept;

} // namespace forge::vm::wasm::interpret

namespace forge::vm::wasm::interpret {

namespace detail {
// Private transport, not an interruption reason. Only the existing owned-code
// recovery branch can create it; a timed scope must also match this mapping
// and have its own fired cause before translating it.
struct code_interruption : exceptions::memory {
   std::span<const std::byte> code;
   explicit code_interruption(std::span<const std::byte> mapping)
       : exceptions::memory{"wasm memory out-of-bounds"}, code(mapping) {}
};

// Execution-thread-only stack, borrowed until guard callbacks are terminal.
// Typed scopes reject overlapping protection of the SAME mapping; distinct
// nested mappings retain independent reasons and protection lifetimes.
struct timed_run_scope {
   std::span<const std::byte> code;
   bool typed;
   timed_run_scope* previous;
};
__attribute__((visibility("default"))) extern thread_local timed_run_scope* active_timed_run;

inline bool contains_address(std::span<const std::byte> range, std::uintptr_t address) noexcept {
   if (range.empty())
      return false;

   const auto start = reinterpret_cast<std::uintptr_t>(range.data());
   return address >= start && address - start < range.size();
}
} // namespace detail

// Fixes a duplicate symbol build issue when building with `-fvisibility=hidden`
__attribute__((visibility("default"))) extern thread_local std::atomic<sigjmp_buf*> signal_dest;

__attribute__((visibility("default"))) extern thread_local std::span<std::byte> code_memory_range;

__attribute__((visibility("default"))) extern thread_local std::span<std::byte> memory_range;

// Sole publication truth. Increment before protecting code, restore code
// before decrement. Signal readers never consume a separately stored flag.
__attribute__((visibility("default"))) extern thread_local std::atomic<std::size_t> timed_run_timeout_count;

struct legacy_signal_outcome {
   std::exception_ptr exception;
   signal_boundary_status status;
};

__attribute__((visibility("default"))) extern thread_local legacy_signal_outcome* current_legacy_outcome;

void check_signal_boundary_status(signal_boundary_status status);
[[noreturn]] void jump_to_signal_recovery(int result) noexcept;

void signal_handler(int signal, siginfo_t* info, void* context);

// only valid inside invoke_with_signal_handler.
// This is a workaround for the fact that it
// is currently unsafe to throw an exception through
// a jit frame.
template <typename F> inline void longjmp_on_exception(F&& f) {
   static_assert(std::is_trivially_destructible_v<std::decay_t<F>>,
                 "longjmp has undefined behavior when it bypasses destructors.");
   struct callback_state {
      std::remove_reference_t<F>* function;
      legacy_signal_outcome* outcome;
      bool failed;
   };
   const auto callback = [](void* opaque) noexcept {
      auto& state = *static_cast<callback_state*>(opaque);
      try {
         (*state.function)();
      } catch (...) {
         state.outcome->exception = std::current_exception();
         state.failed = true;
      }
   };
   if (std::atomic_load(&signal_dest) == nullptr) {
      // Even a standalone/imported-only host call requires the owner mask.
      // This outcome lives above the raw suspension, with no active ancestor
      // checkpoint; ordinary rethrow preserves nested local C++ catches.
      legacy_signal_outcome outcome;
      auto state = callback_state{&f, &outcome, false};
      const auto status = invoke_without_signal_recovery(callback, &state);
      check_signal_boundary_status(status);
      if (outcome.exception)
         std::rethrow_exception(outcome.exception);
      return;
   }
   if (current_legacy_outcome == nullptr)
      std::abort();
   auto state = callback_state{&f, current_legacy_outcome, false};
   const auto status = invoke_without_signal_recovery(callback, &state);
   if (status.stage != signal_boundary_stage::none) {
      state.outcome->status = status;
      state.failed = true;
   }
   // The complete callback/catch/unwind has finished; only trivial state remains before the existing transport.
   if (state.failed) {
      jump_to_signal_recovery(-1);
   }
}

template <typename E> [[noreturn]] inline void throw_(const char* msg) {
   longjmp_on_exception([msg] { throw E{msg}; });
   std::abort();
}

// Preserves the donor internal test hook. Production ownership is scoped by acquire/release below.
void setup_signal_handler_impl();
void acquire_signal_handler_scope();
void release_signal_handler_scope();
native_signal_result invoke_legacy_signal_handler(signal_recovery_entry callback, void* state,
                                                  legacy_signal_outcome* outcome, std::span<const std::byte> code,
                                                  std::span<const std::byte> memory) noexcept;

/// Private wrapper: caller recovery must be NULL throughout outcome lifetime and the typed tail.
/// Call a function with a signal handler installed.  If this thread is
/// signalled during the execution of f, the function e will be called with
/// the signal number as an argument.  If f creates any automatic variables
/// with non-trivial destructors, then it must mask the relevant signals
/// during the lifetime of these objects or the behavior is undefined.
///
/// signals handled: SIGSEGV, SIGBUS (except on Linux), SIGFPE
///
/// Components that replace these process-wide handlers must not mutate them
/// while an outer VM invocation is active. POSIX does not provide an atomic
/// compare-and-restore operation for signal dispositions.
///
// Make this noinline to prevent possible corruption of the caller's local variables.
// It's unlikely, but I'm not sure that it can definitely be ruled out if both
// this and f are inlined and f modifies locals from the caller.
template <typename F, typename E>
[[gnu::noinline]] auto invoke_with_signal_handler(F&& f, E&& e, growable_allocator* code_allocator,
                                                  wasm_allocator* mem_allocator) {
   if (std::atomic_load(&signal_dest) != nullptr)
      std::abort();
   legacy_signal_outcome outcome;
   const auto next_code_memory_range = code_allocator ? code_allocator->get_code_span() : std::span<std::byte>{};
   const auto next_memory_range = mem_allocator ? mem_allocator->get_span() : std::span<std::byte>{};
   auto callback = [&f, &outcome]() noexcept {
      try {
         f();
      } catch (...) {
         signal_dest.store(nullptr);
         outcome.exception = std::current_exception();
      }
   };
   const auto result =
       invoke_legacy_signal_handler([](void* state) noexcept { (*static_cast<decltype(callback)*>(state))(); },
                                    &callback, &outcome, next_code_memory_range, next_memory_range);
   check_signal_boundary_status(result.status);
   check_signal_boundary_status(outcome.status);
   if (outcome.exception) {
      std::rethrow_exception(outcome.exception);
   } else if (result.signal != 0) {
      if constexpr (std::is_invocable_v<E&, const native_signal_result&, std::span<const std::byte>>)
         e(result, next_code_memory_range);
      else
         e(result.signal); // Preserve the private donor/custom int-only hook.
   }
}

template <typename F, typename E>
auto invoke_with_signal_handler(F&& f, E&& e, growable_allocator& code_allocator, wasm_allocator* mem_allocator) {
   return invoke_with_signal_handler(std::forward<F>(f), std::forward<E>(e), &code_allocator, mem_allocator);
}

} // namespace forge::vm::wasm::interpret
