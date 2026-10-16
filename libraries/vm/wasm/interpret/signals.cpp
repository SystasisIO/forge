module;

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <setjmp.h>
#include <signal.h>
#include <span>
#include <string>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

module forge.vm.wasm.interpret.backend;

import :signals;

#if defined(__APPLE__) && defined(__aarch64__)
#include "details/mach_recovery.hxx"
#endif

namespace forge::vm::wasm::interpret {

namespace {

using simple_signal_handler = void (*)(int);
using detailed_signal_handler = void (*)(int, siginfo_t*, void*);

enum class forwarded_action : unsigned { default_action, ignore, simple, detailed };

static_assert(std::atomic<sigjmp_buf*>::is_always_lock_free,
              "Atomic pointers must be lock-free to be async signal safe.");
static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "The sole timed-run count must be lock-free to be async signal safe.");
static_assert(std::atomic<forwarded_action>::is_always_lock_free,
              "Forwarded signal actions must be lock-free to be async signal safe.");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Signal handler generations must be lock-free to be async signal safe.");
static_assert(std::atomic<simple_signal_handler>::is_always_lock_free,
              "Signal handler pointers must be lock-free to be async signal safe.");
static_assert(std::atomic<detailed_signal_handler>::is_always_lock_free,
              "Detailed signal handler pointers must be lock-free to be async signal safe.");

struct signal_handler_state {
   std::atomic<std::uint64_t> generation{0};
   std::atomic<forwarded_action> action{forwarded_action::default_action};
   std::atomic<simple_signal_handler> simple{SIG_DFL};
   std::atomic<detailed_signal_handler> detailed{nullptr};
   struct sigaction displaced{};
   bool installed = false;
};

struct signal_handler_snapshot {
   forwarded_action action = forwarded_action::default_action;
   simple_signal_handler simple = SIG_DFL;
   detailed_signal_handler detailed = nullptr;
};

signal_handler_state segv_handler_state;
#ifndef __linux__
signal_handler_state bus_handler_state;
#endif
signal_handler_state fpe_handler_state;
std::mutex signal_handler_mutex;
std::size_t active_signal_scopes = 0;

thread_local bool strict_native_recovery = false;
thread_local std::span<const std::byte> native_stack_guard;
thread_local volatile sig_atomic_t native_fault = static_cast<sig_atomic_t>(native_signal_fault::none);

std::uintptr_t instruction_address(void* context) noexcept {
   if (!context)
      return 0;
   const auto* native = static_cast<const ucontext_t*>(context);
#if defined(__APPLE__) && defined(__aarch64__)
   return native->uc_mcontext ? native->uc_mcontext->__ss.__pc : 0;
#elif defined(__APPLE__) && defined(__x86_64__)
   return native->uc_mcontext ? native->uc_mcontext->__ss.__rip : 0;
#elif defined(__linux__) && defined(__x86_64__)
   return static_cast<std::uintptr_t>(native->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
   return native->uc_mcontext.pc;
#else
   return 0;
#endif
}

[[maybe_unused]] constexpr bool has_instruction_address =
#if (defined(__APPLE__) || defined(__linux__)) && (defined(__aarch64__) || defined(__x86_64__))
    true;
#else
    false;
#endif

bool synchronous_fault(const siginfo_t& info) noexcept {
#if defined(__APPLE__)
   // Darwin's SI_USER/SI_QUEUE/... are positive, unlike their Linux values.
   return info.si_code > 0 && info.si_code < SI_USER;
#else
   return info.si_code > 0;
#endif
}

bool restore_alternate_stack(const stack_t& original) noexcept {
   auto restore = original;
#if defined(__APPLE__)
   // Darwin's UNIX2003 wrapper checks size even when disabling; the kernel ignores these disabled fields.
   if ((restore.ss_flags & SS_DISABLE) != 0 && restore.ss_size < MINSIGSTKSZ)
      restore.ss_size = MINSIGSTKSZ;
#endif
   return ::sigaltstack(&restore, nullptr) == 0;
}

signal_handler_state& state_for(int signal) noexcept {
   switch (signal) {
   case SIGSEGV:
      return segv_handler_state;
#ifndef __linux__
   case SIGBUS:
      return bus_handler_state;
#endif
   case SIGFPE:
      return fpe_handler_state;
   default:
      std::abort();
   }
}

signal_handler_snapshot previous_handler(int signal) noexcept {
   auto& state = state_for(signal);
   while (true) {
      const auto before = state.generation.load();
      if ((before & 1U) != 0) {
         continue;
      }

      const auto snapshot = signal_handler_snapshot{
          .action = state.action.load(),
          .simple = state.simple.load(),
          .detailed = state.detailed.load(),
      };
      if (state.generation.load() == before) {
         return snapshot;
      }
   }
}

bool is_forge_signal_handler(const struct sigaction& action) noexcept {
   return (action.sa_flags & SA_SIGINFO) != 0 && action.sa_sigaction == &signal_handler;
}

struct sigaction default_signal_action() noexcept {
   struct sigaction action{};
   action.sa_handler = SIG_DFL;
   sigemptyset(&action.sa_mask);
   return action;
}

template <int Signal> bool install_signal_handler(const struct sigaction& action) noexcept {
   auto& state = state_for(Signal);
   const auto generation = state.generation.load();
   // An odd generation keeps signal readers from observing the displaced handler before its fields are published.
   state.generation.store(generation + 1);

   struct sigaction displaced{};
   if (::sigaction(Signal, &action, &displaced) != 0) {
      state.generation.store(generation + 2);
      return false;
   }
   if (is_forge_signal_handler(displaced)) {
      if (!state.installed) {
         state.displaced = default_signal_action();
         state.action.store(forwarded_action::default_action);
      }
      state.installed = true;
      state.generation.store(generation + 2);
      return true;
   }

   state.displaced = displaced;
   if ((displaced.sa_flags & SA_SIGINFO) != 0) {
      state.detailed.store(displaced.sa_sigaction);
      state.action.store(forwarded_action::detailed);
   } else if (displaced.sa_handler == SIG_DFL) {
      state.action.store(forwarded_action::default_action);
   } else if (displaced.sa_handler == SIG_IGN) {
      state.action.store(forwarded_action::ignore);
   } else {
      state.simple.store(displaced.sa_handler);
      state.action.store(forwarded_action::simple);
   }
   state.installed = true;
   state.generation.store(generation + 2);
   return true;
}

template <int Signal> bool restore_signal_handler() noexcept {
   auto& state = state_for(Signal);
   if (!state.installed) {
      return true;
   }

   const auto generation = state.generation.load();
   state.generation.store(generation + 1);
   struct sigaction current{};
   if (::sigaction(Signal, nullptr, &current) != 0) {
      state.generation.store(generation + 2);
      return false;
   }
   if (is_forge_signal_handler(current) && ::sigaction(Signal, &state.displaced, nullptr) != 0) {
      state.generation.store(generation + 2);
      return false;
   }
   // Keep the immutable forwarding snapshot available to a handler that entered before the OS disposition changed.
   state.installed = false;
   state.generation.store(generation + 2);
   return true;
}

void prepare_signal_mask(sigset_t& blocked) noexcept {
   sigemptyset(&blocked);
   sigaddset(&blocked, SIGSEGV);
#ifndef __linux__
   sigaddset(&blocked, SIGBUS);
#endif
   sigaddset(&blocked, SIGFPE);
}

void prepare_transition_mask(sigset_t& blocked) noexcept {
   prepare_signal_mask(blocked);
   sigaddset(&blocked, SIGPROF);
}

bool restore_installed_handlers() noexcept {
   auto restored = restore_signal_handler<SIGSEGV>();
#ifndef __linux__
   restored = restore_signal_handler<SIGBUS>() && restored;
#endif
   restored = restore_signal_handler<SIGFPE>() && restored;
   return restored;
}

bool install_managed_handlers() noexcept {
   struct sigaction action{};
   action.sa_sigaction = &signal_handler;
   sigemptyset(&action.sa_mask);
   sigaddset(&action.sa_mask, SIGPROF);
   action.sa_flags = SA_NODEFER | SA_SIGINFO;
#if !(defined(__APPLE__) && defined(__aarch64__))
   action.sa_flags |= SA_ONSTACK;
#endif
   if (!install_signal_handler<SIGSEGV>(action))
      return false;
#ifndef __linux__
   if (!install_signal_handler<SIGBUS>(action))
      return false;
#endif
   return install_signal_handler<SIGFPE>(action);
}

void record_failure(signal_boundary_status& status, signal_boundary_stage stage, int error) noexcept {
   if (status.stage == signal_boundary_stage::none) {
      status.stage = stage;
      status.error = error;
   }
}

void record_rollback_failure(signal_boundary_status& status, signal_boundary_stage stage, int error) noexcept {
   record_failure(status, stage, error);
   status.rollback_failed |= std::uint32_t{1} << static_cast<unsigned>(stage);
}

[[noreturn]] void restoration_failed() noexcept {
   // Returning could free a borrowed alternate stack still referenced by the OS, or publish a fictitious lease.
   std::_Exit(EXIT_FAILURE);
}

signal_boundary_status install_handlers_locked() noexcept {
   auto status = signal_boundary_status{};
   sigset_t blocked, previous_mask;
   prepare_signal_mask(blocked);
   const auto mask_error = pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask);
   if (mask_error != 0) {
      record_failure(status, signal_boundary_stage::acquire_handlers, mask_error);
      return status;
   }
   if (!install_managed_handlers()) {
      record_failure(status, signal_boundary_stage::acquire_handlers, errno);
      if (!restore_installed_handlers())
         record_rollback_failure(status, signal_boundary_stage::release_handlers, errno);
   }
   const auto restore_error = pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
   if (restore_error != 0) {
      record_rollback_failure(status, signal_boundary_stage::restore_mask, restore_error);
      if (!restore_installed_handlers())
         record_rollback_failure(status, signal_boundary_stage::release_handlers, errno);
   }
   return status;
}

signal_boundary_status acquire_handlers() noexcept {
   try {
      const auto lock = std::scoped_lock{signal_handler_mutex};
      if (active_signal_scopes != 0) {
         if (active_signal_scopes == std::numeric_limits<std::size_t>::max())
            return {signal_boundary_stage::acquire_handlers, EOVERFLOW, 0};
         ++active_signal_scopes;
         return {};
      }
      const auto status = install_handlers_locked();
      if (status.stage == signal_boundary_stage::none)
         active_signal_scopes = 1;
      return status;
   } catch (...) {
      // A mutex acquisition failure precedes any owner mutation. EH and lock cleanup finish with recovery NULL.
      return {signal_boundary_stage::acquire_handlers, EIO, 0};
   }
}

signal_boundary_status release_handlers() noexcept {
   try {
      const auto lock = std::scoped_lock{signal_handler_mutex};
      if (active_signal_scopes == 0)
         return {signal_boundary_stage::release_handlers, EINVAL, 0};
      if (active_signal_scopes != 1) {
         --active_signal_scopes;
         return {};
      }
      auto status = signal_boundary_status{};
      sigset_t blocked, previous_mask;
      prepare_signal_mask(blocked);
      const auto mask_error = pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask);
      if (mask_error != 0)
         return {signal_boundary_stage::release_handlers, mask_error, 0};
      const auto restored = restore_installed_handlers();
      if (!restored)
         record_rollback_failure(status, signal_boundary_stage::release_handlers, errno);
      const auto restore_error = pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
      if (restore_error != 0)
         record_rollback_failure(status, signal_boundary_stage::restore_mask, restore_error);
      if (status.stage == signal_boundary_stage::none)
         active_signal_scopes = 0;
      return status;
   } catch (...) {
      return {signal_boundary_stage::release_handlers, EIO, 0};
   }
}

} // namespace

