#include "test_prelude.hpp"
#include "fresh_process.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <csignal>
#include <exception>
#include <span>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach.h>
#endif

import forge.vm.wasm.interpret.backend;

#include "test_support.hpp"

#define FORGE_VM_WASM_INTERPRET_TEST_FILE native_signal_boundary_tests

namespace wasm = forge::vm::wasm::interpret;

namespace {

volatile std::sig_atomic_t forwarded = 0;

void forward_simple(int) {
   forwarded = forwarded + 1;
}

void forward_detailed(int, siginfo_t*, void*) {
   forwarded = forwarded + 1;
}

class action_guard {
 public:
   explicit action_guard(int signal) : _signal(signal) {
      BOOST_REQUIRE(::sigaction(signal, nullptr, &_previous) == 0);
   }
   ~action_guard() {
      static_cast<void>(::sigaction(_signal, &_previous, nullptr));
   }

 private:
   int _signal;
   struct sigaction _previous{};
};

class mask_guard {
 public:
   mask_guard() {
      BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &_previous) == 0);
   }
   ~mask_guard() {
      static_cast<void>(::pthread_sigmask(SIG_SETMASK, &_previous, nullptr));
   }

 private:
   sigset_t _previous{};
};

bool same_mask(const sigset_t& first, const sigset_t& second) {
   for (int signal = 1; signal < NSIG; ++signal)
      if (sigismember(&first, signal) != sigismember(&second, signal))
         return false;
   return true;
}

std::vector<std::byte> signal_storage() {
   return std::vector<std::byte>(std::max<std::size_t>(SIGSTKSZ, 128 * 1024));
}

class prepared_recovery {
 public:
   explicit prepared_recovery(std::span<std::byte> storage) {
      // Tests may deliberately replace a compatible standalone legacy lease;
      // production worker hooks prepare once, not per invocation.
      BOOST_REQUIRE(wasm::release_signal_recovery_thread().stage == wasm::signal_boundary_stage::none);
      BOOST_REQUIRE(wasm::prepare_signal_recovery_thread(storage).stage == wasm::signal_boundary_stage::none);
   }
   ~prepared_recovery() {
      if (wasm::release_signal_recovery_thread().stage != wasm::signal_boundary_stage::none)
         std::_Exit(93);
   }
};

