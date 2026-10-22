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

namespace forge::vm::wasm::interpret {

namespace detail {
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

__attribute__((visibility("default"))) extern thread_local std::atomic<bool> timed_run_has_timed_out;

__attribute__((visibility("default"))) extern thread_local std::atomic<std::size_t> timed_run_timeout_count;

// Fixes a duplicate symbol build issue when building with `-fvisibility=hidden`
__attribute__((visibility("default"))) extern thread_local std::exception_ptr saved_exception;

void signal_handler(int signal, siginfo_t* info, void* context);

// only valid inside invoke_with_signal_handler.
// This is a workaround for the fact that it
// is currently unsafe to throw an exception through
// a jit frame.
template <typename F> inline void longjmp_on_exception(F&& f) {
   static_assert(std::is_trivially_destructible_v<std::decay_t<F>>,
                 "longjmp has undefined behavior when it bypasses destructors.");
   bool caught_exception = false;
   try {
      f();
   } catch (...) {
      saved_exception = std::current_exception();
      // Cannot safely longjmp from inside the catch,
      // as that will leak the exception.
      caught_exception = true;
   }
   if (caught_exception) {
      sigset_t block_mask;
      sigemptyset(&block_mask);
      sigaddset(&block_mask, SIGPROF);
      pthread_sigmask(SIG_BLOCK, &block_mask, nullptr);
      sigjmp_buf* dest = std::atomic_load(&signal_dest);
      siglongjmp(*dest, -1);
   }
}

template <typename E> [[noreturn]] inline void throw_(const char* msg) {
   saved_exception = std::make_exception_ptr(E{msg});
   sigset_t block_mask;
   sigemptyset(&block_mask);
   sigaddset(&block_mask, SIGPROF);
   pthread_sigmask(SIG_BLOCK, &block_mask, nullptr);
   sigjmp_buf* dest = std::atomic_load(&signal_dest);
   siglongjmp(*dest, -1);
}

// Preserves the donor internal test hook. Production ownership is scoped by acquire/release below.
void setup_signal_handler_impl();
void acquire_signal_handler_scope();
void release_signal_handler_scope();

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
[[gnu::noinline]] auto invoke_with_signal_handler(F&& f, E&& e, std::span<std::byte> code_range,
                                                  wasm_allocator* mem_allocator) {
   const auto next_code_memory_range = code_range;
   const auto next_memory_range = mem_allocator ? mem_allocator->get_span() : std::span<std::byte>{};
   const bool owns_process_signal_scope = std::atomic_load(&signal_dest) == nullptr;
   if (owns_process_signal_scope) {
      acquire_signal_handler_scope();
   }
   sigjmp_buf dest;
   sigjmp_buf* volatile old_signal_handler = nullptr;
   const auto old_code_memory_range = code_memory_range;
   const auto old_memory_range = memory_range;
   code_memory_range = next_code_memory_range;
   memory_range = next_memory_range;
   int sig;
   if ((sig = sigsetjmp(dest, 1)) == 0) {
      // Note: Cannot use RAII, as non-trivial destructors w/ longjmp
      // have undefined behavior. [csetjmp.syn]
      //
      // Warning: The order of operations is critical here.
      // We also have to register signal_dest before unblocking
      // signals to make sure that only our signal handler is executed
      // if the caller has previously blocked signals.
      old_signal_handler = std::atomic_exchange(&signal_dest, &dest);
      sigset_t unblock_mask, old_sigmask; // Might not be preserved across longjmp
      sigemptyset(&unblock_mask);
      sigaddset(&unblock_mask, SIGSEGV);
      sigaddset(&unblock_mask, SIGBUS);
      sigaddset(&unblock_mask, SIGFPE);
      sigaddset(&unblock_mask, SIGPROF);
      pthread_sigmask(SIG_UNBLOCK, &unblock_mask, &old_sigmask);
      try {
         f();
         pthread_sigmask(SIG_SETMASK, &old_sigmask, nullptr);
         std::atomic_store(&signal_dest, old_signal_handler);
         code_memory_range = old_code_memory_range;
         memory_range = old_memory_range;
      } catch (...) {
         pthread_sigmask(SIG_SETMASK, &old_sigmask, nullptr);
         std::atomic_store(&signal_dest, old_signal_handler);
         code_memory_range = old_code_memory_range;
         memory_range = old_memory_range;
         if (owns_process_signal_scope) {
            release_signal_handler_scope();
         }
         throw;
      }
      if (owns_process_signal_scope) {
         release_signal_handler_scope();
      }
   } else {
      std::atomic_store(&signal_dest, old_signal_handler);
      code_memory_range = old_code_memory_range;
      memory_range = old_memory_range;
      if (owns_process_signal_scope) {
         release_signal_handler_scope();
      }
      if (sig == -1) {
         std::exception_ptr exception = std::move(saved_exception);
         saved_exception = nullptr;
         std::rethrow_exception(exception);
      } else {
         e(sig);
      }
   }
}

template <typename F, typename E, typename Null>
   requires std::is_same_v<std::remove_cvref_t<Null>, std::nullptr_t>
auto invoke_with_signal_handler(F&& f, E&& e, Null&&, wasm_allocator* mem_allocator) {
   return invoke_with_signal_handler(std::forward<F>(f), std::forward<E>(e), std::span<std::byte>{}, mem_allocator);
}

} // namespace forge::vm::wasm::interpret