__attribute__((visibility("default"))) thread_local std::atomic<sigjmp_buf*> signal_dest{nullptr};
__attribute__((visibility("default"))) thread_local std::span<std::byte> code_memory_range;
__attribute__((visibility("default"))) thread_local std::span<std::byte> memory_range;
__attribute__((visibility("default"))) thread_local std::atomic<std::size_t> timed_run_timeout_count{0};
__attribute__((visibility("default"))) thread_local detail::timed_run_scope* detail::active_timed_run = nullptr;
__attribute__((visibility("default"))) thread_local legacy_signal_outcome* current_legacy_outcome = nullptr;

namespace {

void forward_callback(signal_handler_snapshot previous, int signal, siginfo_t* info, void* context) {
   auto invoke = [previous, signal, info, context] {
      if (previous.action == forwarded_action::detailed)
         previous.detailed(signal, info, context);
      else
         previous.simple(signal);
   };
   if (signal_dest.load() == nullptr || current_legacy_outcome != nullptr) {
      // The complete foreign C++ callback, including cleanup and typed failure,
      // uses the same whole-NULL bridge and existing external legacy outcome.
      longjmp_on_exception(invoke);
   } else {
      // Active native entry deliberately has no legacy exception slot. A prior
      // POSIX handler must return without throwing; an unexpected exception
      // terminates at this noexcept boundary while canonical recovery is NULL.
      const auto status = invoke_without_signal_recovery(
          [](void* opaque) noexcept { (*static_cast<decltype(invoke)*>(opaque))(); }, &invoke);
      if (status.stage != signal_boundary_stage::none) {
         signal_dest.store(nullptr);
         std::terminate(); // Cannot safely dispatch foreign C++ without suspension.
      }
   }
}

} // namespace