class protected_page {
 public:
   protected_page() : _size(static_cast<std::size_t>(::sysconf(_SC_PAGESIZE))) {
      _data = static_cast<std::byte*>(::mmap(nullptr, _size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
      BOOST_REQUIRE(_data != MAP_FAILED);
   }
   ~protected_page() {
      static_cast<void>(::munmap(_data, _size));
   }
   std::span<const std::byte> bytes() const {
      return {_data, _size};
   }
   void* data() const {
      return _data;
   }

 private:
   std::byte* _data;
   std::size_t _size;
};

// Tiny raw instructions, not a WASM compiler or product code path. No C++ frame lives below the native jump.
class native_fixture {
 public:
   enum class operation { read, exhaust_stack };

   explicit native_fixture(operation operation) : _size(static_cast<std::size_t>(::sysconf(_SC_PAGESIZE))) {
      _data = static_cast<std::byte*>(::mmap(nullptr, _size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
      BOOST_REQUIRE(_data != MAP_FAILED);
#if defined(__aarch64__)
      const auto instructions = operation == operation::read
                                    ? std::array<std::uint32_t, 3>{0xf9400001, 0xd65f03c0, 0xd65f03c0}
                                    : std::array<std::uint32_t, 3>{0x9100001f, 0xf90003ff, 0xd65f03c0};
      _code_size = operation == operation::read ? 8 : 12;
      std::memcpy(_data, instructions.data(), _code_size);
#elif defined(__x86_64__)
      const auto instructions = operation == operation::read
                                    ? std::array<unsigned char, 8>{0x48, 0x8b, 0x07, 0xc3}
                                    : std::array<unsigned char, 8>{0x48, 0x89, 0xfc, 0x48, 0x89, 0x04, 0x24, 0xc3};
      _code_size = operation == operation::read ? 4 : 8;
      std::memcpy(_data, instructions.data(), _code_size);
#else
#error Native signal fixtures require an explicitly supported instruction set.
#endif
      __builtin___clear_cache(reinterpret_cast<char*>(_data), reinterpret_cast<char*>(_data + _code_size));
      BOOST_REQUIRE(::mprotect(_data, _size, PROT_READ | PROT_EXEC) == 0);
   }
   ~native_fixture() {
      static_cast<void>(::munmap(_data, _size));
   }
   wasm::signal_recovery_entry entry() const {
      return reinterpret_cast<wasm::signal_recovery_entry>(_data);
   }
   std::span<const std::byte> code() const {
      return {_data, _code_size};
   }

 private:
   std::byte* _data;
   std::size_t _size;
   std::size_t _code_size;
};

void empty_entry(void*) noexcept {}

struct counted_object {
   int* counter;
   ~counted_object() {
      ++*counter;
   }
};

struct callback_failure {};

struct host_state {
   int destroyed = 0;
   bool prof_blocked = false;
   std::exception_ptr exception;
   wasm::signal_boundary_status status;
};

void throwing_host(void* opaque) {
   auto& state = *static_cast<host_state*>(opaque);
   const auto counted = counted_object{&state.destroyed};
   sigset_t mask;
   ::pthread_sigmask(SIG_SETMASK, nullptr, &mask);
   state.prof_blocked = sigismember(&mask, SIGPROF) == 1;
   throw callback_failure{};
}

void host_bridge_entry(void* opaque) noexcept {
   auto& state = *static_cast<host_state*>(opaque);
   state.status = wasm::invoke_without_signal_recovery(
       [](void* opaque) noexcept {
          auto& state = *static_cast<host_state*>(opaque);
          try {
             throwing_host(opaque);
          } catch (...) {
             state.exception = std::current_exception();
          }
       },
       opaque);
   wasm::leave_native_with_signals();
}

void foreign_fatal(int, siginfo_t*, void*) {
   ::_exit(73);
}

void host_raw_fault(void* memory) noexcept {
   *static_cast<volatile std::byte*>(memory) = std::byte{1};
}

void foreign_host_entry(void* memory) noexcept {
   static_cast<void>(wasm::invoke_without_signal_recovery(&host_raw_fault, memory));
}

void queue_exit_signal(void*) noexcept {
   sigset_t mask;
   sigemptyset(&mask);
   sigaddset(&mask, SIGFPE);
   ::pthread_sigmask(SIG_BLOCK, &mask, nullptr);
   ::raise(SIGFPE);
}

struct nested_state {
   std::span<std::byte> signal_stack;
   wasm::native_signal_ranges ranges;
   wasm::signal_recovery_entry entry;
   void* memory;
   bool completed = false;
};

void nested_host(void* opaque) noexcept {
   auto& state = *static_cast<nested_state*>(opaque);
   const auto result = wasm::invoke_native_with_signals(state.ranges, state.signal_stack, state.entry, state.memory);
   state.completed =
       result.status.stage == wasm::signal_boundary_stage::none && result.fault == wasm::native_signal_fault::memory;
}

void nested_entry(void* opaque) noexcept {
   static_cast<void>(wasm::invoke_without_signal_recovery(&nested_host, opaque));
   wasm::leave_native_with_signals();
}

struct active_stack_state {
   std::span<std::byte> supplied;
   volatile std::sig_atomic_t rejected = 0;
   volatile std::sig_atomic_t calls = 0;
};

active_stack_state* active_stack_test = nullptr;

void active_stack_handler(int) {
   const auto result = wasm::invoke_native_with_signals(
       {}, active_stack_test->supplied,
       [](void* opaque) noexcept {
          auto* state = static_cast<active_stack_state*>(opaque);
          state->calls = state->calls + 1;
       },
       active_stack_test);
   active_stack_test->rejected = result.status.stage == wasm::signal_boundary_stage::active_stack ? 1 : 0;
}

struct sibling_state {
   std::atomic<unsigned>* phase;
   wasm::signal_recovery_entry entry;
   void* memory;
   bool live_release_rejected = false;
};

void sibling_entry(void* opaque) noexcept {
   auto& state = *static_cast<sibling_state*>(opaque);
   static_cast<void>(wasm::invoke_without_signal_recovery(
       [](void* opaque) noexcept {
          auto& state = *static_cast<sibling_state*>(opaque);
          const auto release = wasm::release_signal_recovery_thread();
#if defined(__APPLE__) && defined(__aarch64__)
          state.live_release_rejected = release.stage == wasm::signal_boundary_stage::thread_release;
#else
          state.live_release_rejected = release.stage == wasm::signal_boundary_stage::none;
#endif
          state.phase->store(1);
          while (state.phase->load() != 2)
             std::this_thread::yield(); // Test-only bounded parent; C++ synchronization remains whole-NULL.
       },
       opaque));
   state.entry(state.memory); // Raw fault now occurs only in the supplied native fixture.
}

void run_sibling_retirement() {
   auto phase = std::atomic<unsigned>{0};
   auto prepared = std::atomic<bool>{false};
   auto passed = std::atomic<bool>{false};
   auto first = std::thread{[&] {
      auto storage = signal_storage();
      {
         const auto preparation = prepared_recovery{storage};
         prepared.store(true);
         while (phase.load() != 1)
            std::this_thread::yield();
      } // Remove this thread's member while the other invocation is live (destination NULL in host C++).
      phase.store(2);
   }};
   auto second = std::thread{[&] {
      while (!prepared.load())
         std::this_thread::yield();
      auto storage = signal_storage();
      const auto preparation = prepared_recovery{storage};
      const auto page = protected_page{};
      const auto fixture = native_fixture{native_fixture::operation::read};
      auto state = sibling_state{&phase, fixture.entry(), page.data()};
      const auto result =
          wasm::invoke_native_with_signals({fixture.code(), page.bytes(), {}}, storage, &sibling_entry, &state);
      passed.store(result.status.stage == wasm::signal_boundary_stage::none &&
                   result.fault == wasm::native_signal_fault::memory && state.live_release_rejected);
   }};
   first.join();
   second.join();
   BOOST_TEST(passed.load());
}

struct start_failure {};

void run_partial_start_failure() {
   // Reserve lifetime is in the caller, beyond join, as required when a compute
   // start hook throws and the pool does not invoke its stop hook.
   auto reserve = signal_storage();
   auto caught = std::atomic<bool>{false};
   struct sigaction previous{};
   BOOST_REQUIRE(::sigaction(SIGFPE, nullptr, &previous) == 0);
   auto worker = std::thread{[&] {
      try {
         if (wasm::prepare_signal_recovery_thread(reserve).stage != wasm::signal_boundary_stage::none)
            std::_Exit(95);
         throw start_failure{};
      } catch (const start_failure&) {
         caught.store(true);
      }
      // No explicit release/stop hook: the same owner's TLS lease performs cleanup at thread exit.
   }};
   worker.join();
   BOOST_TEST(caught.load());
   struct sigaction restored{};
   BOOST_REQUIRE(::sigaction(SIGFPE, nullptr, &restored) == 0);
   BOOST_TEST(restored.sa_flags == previous.sa_flags);
   if ((previous.sa_flags & SA_SIGINFO) != 0)
      BOOST_CHECK(restored.sa_sigaction == previous.sa_sigaction);
   else
      BOOST_CHECK(restored.sa_handler == previous.sa_handler);
   const auto restarted = prepared_recovery{reserve};
   const auto result = wasm::invoke_native_with_signals({}, reserve, &empty_entry, nullptr);
   BOOST_CHECK(result.status.stage == wasm::signal_boundary_stage::none);
}

#if defined(__APPLE__) && defined(__aarch64__)
void run_rejected_prior_routes() {
   auto storage = signal_storage();
   const auto thread = mach_thread_self();
   mach_port_t port = MACH_PORT_NULL;
   BOOST_REQUIRE(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
   BOOST_REQUIRE(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
   std::array<exception_mask_t, EXC_TYPES_COUNT> old_masks{};
   std::array<mach_port_t, EXC_TYPES_COUNT> old_ports{};
   std::array<exception_behavior_t, EXC_TYPES_COUNT> old_behaviors{};
   std::array<thread_state_flavor_t, EXC_TYPES_COUNT> old_flavors{};
   auto old_count = static_cast<mach_msg_type_number_t>(EXC_TYPES_COUNT);
   BOOST_REQUIRE(thread_swap_exception_ports(thread, EXC_MASK_BAD_ACCESS, port, EXCEPTION_DEFAULT, ARM_THREAD_STATE64,
                                             old_masks.data(), &old_count, old_ports.data(), old_behaviors.data(),
                                             old_flavors.data()) == KERN_SUCCESS);
   const std::array<unsigned, 10> behaviors{EXCEPTION_DEFAULT,
                                            EXCEPTION_STATE,
                                            EXCEPTION_STATE_IDENTITY,
                                            EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
                                            EXCEPTION_STATE | MACH_EXCEPTION_CODES,
                                            EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES,
                                            EXCEPTION_IDENTITY_PROTECTED | MACH_EXCEPTION_CODES,
                                            EXCEPTION_STATE_IDENTITY_PROTECTED | MACH_EXCEPTION_CODES,
                                            EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES |
                                                MACH_EXCEPTION_BACKTRACE_PREFERRED,
                                            EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES | MACH_EXCEPTION_ERRORS};
   for (const auto raw_behavior : behaviors) {
      const auto behavior = static_cast<exception_behavior_t>(raw_behavior);
      BOOST_REQUIRE(thread_set_exception_ports(thread, EXC_MASK_BAD_ACCESS, port, behavior, ARM_THREAD_STATE64) ==
                    KERN_SUCCESS);
      const auto rejected = wasm::prepare_signal_recovery_thread(storage);
      BOOST_CHECK(rejected.stage == wasm::signal_boundary_stage::mach_routes);
      BOOST_TEST(rejected.rollback_failed == 0U);
      auto calls = 0;
      const auto invocation = wasm::invoke_native_with_signals(
          {}, storage, [](void* opaque) noexcept { ++*static_cast<int*>(opaque); }, &calls);
      BOOST_CHECK(invocation.status.stage == wasm::signal_boundary_stage::recovery_not_ready);
      BOOST_TEST(calls == 0);
      std::array<exception_mask_t, EXC_TYPES_COUNT> masks{};
      std::array<mach_port_t, EXC_TYPES_COUNT> ports{};
      std::array<exception_behavior_t, EXC_TYPES_COUNT> actual_behaviors{};
      std::array<thread_state_flavor_t, EXC_TYPES_COUNT> flavors{};
      auto count = static_cast<mach_msg_type_number_t>(EXC_TYPES_COUNT);
      BOOST_REQUIRE(thread_get_exception_ports(thread, EXC_MASK_BAD_ACCESS, masks.data(), &count, ports.data(),
                                               actual_behaviors.data(), flavors.data()) == KERN_SUCCESS);
      BOOST_REQUIRE(count == 1U);
      BOOST_TEST(masks[0] == EXC_MASK_BAD_ACCESS);
      BOOST_TEST(ports[0] == port);
      BOOST_TEST(actual_behaviors[0] == behavior);
      BOOST_TEST(flavors[0] == ARM_THREAD_STATE64);
      BOOST_REQUIRE(mach_port_deallocate(mach_task_self(), ports[0]) == KERN_SUCCESS);
   }
   BOOST_REQUIRE(thread_set_exception_ports(thread, EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT,
                                            THREAD_STATE_NONE) == KERN_SUCCESS);
   for (mach_msg_type_number_t index = 0; index < old_count; ++index) {
      BOOST_REQUIRE(thread_set_exception_ports(thread, old_masks[index], old_ports[index], old_behaviors[index],
                                               old_flavors[index]) == KERN_SUCCESS);
      if (MACH_PORT_VALID(old_ports[index]))
         BOOST_REQUIRE(mach_port_deallocate(mach_task_self(), old_ports[index]) == KERN_SUCCESS);
   }
   BOOST_REQUIRE(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1) == KERN_SUCCESS);
   BOOST_REQUIRE(mach_port_deallocate(mach_task_self(), port) == KERN_SUCCESS);
   BOOST_REQUIRE(mach_port_deallocate(mach_task_self(), thread) == KERN_SUCCESS);
   const auto restarted = prepared_recovery{storage};
   BOOST_CHECK(wasm::invoke_native_with_signals({}, storage, &empty_entry, nullptr).status.stage ==
               wasm::signal_boundary_stage::none);
}
#endif

} // namespace

TEST_CASE("strict native empty ranges and asynchronous faults forward", "[native-signals]") {
   const auto guard = action_guard{SIGFPE};
   auto storage = signal_storage();
   for (const auto detailed : {false, true}) {
      struct sigaction action{};
      sigemptyset(&action.sa_mask);
      if (detailed) {
         action.sa_flags = SA_SIGINFO;
         action.sa_sigaction = &forward_detailed;
      } else {
         action.sa_handler = &forward_simple;
      }
      BOOST_REQUIRE(::sigaction(SIGFPE, &action, nullptr) == 0);
      forwarded = 0;
      const auto result = [&] {
         const auto preparation = prepared_recovery{storage};
         return wasm::invoke_native_with_signals({}, storage, [](void*) noexcept { ::raise(SIGFPE); }, nullptr);
      }();
      BOOST_CHECK(result.fault == wasm::native_signal_fault::none);
      BOOST_TEST(forwarded == 1);
      struct sigaction restored{};
      BOOST_REQUIRE(::sigaction(SIGFPE, nullptr, &restored) == 0);
      BOOST_TEST((restored.sa_flags & SA_SIGINFO) == (action.sa_flags & SA_SIGINFO));
      if (detailed)
         BOOST_CHECK(restored.sa_sigaction == &forward_detailed);
      else
         BOOST_CHECK(restored.sa_handler == &forward_simple);
   }
}

TEST_CASE("native owned hardware memory and exhausted stack faults use the alternate stack", "[native-signals]") {
   auto storage = signal_storage();
   const auto preparation = prepared_recovery{storage};
   const auto page = protected_page{};
   const auto read = native_fixture{native_fixture::operation::read};
   const auto exhaust = native_fixture{native_fixture::operation::exhaust_stack};
   stack_t original{};
   BOOST_REQUIRE(::sigaltstack(nullptr, &original) == 0);
   sigset_t original_mask;
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &original_mask) == 0);
   const auto memory =
       wasm::invoke_native_with_signals({read.code(), page.bytes(), {}}, storage, read.entry(), page.data());
   BOOST_CHECK(memory.fault == wasm::native_signal_fault::memory);
   BOOST_CHECK(memory.signal == SIGSEGV || memory.signal == SIGBUS);
   const auto stack =
       wasm::invoke_native_with_signals({exhaust.code(), {}, page.bytes()}, storage, exhaust.entry(), page.data());
   BOOST_CHECK(stack.fault == wasm::native_signal_fault::stack);
   stack_t restored{};
   BOOST_REQUIRE(::sigaltstack(nullptr, &restored) == 0);
   if ((original.ss_flags & SS_DISABLE) == 0) {
      BOOST_TEST(restored.ss_sp == original.ss_sp);
      BOOST_TEST(restored.ss_size == original.ss_size);
   }
   BOOST_TEST(restored.ss_flags == original.ss_flags);
   sigset_t restored_mask;
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &restored_mask) == 0);
   BOOST_TEST(same_mask(original_mask, restored_mask));
}