void signal_handler(int signal, siginfo_t* info, void* context) {
   auto* destination = std::atomic_load(&signal_dest);
#if defined(__APPLE__) && defined(__aarch64__)
   // Darwin ARM64 synthesizes fault si_code values even for raise/pthread_kill.
   // Hardware recovery is exclusively Mach-owned for native and production
   // nonempty legacy ranges. Preserve ONLY the original private both-empty
   // checkpoint catch-all; it is not a public or whole-C++ recovery contract.
   if (destination && (strict_native_recovery || !code_memory_range.empty() || !memory_range.empty()))
      destination = nullptr;
#endif
   if (destination) {
      const auto address = info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;
      if (strict_native_recovery) {
         if (info && synchronous_fault(*info) &&
             detail::contains_address(code_memory_range, instruction_address(context))) {
            const auto fault = detail::contains_address(native_stack_guard, address)  ? native_signal_fault::stack
                               : detail::contains_address(memory_range, address)      ? native_signal_fault::memory
                               : signal == SIGFPE                                     ? native_signal_fault::arithmetic
                               : detail::contains_address(code_memory_range, address) ? native_signal_fault::code
                                                                                      : native_signal_fault::none;
            if (fault != native_signal_fault::none) {
               native_fault = static_cast<sig_atomic_t>(fault);
               jump_to_signal_recovery(signal);
            }
         }
      } else {
         if (code_memory_range.empty() && memory_range.empty()) {
            jump_to_signal_recovery(signal);
         }
         if (detail::contains_address(memory_range, address)) {
            native_fault = static_cast<sig_atomic_t>(native_signal_fault::memory);
            jump_to_signal_recovery(signal);
         }
         if (detail::contains_address(code_memory_range, address)) {
            if ((signal == SIGSEGV || signal == SIGBUS) &&
                timed_run_timeout_count.load(std::memory_order_acquire) == 0) {
               return;
            }
            native_fault = static_cast<sig_atomic_t>(native_signal_fault::code);
            jump_to_signal_recovery(signal);
         }
      }
   }

   const auto previous = previous_handler(signal);
   switch (previous.action) {
   case forwarded_action::detailed:
      forward_callback(previous, signal, info, context);
      break;
   case forwarded_action::default_action: {
      struct sigaction action{};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      sigaction(signal, &action, nullptr);
      raise(signal);
      break;
   }
   case forwarded_action::ignore:
      break;
   case forwarded_action::simple:
      forward_callback(previous, signal, info, context);
      break;
   }
}

namespace {

#if defined(__APPLE__) && defined(__aarch64__)
void release_prepared_handlers() noexcept {
   const auto status = release_handlers();
   if (status.stage != signal_boundary_stage::none)
      restoration_failed();
}

[[noreturn]] void leave_hardware_fault(int signal, native_signal_fault fault) noexcept {
   native_fault = static_cast<sig_atomic_t>(fault);
   jump_to_signal_recovery(signal);
}

mach_recovery::view recovery_view() noexcept {
   return {&signal_dest,          &code_memory_range,        &memory_range,
           &native_stack_guard,   &strict_native_recovery,   &timed_run_timeout_count,
           &leave_hardware_fault, &release_prepared_handlers};
}

signal_boundary_status prepare_legacy_thread() noexcept {
   if (mach_recovery::fork_child())
      return {signal_boundary_stage::fork_child, ECHILD, 0};
   if (mach_recovery::ready())
      return {};
   auto owner = recovery_view();
   owner.release_handlers = nullptr;
   // The compatible legacy path caches the Mach route/reserve, but retains
   // the old per-outer-invocation POSIX disposition restoration contract.
   return mach_recovery::prepare_legacy(owner);
}
#endif

struct recovery_state {
   sigjmp_buf* destination;
   std::span<std::byte> code;
   std::span<std::byte> memory;
   std::span<const std::byte> stack_guard;
   bool strict_native;
   sig_atomic_t fault;
   legacy_signal_outcome* outcome;
};

recovery_state current_recovery() noexcept {
   return {signal_dest.load(),     code_memory_range, memory_range,          native_stack_guard,
           strict_native_recovery, native_fault,      current_legacy_outcome};
}

void restore_recovery(const recovery_state& previous) noexcept {
   code_memory_range = previous.code;
   memory_range = previous.memory;
   native_stack_guard = previous.stack_guard;
   strict_native_recovery = previous.strict_native;
   native_fault = previous.fault;
   current_legacy_outcome = previous.outcome;
   signal_dest.store(previous.destination);
}

// Called only with transition signals blocked. These spans classify addresses; no bytes are mutated.
void publish_recovery(sigjmp_buf* destination, native_signal_ranges ranges, bool strict) noexcept {
   code_memory_range = {const_cast<std::byte*>(ranges.code.data()), ranges.code.size()};
   memory_range = {const_cast<std::byte*>(ranges.memory.data()), ranges.memory.size()};
   native_stack_guard = ranges.stack_guard;
   strict_native_recovery = strict;
   native_fault = static_cast<sig_atomic_t>(native_signal_fault::none);
   signal_dest.store(destination);
}

[[gnu::noinline]] native_signal_result invoke_signal_boundary(signal_recovery_entry callback, void* state,
                                                              native_signal_ranges ranges, bool strict,
                                                              std::span<std::byte> signal_stack,
                                                              legacy_signal_outcome* outcome) noexcept {
   static_assert(std::is_trivially_copyable_v<recovery_state>);
   static_assert(std::is_trivially_copyable_v<native_signal_result>);
   auto result = native_signal_result{};
   if (callback == nullptr) {
      result.status = {signal_boundary_stage::entry, EINVAL, 0};
      return result;
   }
   sigset_t blocked, original_mask;
   prepare_transition_mask(blocked);
   const auto block_error = pthread_sigmask(SIG_BLOCK, &blocked, &original_mask);
   if (block_error != 0) {
      result.status = {signal_boundary_stage::block_entry, block_error, 0};
      return result;
   }
   const auto previous = current_recovery();
   signal_dest.store(nullptr);
   bool acquired = false;
   bool invocation_started = false;
   bool alternate_stack_installed = false;
   stack_t original_stack{};
#if defined(__APPLE__) && defined(__aarch64__)
   if (mach_recovery::fork_child()) {
      record_failure(result.status, signal_boundary_stage::fork_child, ECHILD);
   } else if (strict) {
      if (!signal_stack.data() || signal_stack.size() < 128 * 1024)
         record_failure(result.status, signal_boundary_stage::signal_stack, EINVAL);
      else if (!mach_recovery::ready())
         record_failure(result.status, signal_boundary_stage::recovery_not_ready, EAGAIN);
      else if (!mach_recovery::matches_reserve(signal_stack))
         record_failure(result.status, signal_boundary_stage::thread_prepare, EBUSY);
      else if (::sigaltstack(nullptr, &original_stack) != 0)
         record_failure(result.status, signal_boundary_stage::query_stack, errno);
      else if ((original_stack.ss_flags & SS_ONSTACK) != 0)
         record_failure(result.status, signal_boundary_stage::active_stack, EBUSY);
   } else {
      result.status = prepare_legacy_thread();
   }
   if (result.status.stage == signal_boundary_stage::none) {
      if (!mach_recovery::persistent_handlers()) {
         result.status = acquire_handlers();
         acquired = result.status.stage == signal_boundary_stage::none;
      }
   }
   if (result.status.stage == signal_boundary_stage::none) {
      mach_recovery::begin_invocation();
      invocation_started = true;
   }
#else
   if (strict) {
      if (!has_instruction_address) {
         record_failure(result.status, signal_boundary_stage::platform, ENOTSUP);
      } else if (signal_stack.data() == nullptr || signal_stack.size() < MINSIGSTKSZ) {
         record_failure(result.status, signal_boundary_stage::signal_stack, EINVAL);
      } else if (::sigaltstack(nullptr, &original_stack) != 0) {
         record_failure(result.status, signal_boundary_stage::query_stack, errno);
      } else if ((original_stack.ss_flags & SS_ONSTACK) != 0) {
         record_failure(result.status, signal_boundary_stage::active_stack, EBUSY);
      } else {
         stack_t next_stack{};
         next_stack.ss_sp = signal_stack.data();
         next_stack.ss_size = signal_stack.size();
         if (::sigaltstack(&next_stack, nullptr) != 0)
            record_failure(result.status, signal_boundary_stage::install_stack, errno);
         else
            alternate_stack_installed = true;
      }
   }
   if (result.status.stage == signal_boundary_stage::none) {
      result.status = acquire_handlers();
      acquired = result.status.stage == signal_boundary_stage::none;
   }
#endif
   // This storage stays live until destination publication has been removed during teardown.
   // Whole-object initialization covers OS-owned storage not initialized by some libc sigsetjmp versions.
   sigjmp_buf destination{};
   if (result.status.stage == signal_boundary_stage::none) {
      switch (sigsetjmp(destination,
#if defined(__APPLE__) && defined(__aarch64__)
                        0
#else
                        1
#endif
                        )) {
      case 0: {
         current_legacy_outcome = outcome;
         publish_recovery(&destination, ranges, strict);
         auto execution_mask = original_mask;
         sigdelset(&execution_mask, SIGSEGV);
#ifndef __linux__
         sigdelset(&execution_mask, SIGBUS);
#endif
         sigdelset(&execution_mask, SIGFPE);
         if (strict)
            sigaddset(&execution_mask, SIGPROF);
         else
            sigdelset(&execution_mask, SIGPROF);
         const auto enter_error = pthread_sigmask(SIG_SETMASK, &execution_mask, nullptr);
         if (enter_error != 0)
            record_failure(result.status, signal_boundary_stage::enter_mask, enter_error);
         else
            callback(state);
         break;
      }
      case SIGSEGV:
         result.signal = SIGSEGV;
         break;
#ifndef __linux__
      case SIGBUS:
         result.signal = SIGBUS;
         break;
#endif
      case SIGFPE:
         result.signal = SIGFPE;
         break;
      case -1:
         result.signal = -1;
         break;
      case -2:
         result.fault = native_signal_fault::explicit_exit;
         break;
      default:
         std::abort();
      }
      // Capture existing owned provenance before restore_recovery restores the
      // ancestor slot. The legacy both-empty hook still publishes no fault.
      if (result.signal != 0)
         result.fault = static_cast<native_signal_fault>(native_fault);
   }

   // The callback's complete lifetime has ended. Teardown has only POD while recovery is NULL.
   const auto exit_error = pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
   signal_dest.store(nullptr);
   if (exit_error != 0)
      record_rollback_failure(result.status, signal_boundary_stage::block_exit, exit_error);
   if (alternate_stack_installed && !restore_alternate_stack(original_stack))
      record_rollback_failure(result.status, signal_boundary_stage::restore_stack, errno);
   if (acquired) {
      const auto release = release_handlers();
      if (release.stage != signal_boundary_stage::none)
         record_rollback_failure(result.status, signal_boundary_stage::release_handlers, release.error);
      result.status.rollback_failed |= release.rollback_failed;
   }
#if defined(__APPLE__) && defined(__aarch64__)
   if (invocation_started)
      mach_recovery::end_invocation();
#endif
   if (result.status.rollback_failed != 0) {
      // Attempt the remaining safe mask restoration, but do not rearm an unusable ancestor or return borrowed bytes.
      static_cast<void>(pthread_sigmask(SIG_SETMASK, &original_mask, nullptr));
      restoration_failed();
   }
   restore_recovery(previous);
   if (pthread_sigmask(SIG_SETMASK, &original_mask, nullptr) != 0)
      restoration_failed();
   // Rearm/mask restore is the last boundary: no exception, lock, diagnostic or nontrivial temporary follows.
   return result;
}

} // namespace