TEST_CASE("typed host exceptions destroy host objects before generated explicit exit", "[native-signals]") {
   auto storage = signal_storage();
   const auto preparation = prepared_recovery{storage};
   auto state = host_state{};
   const auto result = wasm::invoke_native_with_signals({}, storage, &host_bridge_entry, &state);
   BOOST_CHECK(result.fault == wasm::native_signal_fault::explicit_exit);
   BOOST_CHECK(result.status.stage == wasm::signal_boundary_stage::none);
   BOOST_CHECK(state.status.stage == wasm::signal_boundary_stage::none);
   BOOST_TEST(result.signal == 0);
   BOOST_TEST(state.destroyed == 1);
   BOOST_TEST(state.prof_blocked);
   BOOST_CHECK_THROW(std::rethrow_exception(state.exception), callback_failure);
}

TEST_CASE("trivial explicit native exit restores the alternate stack without a hardware signal", "[native-signals]") {
   auto storage = signal_storage();
   const auto preparation = prepared_recovery{storage};
   const auto result = wasm::invoke_native_with_signals(
       {}, storage, [](void*) noexcept { wasm::leave_native_with_signals(); }, nullptr);
   BOOST_CHECK(result.fault == wasm::native_signal_fault::explicit_exit);
   BOOST_TEST(result.signal == 0);
   stack_t restored{};
   BOOST_REQUIRE(::sigaltstack(nullptr, &restored) == 0);
   BOOST_TEST((restored.ss_flags & SS_ONSTACK) == 0);
}