native_signal_result invoke_legacy_signal_handler(signal_recovery_entry callback, void* state,
                                                  legacy_signal_outcome* outcome, std::span<const std::byte> code,
                                                  std::span<const std::byte> memory) noexcept {
   return invoke_signal_boundary(callback, state, {code, memory, {}}, false, {}, outcome);
}

signal_boundary_status prepare_signal_recovery_thread(std::span<std::byte> recovery_stack) noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
   if (signal_dest.load() != nullptr)
      return {signal_boundary_stage::thread_prepare, EBUSY, 0};
   if (mach_recovery::fork_child())
      return {signal_boundary_stage::fork_child, ECHILD, 0};
   if (mach_recovery::ready())
      return mach_recovery::matches_reserve(recovery_stack)
                 ? signal_boundary_status{}
                 : signal_boundary_status{signal_boundary_stage::thread_prepare, EBUSY, 0};
   const auto handlers = acquire_handlers();
   if (handlers.rollback_failed != 0)
      restoration_failed();
   if (handlers.stage != signal_boundary_stage::none)
      return handlers;
   const auto prepared = mach_recovery::prepare(recovery_view(), recovery_stack);
   if (prepared.stage != signal_boundary_stage::none)
      release_prepared_handlers();
   return prepared;
#else
   static_cast<void>(recovery_stack);
   return {};
#endif
}

signal_boundary_status release_signal_recovery_thread() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
   return mach_recovery::release();
#else
   return {};
#endif
}

native_signal_result invoke_native_with_signals(native_signal_ranges ranges, std::span<std::byte> signal_stack,
                                                signal_recovery_entry entry, void* state) noexcept {
   return invoke_signal_boundary(entry, state, ranges, true, signal_stack, nullptr);
}

[[noreturn]] void leave_native_with_signals() noexcept {
   auto* destination = signal_dest.load();
   if (!destination || !strict_native_recovery)
      std::terminate();
   sigset_t blocked;
   prepare_transition_mask(blocked);
   if (pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0)
      restoration_failed();
   native_fault = static_cast<sig_atomic_t>(native_signal_fault::explicit_exit);
   jump_to_signal_recovery(-2);
}

[[noreturn]] void jump_to_signal_recovery(int result) noexcept {
   auto* destination = signal_dest.load();
   if (!destination)
      std::abort();
   sigset_t blocked;
#if defined(__APPLE__) && defined(__aarch64__)
   // Darwin save-mask0 must reach its continuation behind a thread-only barrier.
   prepare_transition_mask(blocked);
#else
   sigemptyset(&blocked);
   sigaddset(&blocked, SIGPROF);
#endif
   if (pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0)
      restoration_failed();
   siglongjmp(*destination, result);
}