TEST_CASE("native return restores an existing enabled alternate stack", "[native-signals]") {
   auto caller_storage = signal_storage();
   auto native_storage = signal_storage();
   const auto preparation = prepared_recovery{native_storage};
   stack_t original{}, caller{};
   caller.ss_sp = caller_storage.data();
   caller.ss_size = caller_storage.size();
   BOOST_REQUIRE(::sigaltstack(&caller, &original) == 0);
   const auto result = wasm::invoke_native_with_signals({}, native_storage, &empty_entry, nullptr);
   stack_t restored{};
   BOOST_REQUIRE(::sigaltstack(nullptr, &restored) == 0);
   BOOST_CHECK(result.fault == wasm::native_signal_fault::none);
   BOOST_TEST(restored.ss_sp == caller.ss_sp);
   BOOST_TEST(restored.ss_size == caller.ss_size);
   BOOST_TEST(restored.ss_flags == caller.ss_flags);
#if defined(__APPLE__)
   if ((original.ss_flags & SS_DISABLE) != 0)
      original.ss_size = std::max<std::size_t>(original.ss_size, MINSIGSTKSZ);
#endif
   BOOST_REQUIRE(::sigaltstack(&original, nullptr) == 0);
}

TEST_CASE("native nesting across host suspension restores the outer explicit destination", "[native-signals]") {
   auto outer_storage = signal_storage();
   auto inner_storage = signal_storage();
   const auto preparation = prepared_recovery{outer_storage};
   const auto page = protected_page{};
   const auto fixture = native_fixture{native_fixture::operation::read};
   auto nested_storage = std::span<std::byte>{inner_storage};
#if defined(__APPLE__) && defined(__aarch64__)
   nested_storage = outer_storage; // Same prepared per-thread Mach landing reserve, not a second lease.
#endif
   auto state = nested_state{nested_storage, {fixture.code(), page.bytes(), {}}, fixture.entry(), page.data()};
   const auto result = wasm::invoke_native_with_signals({}, outer_storage, &nested_entry, &state);
   BOOST_TEST(state.completed);
   BOOST_CHECK(result.fault == wasm::native_signal_fault::explicit_exit);
}

TEST_CASE("native masks publish before entry and restore after teardown", "[native-signals]") {
   const auto actions = action_guard{SIGFPE};
   const auto masks = mask_guard{};
   struct sigaction action{};
   action.sa_handler = &forward_simple;
   sigemptyset(&action.sa_mask);
   BOOST_REQUIRE(::sigaction(SIGFPE, &action, nullptr) == 0);
   sigset_t block;
   sigemptyset(&block);
   sigaddset(&block, SIGFPE);
   BOOST_REQUIRE(::pthread_sigmask(SIG_BLOCK, &block, nullptr) == 0);
   forwarded = 0;
   ::raise(SIGFPE);
   auto storage = signal_storage();
   const auto preparation = prepared_recovery{storage};
   static_cast<void>(wasm::invoke_native_with_signals({}, storage, &empty_entry, nullptr));
   BOOST_TEST(forwarded == 1);
   sigset_t restored;
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &restored) == 0);
   BOOST_TEST(sigismember(&restored, SIGFPE) == 1);
   BOOST_REQUIRE(::pthread_sigmask(SIG_UNBLOCK, &block, nullptr) == 0);
   static_cast<void>(wasm::invoke_native_with_signals({}, storage, &queue_exit_signal, nullptr));
   BOOST_TEST(forwarded == 2);
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &restored) == 0);
   BOOST_TEST(sigismember(&restored, SIGFPE) == 0);
}