signal_boundary_status invoke_without_signal_recovery(signal_recovery_entry callback, void* state) noexcept {
   if (callback == nullptr)
      return {signal_boundary_stage::entry, EINVAL, 0};
   sigset_t blocked, original_mask;
   prepare_transition_mask(blocked);
   const auto block_error = pthread_sigmask(SIG_BLOCK, &blocked, &original_mask);
   if (block_error != 0)
      return {signal_boundary_stage::block_entry, block_error, 0};
   const auto previous = current_recovery();
   signal_dest.store(nullptr);
   auto host_mask = original_mask;
   sigaddset(&host_mask, SIGPROF);
   auto status = signal_boundary_status{};
   const auto enter_error = pthread_sigmask(SIG_SETMASK, &host_mask, nullptr);
   if (enter_error != 0)
      record_failure(status, signal_boundary_stage::enter_mask, enter_error);
   else
      callback(state);
   const auto exit_error = pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
   signal_dest.store(nullptr);
   if (exit_error != 0) {
      static_cast<void>(pthread_sigmask(SIG_SETMASK, &original_mask, nullptr));
      restoration_failed();
   }
   restore_recovery(previous);
   if (pthread_sigmask(SIG_SETMASK, &original_mask, nullptr) != 0)
      restoration_failed();
   return status;
}