TEST_CASE("native setup rejects an undersized alternate stack without changing caller state", "[native-signals]") {
   auto storage = std::array<std::byte, 1>{};
   stack_t original{};
   sigset_t original_mask;
   BOOST_REQUIRE(::sigaltstack(nullptr, &original) == 0);
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &original_mask) == 0);
   const auto rejected = wasm::invoke_native_with_signals({}, storage, &empty_entry, nullptr);
   BOOST_CHECK(rejected.status.stage == wasm::signal_boundary_stage::signal_stack);
   BOOST_TEST(rejected.status.rollback_failed == 0U);
   stack_t restored{};
   sigset_t restored_mask;
   BOOST_REQUIRE(::sigaltstack(nullptr, &restored) == 0);
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &restored_mask) == 0);
   if ((original.ss_flags & SS_DISABLE) == 0) {
      BOOST_TEST(restored.ss_sp == original.ss_sp);
      BOOST_TEST(restored.ss_size == original.ss_size);
   }
   BOOST_TEST(restored.ss_flags == original.ss_flags);
   BOOST_TEST(same_mask(original_mask, restored_mask));
   auto valid_storage = signal_storage();
   const auto preparation = prepared_recovery{valid_storage};
   BOOST_CHECK(wasm::invoke_native_with_signals({}, valid_storage, &empty_entry, nullptr).fault ==
               wasm::native_signal_fault::none);
}