void setup_signal_handler_impl() {
   if (signal_dest.load() != nullptr)
      std::abort();
   const auto lock = std::scoped_lock{signal_handler_mutex};
   const auto status = install_handlers_locked();
   if (status.rollback_failed != 0)
      restoration_failed();
   check_signal_boundary_status(status);
}

void acquire_signal_handler_scope() {
   if (signal_dest.load() != nullptr)
      std::abort();
   const auto status = acquire_handlers();
   if (status.rollback_failed != 0)
      restoration_failed();
   check_signal_boundary_status(status);
}

void release_signal_handler_scope() {
   if (signal_dest.load() != nullptr)
      std::abort();
   const auto status = release_handlers();
   if (status.stage != signal_boundary_stage::none)
      restoration_failed();
}

void check_signal_boundary_status(signal_boundary_status status) {
   if (signal_dest.load() != nullptr)
      std::abort();
   if (status.stage != signal_boundary_stage::none)
      switch (status.stage) {
      case signal_boundary_stage::platform:
      case signal_boundary_stage::block_entry:
      case signal_boundary_stage::query_stack:
      case signal_boundary_stage::active_stack:
      case signal_boundary_stage::install_stack:
      case signal_boundary_stage::acquire_handlers:
      case signal_boundary_stage::enter_mask:
      case signal_boundary_stage::thread_prepare:
      case signal_boundary_stage::thread_release:
      case signal_boundary_stage::thread_capacity:
      case signal_boundary_stage::recovery_not_ready:
      case signal_boundary_stage::fork_child:
      case signal_boundary_stage::mach_service:
      case signal_boundary_stage::mach_routes:
         detail::fail<exceptions::execution_unavailable>("VM recovery machinery unavailable (stage " +
                                                         std::to_string(static_cast<unsigned>(status.stage)) +
                                                         ", OS status " + std::to_string(status.error) + ")");
      default:
         detail::fail<exceptions::interpreter>("VM signal boundary failed (stage " +
                                               std::to_string(static_cast<unsigned>(status.stage)) + ", errno " +
                                               std::to_string(status.error) + ")");
      }
}

} // namespace forge::vm::wasm::interpret