TEST_CASE("native setup rejects invocation from an already active alternate stack", "[native-signals]") {
   const auto actions = action_guard{SIGUSR1};
   auto original_storage = signal_storage();
   auto proposed_storage = signal_storage();
   const auto preparation = prepared_recovery{proposed_storage};
   auto state = active_stack_state{proposed_storage};
   active_stack_test = &state;
   stack_t original{}, configured{};
   configured.ss_sp = original_storage.data();
   configured.ss_size = original_storage.size();
   BOOST_REQUIRE(::sigaltstack(&configured, &original) == 0);
   struct sigaction action{};
   action.sa_handler = &active_stack_handler;
   action.sa_flags = SA_ONSTACK;
   sigemptyset(&action.sa_mask);
   BOOST_REQUIRE(::sigaction(SIGUSR1, &action, nullptr) == 0);
   ::raise(SIGUSR1);
   BOOST_TEST(state.rejected == 1);
   BOOST_TEST(state.calls == 0);
   // POSIX ignores size for SS_DISABLE; Darwin's UNIX2003 wrapper nevertheless validates it.
   if ((original.ss_flags & SS_DISABLE) != 0)
      original.ss_size = std::max<std::size_t>(original.ss_size, MINSIGSTKSZ);
   BOOST_REQUIRE(::sigaltstack(&original, nullptr) == 0);
   active_stack_test = nullptr;
}

TEST_CASE("foreign host hardware faults forward instead of jumping through host frames", "[native-signals]") {
   const auto child = fresh_process::run("native_signal_fresh_child_control", "native-foreign");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == 73);
}

TEST_CASE("genuine host hardware faults retain their default fatal disposition", "[native-signals]") {
   const auto child = fresh_process::run("native_signal_fresh_child_control", "native-fatal");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFSIGNALED(child.status));
   BOOST_CHECK(WTERMSIG(child.status) == SIGSEGV || WTERMSIG(child.status) == SIGBUS);
}

TEST_CASE("native empty ranges never recover a real active guest-shaped hardware fault", "[native-signals]") {
   const auto child = fresh_process::run("native_signal_fresh_child_control", "native-empty");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFSIGNALED(child.status));
   BOOST_CHECK(WTERMSIG(child.status) == SIGSEGV || WTERMSIG(child.status) == SIGBUS);
}

TEST_CASE("raw NULL callbacks return POD setup failure without entering application code", "[native-signals]") {
   static_assert(noexcept(wasm::invoke_without_signal_recovery(nullptr, nullptr)));
   static_assert(std::is_trivially_copyable_v<wasm::signal_boundary_status>);
   static_assert(std::is_trivially_destructible_v<wasm::native_signal_result>);
   const auto rejected = wasm::invoke_without_signal_recovery(nullptr, nullptr);
   BOOST_CHECK(rejected.stage == wasm::signal_boundary_stage::entry);
   BOOST_TEST(rejected.rollback_failed == 0U);
   auto entered = false;
   auto short_stack = std::array<std::byte, 1>{};
   const auto native = wasm::invoke_native_with_signals(
       {}, short_stack, [](void* opaque) noexcept { *static_cast<bool*>(opaque) = true; }, &entered);
   BOOST_CHECK(native.status.stage == wasm::signal_boundary_stage::signal_stack);
   BOOST_TEST(!entered);
}

TEST_CASE("native fault ranges and alternate stacks remain per-thread", "[native-signals]") {
   const auto child = fresh_process::run("native_signal_fresh_child_control", "native-concurrent");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == 0);
}

TEST_CASE("native member retirement preserves a live sibling invocation", "[native-signals]") {
   const auto child = fresh_process::run("native_signal_fresh_child_control", "native-sibling");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == 0);
}

TEST_CASE("partial worker startup failure cleans the TLS lease without a stop hook", "[native-signals]") {
   const auto child = fresh_process::run("native_signal_fresh_child_control", "native-partial-start");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == 0);
}

#if defined(__APPLE__) && defined(__aarch64__)
TEST_CASE("nonnull prior thread routes are restored and operationally rejected before guest entry",
          "[native-signals]") {
   const auto child = fresh_process::run("native_signal_fresh_child_control", "native-prior-routes");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == 0);
}
#endif

BOOST_AUTO_TEST_CASE(native_signal_fresh_child_control) {
   const auto* mode = fresh_process::mode();
   if (!mode)
      return;
   fresh_process::disable_core_dumps();
#if defined(__APPLE__) && defined(__aarch64__)
   if (std::strcmp(mode, "native-prior-routes") == 0) {
      run_rejected_prior_routes();
      return;
   }
#endif
   if (std::strcmp(mode, "native-sibling") == 0) {
      run_sibling_retirement();
      return;
   }
   if (std::strcmp(mode, "native-partial-start") == 0) {
      run_partial_start_failure();
      return;
   }
   if (std::strcmp(mode, "native-foreign") == 0 || std::strcmp(mode, "native-fatal") == 0 ||
       std::strcmp(mode, "native-empty") == 0) {
      struct sigaction action{};
      action.sa_sigaction = &foreign_fatal;
      action.sa_flags = SA_SIGINFO;
      if (std::strcmp(mode, "native-foreign") != 0) {
         action.sa_flags = 0;
         action.sa_handler = SIG_DFL;
      }
      sigemptyset(&action.sa_mask);
      if (::sigaction(SIGSEGV, &action, nullptr) != 0 || ::sigaction(SIGBUS, &action, nullptr) != 0)
         std::_Exit(90);
      auto storage = signal_storage();
      const auto preparation = prepared_recovery{storage};
      const auto page = protected_page{};
      const auto fixture = native_fixture{native_fixture::operation::read};
      if (std::strcmp(mode, "native-empty") == 0) {
         static_cast<void>(wasm::invoke_native_with_signals({}, storage, fixture.entry(), page.data()));
         std::_Exit(91);
      }
      static_cast<void>(wasm::invoke_native_with_signals({fixture.code(), page.bytes(), {}}, storage,
                                                         &foreign_host_entry, page.data()));
      std::_Exit(91);
   }
   if (std::strcmp(mode, "native-concurrent") != 0)
      std::_Exit(94);
   auto failures = std::atomic<unsigned>{0};
   const auto run = [&] {
      auto storage = signal_storage();
      const auto preparation = prepared_recovery{storage};
      const auto page = protected_page{};
      const auto fixture = native_fixture{native_fixture::operation::read};
      for (unsigned iteration = 0; iteration < 64; ++iteration) {
         try {
            const auto result = wasm::invoke_native_with_signals({fixture.code(), page.bytes(), {}}, storage,
                                                                 fixture.entry(), page.data());
            if (result.status.stage != wasm::signal_boundary_stage::none ||
                result.fault != wasm::native_signal_fault::memory)
               failures.fetch_add(1);
         } catch (...) {
            failures.fetch_add(1);
         }
      }
   };
   auto first = std::thread{run};
   auto second = std::thread{run};
   first.join();
   second.join();
   BOOST_TEST(failures.load() == 0);
}
