module;

#include "test_prelude.hpp"
#include "fresh_process.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <latch>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

module forge.vm.wasm.interpret.backend;

import :signals;
import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.utils;

#include "test_support.hpp"

#define FORGE_VM_WASM_INTERPRET_TEST_FILE signals_regression_tests

namespace wasm = forge::vm::wasm::interpret;

namespace {
struct reexport_host {
   static std::uint32_t read(wasm::argument_proxy<const char*> value) {
      return static_cast<std::uint32_t>(*value.get());
   }
};

struct escaped_signal {};

using reexport_host_functions = wasm::registered_host_functions<std::nullptr_t>;

volatile std::sig_atomic_t external_signal_count = 0;
volatile std::sig_atomic_t prof_count = 0;
volatile std::sig_atomic_t prof_cleanup = 0;
volatile std::sig_atomic_t prof_unsafe = 0;
volatile std::sig_atomic_t forwarded_cleanup = 0;
volatile std::sig_atomic_t forwarded_active = 0;
volatile std::sig_atomic_t forwarded_behavior = 0;
volatile unsigned char* forwarded_fault = nullptr;

void guarded_foreign_handler(int) {
   struct object {
      ~object() {
         forwarded_cleanup = forwarded_cleanup + 1;
      }
   };
   const auto lifetime = object{};
   forwarded_active = wasm::signal_dest.load() != nullptr;
   if (forwarded_behavior == 1)
      throw escaped_signal{};
   if (forwarded_behavior == 2)
      *forwarded_fault = 1;
}

void guarded_foreign_detailed_handler(int signal, siginfo_t*, void*) {
   guarded_foreign_handler(signal);
}

void deferred_prof_handler(int) {
   prof_count = prof_count + 1;
   if (prof_cleanup == 0)
      prof_unsafe = 1;
}

void external_signal_handler(int) {
   external_signal_count = external_signal_count + 1;
}

void external_detailed_signal_handler(int, siginfo_t*, void*) {
   external_signal_count = external_signal_count + 1;
}

class signal_action_guard {
 public:
   explicit signal_action_guard(int signal) : signal_{signal} {
      BOOST_REQUIRE(::sigaction(signal_, nullptr, &previous_) == 0);
   }

   ~signal_action_guard() {
      static_cast<void>(::sigaction(signal_, &previous_, nullptr));
   }

 private:
   int signal_;
   struct sigaction previous_{};
};

void register_reexport_host() {
   static const auto registered = [] {
      reexport_host_functions::add<&reexport_host::read>("host", "read");
      return true;
   }();
   static_cast<void>(registered);
}

template <typename Implementation> void check_reexported_host_import() {
   register_reexport_host();

   auto code = wasm::wasm_code{
       0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // header
       0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f, // (i32) -> i32
       0x02, 0x0d, 0x01, 0x04, 0x68, 0x6f, 0x73, 0x74, 0x04, 0x72,
       0x65, 0x61, 0x64, 0x00, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01, // one-page linear memory
       0x07, 0x08, 0x01, 0x04, 0x72, 0x65, 0x61, 0x64, 0x00, 0x00  // re-export imported read
   };
   using runtime = wasm::backend<reexport_host_functions, Implementation>;
   auto memory = wasm::wasm_allocator{};
   auto instance = runtime{code, &memory};

   const auto invoke = [&] {
      wasm::invoke_with_signal_handler(
          [&] {
             wasm::longjmp_on_exception([&] { instance("env", "read", std::numeric_limits<std::uint32_t>::max()); });
          },
          [](int) { throw escaped_signal{}; }, nullptr, &memory);
   };

   BOOST_CHECK_THROW(invoke(), wasm::exceptions::memory);
   memory.free();
}
} // namespace

TEST_CASE("nested signal scopes restore the outer memory range", "[signals]") {
   auto outer_memory = wasm::wasm_allocator{};
   auto inner_memory = wasm::wasm_allocator{};

   wasm::invoke_with_signal_handler(
       [&] {
          wasm::longjmp_on_exception([&] {
             const auto expected = wasm::memory_range;
             wasm::invoke_with_signal_handler([] {}, [](int) {}, nullptr, &inner_memory);
             BOOST_TEST(wasm::memory_range.data() == expected.data());
             BOOST_TEST(wasm::memory_range.size() == expected.size());
          });
       },
       [](int) {}, nullptr, &outer_memory);

   outer_memory.free();
   inner_memory.free();
}

TEST_CASE("signal ranges classify addresses without pointer ordering", "[signals]") {
   auto bytes = std::array<std::byte, 8>{};
   const auto range = std::span<const std::byte>{bytes};
   const auto start = reinterpret_cast<std::uintptr_t>(range.data());

   BOOST_TEST(wasm::detail::contains_address(range, start));
   BOOST_TEST(wasm::detail::contains_address(range, start + range.size() - 1));
   BOOST_TEST(!wasm::detail::contains_address(range, start - 1));
   BOOST_TEST(!wasm::detail::contains_address(range, start + range.size()));
   BOOST_TEST(!wasm::detail::contains_address({}, start));
}

BOOST_AUTO_TEST_CASE(private_empty_legacy_recovers_raised_managed_signals) {
   for (const auto signal : {SIGSEGV,
#ifndef __linux__
                             SIGBUS,
#endif
                             SIGFPE}) {
      auto caught_signal = 0;
      wasm::invoke_with_signal_handler([signal] { std::raise(signal); }, [&](int caught) { caught_signal = caught; },
                                       nullptr, nullptr);
      BOOST_TEST(caught_signal == signal);
      BOOST_CHECK(wasm::signal_dest.load() == nullptr);
   }
}

BOOST_AUTO_TEST_CASE(nonempty_legacy_raised_signals_preserve_foreign_forwarding) {
   auto memory = wasm::wasm_allocator{};
   BOOST_REQUIRE(!memory.get_span().empty());
   for (const auto signal : {SIGSEGV,
#ifndef __linux__
                             SIGBUS,
#endif
                             SIGFPE}) {
      const auto actions = signal_action_guard{signal};
      struct sigaction external{};
      external.sa_sigaction = &external_detailed_signal_handler;
      external.sa_flags = SA_SIGINFO;
      sigemptyset(&external.sa_mask);
      BOOST_REQUIRE(::sigaction(signal, &external, nullptr) == 0);
      external_signal_count = 0;
      auto caught_signal = 0;
      wasm::invoke_with_signal_handler([signal] { std::raise(signal); }, [&](int caught) { caught_signal = caught; },
                                       nullptr, &memory);
      BOOST_TEST(caught_signal == 0);
      BOOST_TEST(external_signal_count == 1);
      BOOST_CHECK(wasm::signal_dest.load() == nullptr);
   }
   memory.free();
}

BOOST_AUTO_TEST_CASE(forwarded_legacy_cpp_callbacks_suspend_canonical_recovery) {
   auto memory = wasm::wasm_allocator{};
   for (const auto detailed : {false, true}) {
      const auto actions = signal_action_guard{SIGFPE};
      struct sigaction external{};
      external.sa_flags = detailed ? SA_SIGINFO : 0;
      if (detailed)
         external.sa_sigaction = &guarded_foreign_detailed_handler;
      else
         external.sa_handler = &guarded_foreign_handler;
      sigemptyset(&external.sa_mask);
      BOOST_REQUIRE(::sigaction(SIGFPE, &external, nullptr) == 0);
      for (const auto failure : {false, true}) {
         forwarded_cleanup = 0;
         forwarded_active = 0;
         forwarded_behavior = failure ? 1 : 0;
         auto restored = false;
         auto caught = false;
         try {
            wasm::invoke_with_signal_handler(
                [&] {
                   const auto* destination = wasm::signal_dest.load();
                   ::raise(SIGFPE);
                   restored = destination != nullptr && wasm::signal_dest.load() == destination;
                },
                [](int) { BOOST_ERROR("foreign callback must not become a guest signal trap"); }, nullptr, &memory);
         } catch (const escaped_signal&) {
            caught = true;
         }
         BOOST_TEST(caught == failure);
         BOOST_TEST(restored == !failure);
         BOOST_TEST(forwarded_active == 0);
         BOOST_TEST(forwarded_cleanup == 1);
         BOOST_CHECK(wasm::signal_dest.load() == nullptr);
      }
   }
   forwarded_behavior = 0;
   memory.free();
}

BOOST_AUTO_TEST_CASE(foreign_legacy_signal_callback_faults_cannot_reenter_guest_recovery) {
   for (const auto* mode : {"legacy-forwarded-simple-fatal", "legacy-forwarded-detailed-fatal"}) {
      const auto child = fresh_process::run("legacy_recovery_fresh_child_control", mode);
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFSIGNALED(child.status));
      BOOST_CHECK(WTERMSIG(child.status) == SIGSEGV || WTERMSIG(child.status) == SIGBUS);
   }
}

BOOST_AUTO_TEST_CASE(native_throwing_foreign_signal_handlers_terminate_while_null) {
   for (const auto* mode : {"native-throw-simple", "native-throw-detailed"}) {
      const auto child = fresh_process::run("legacy_recovery_fresh_child_control", mode);
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFSIGNALED(child.status));
      BOOST_TEST(WTERMSIG(child.status) == SIGABRT);
   }
}

TEST_CASE("legacy recovery preserves disabled and enabled caller signal stacks", "[signals]") {
   // Each path has a fresh process: a failing restoration must not cascade through later cases or free live stack.
   for (auto enabled = 0; enabled < 2; ++enabled) {
      for (auto path = 0; path < 3; ++path) {
         const auto mode = std::string{"legacy-"} + std::to_string(enabled) + "-" + std::to_string(path);
         const auto child = fresh_process::run("legacy_recovery_fresh_child_control", mode);
         BOOST_REQUIRE(child.error == 0);
         BOOST_REQUIRE(!child.timed_out);
         BOOST_CHECK(WIFEXITED(child.status));
         if (WIFEXITED(child.status))
            BOOST_CHECK_EQUAL(WEXITSTATUS(child.status), 0);
      }
   }
}

BOOST_AUTO_TEST_CASE(legacy_recovery_fresh_child_control) {
   const auto* mode = fresh_process::mode();
   if (!mode)
      return;
   fresh_process::disable_core_dumps();
   if (std::strcmp(mode, "native-throw-simple") == 0 || std::strcmp(mode, "native-throw-detailed") == 0) {
      ::signal(SIGABRT, SIG_DFL);
      sigset_t abort_mask;
      sigemptyset(&abort_mask);
      sigaddset(&abort_mask, SIGABRT);
      if (::pthread_sigmask(SIG_UNBLOCK, &abort_mask, nullptr) != 0)
         std::_Exit(90);
      std::set_terminate([] {
         // This owning internal test can observe the private canonical TLS;
         // no public signal-query/export or recursive terminate hook is added.
         if (wasm::signal_dest.load() != nullptr)
            std::_Exit(97);
         ::raise(SIGABRT);
         std::_Exit(98);
      });
      struct sigaction external{};
      if (std::strcmp(mode, "native-throw-detailed") == 0) {
         external.sa_flags = SA_SIGINFO;
         external.sa_sigaction = [](int, siginfo_t*, void*) { throw escaped_signal{}; };
      } else {
         external.sa_handler = [](int) { throw escaped_signal{}; };
      }
      sigemptyset(&external.sa_mask);
      if (::sigaction(SIGFPE, &external, nullptr) != 0)
         std::_Exit(90);
      auto storage = std::vector<std::byte>(128 * 1024);
      if (wasm::prepare_signal_recovery_thread(storage).stage != wasm::signal_boundary_stage::none)
         std::_Exit(90);
      static_cast<void>(
          wasm::invoke_native_with_signals({}, storage, [](void*) noexcept { ::raise(SIGFPE); }, nullptr));
      std::_Exit(99);
   }
   if (std::strcmp(mode, "legacy-forwarded-simple-fatal") == 0 ||
       std::strcmp(mode, "legacy-forwarded-detailed-fatal") == 0) {
      ::signal(SIGSEGV, SIG_DFL);
      ::signal(SIGBUS, SIG_DFL);
      struct sigaction external{};
      if (std::strcmp(mode, "legacy-forwarded-detailed-fatal") == 0) {
         external.sa_flags = SA_SIGINFO;
         external.sa_sigaction = &guarded_foreign_detailed_handler;
      } else {
         external.sa_handler = &guarded_foreign_handler;
      }
      sigemptyset(&external.sa_mask);
      if (::sigaction(SIGFPE, &external, nullptr) != 0)
         std::_Exit(90);
      auto memory = wasm::wasm_allocator{};
      forwarded_fault = memory.get_base_ptr<unsigned char>() + wasm::page_size;
      forwarded_behavior = 2;
      wasm::invoke_with_signal_handler([] { ::raise(SIGFPE); }, [](int) { std::_Exit(97); }, nullptr, &memory);
      std::_Exit(98);
   }
   if (std::strcmp(mode, "legacy-host-fatal") == 0) {
      ::signal(SIGSEGV, SIG_DFL);
      ::signal(SIGBUS, SIG_DFL);
      auto* memory =
          ::mmap(nullptr, static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (memory == MAP_FAILED)
         std::_Exit(92);
      const auto fault = [memory] { *static_cast<volatile unsigned char*>(memory) = 1; };
      const auto nested = [&] {
         wasm::invoke_with_signal_handler([&] { wasm::longjmp_on_exception(fault); }, [](int) { std::_Exit(98); },
                                          nullptr, nullptr);
      };
      wasm::invoke_with_signal_handler([&] { wasm::longjmp_on_exception(nested); }, [](int) { std::_Exit(98); },
                                       nullptr, nullptr);
      std::_Exit(99);
   }
   if (std::strlen(mode) != 10 || std::strncmp(mode, "legacy-", 7) != 0 || mode[8] != '-' ||
       (mode[7] != '0' && mode[7] != '1') || mode[9] < '0' || mode[9] > '2')
      std::_Exit(96);
   const auto enabled = mode[7] - '0';
   const auto path = mode[9] - '0';
   auto signal_stack = std::vector<std::byte>(128 * 1024);
   stack_t supplied{};
   supplied.ss_sp = enabled ? signal_stack.data() : nullptr;
   supplied.ss_size = enabled ? signal_stack.size() : MINSIGSTKSZ;
   supplied.ss_flags = enabled ? 0 : SS_DISABLE;
   if (::sigaltstack(&supplied, nullptr) != 0)
      std::_Exit(90);
   stack_t expected{};
   if (::sigaltstack(nullptr, &expected) != 0 || (expected.ss_flags & SS_ONSTACK) != 0)
      std::_Exit(91);
   auto* protected_memory =
       ::mmap(nullptr, static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
   if (protected_memory == MAP_FAILED)
      std::_Exit(92);

   for (auto iteration = 0; iteration < 32; ++iteration) {
      auto caught = false;
      try {
         if (path == 0) {
            wasm::invoke_with_signal_handler([] {}, [](int) {}, nullptr, nullptr);
         } else if (path == 1) {
            wasm::invoke_with_signal_handler([] { wasm::longjmp_on_exception([] { throw escaped_signal{}; }); },
                                             [](int) {}, nullptr, nullptr);
         } else {
            wasm::invoke_with_signal_handler(
                [protected_memory] { *static_cast<volatile unsigned char*>(protected_memory) = 1; },
                [](int) { throw escaped_signal{}; }, nullptr, nullptr);
         }
      } catch (const escaped_signal&) {
         caught = true;
      } catch (...) {
         std::_Exit(93);
      }
      if (caught != (path != 0))
         std::_Exit(94);
      stack_t current{};
      if (::sigaltstack(nullptr, &current) != 0 || current.ss_flags != expected.ss_flags ||
          current.ss_sp != expected.ss_sp || current.ss_size != expected.ss_size)
         std::_Exit(95);
   }
   // The process exits without destructors: even a failing test cannot free an OS-referenced signal stack.
   std::_Exit(0);
}

TEST_CASE("nested private legacy checkpoints cannot catch a genuine whole NULL host fault", "[signals]") {
   const auto child = fresh_process::run("legacy_recovery_fresh_child_control", "legacy-host-fatal");
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFSIGNALED(child.status));
   BOOST_CHECK(WTERMSIG(child.status) == SIGSEGV || WTERMSIG(child.status) == SIGBUS);
}

TEST_CASE("local signal readiness status maps to neutral operational exceptions", "[signals]") {
   for (const auto stage : {wasm::signal_boundary_stage::platform, wasm::signal_boundary_stage::block_entry,
                            wasm::signal_boundary_stage::query_stack, wasm::signal_boundary_stage::active_stack,
                            wasm::signal_boundary_stage::install_stack, wasm::signal_boundary_stage::acquire_handlers,
                            wasm::signal_boundary_stage::enter_mask, wasm::signal_boundary_stage::thread_prepare,
                            wasm::signal_boundary_stage::thread_release, wasm::signal_boundary_stage::thread_capacity,
                            wasm::signal_boundary_stage::recovery_not_ready, wasm::signal_boundary_stage::fork_child,
                            wasm::signal_boundary_stage::mach_service, wasm::signal_boundary_stage::mach_routes}) {
      BOOST_CHECK_THROW(wasm::check_signal_boundary_status({stage, EAGAIN, 0}),
                        wasm::exceptions::execution_unavailable);
   }
   BOOST_CHECK_THROW(wasm::check_signal_boundary_status({wasm::signal_boundary_stage::entry, EINVAL, 0}),
                     wasm::exceptions::interpreter);
   BOOST_CHECK_THROW(wasm::check_signal_boundary_status({wasm::signal_boundary_stage::signal_stack, EINVAL, 0}),
                     wasm::exceptions::interpreter);
   BOOST_CHECK_NO_THROW(wasm::check_signal_boundary_status({}));
}

TEST_CASE("signal handlers are restored after an external replacement", "[signals]") {
   const auto guard = signal_action_guard{SIGFPE};

   struct sigaction external{};
   external.sa_handler = &external_signal_handler;
   sigemptyset(&external.sa_mask);
   external.sa_flags = 0;
   BOOST_REQUIRE(::sigaction(SIGFPE, &external, nullptr) == 0);
   external_signal_count = 0;

   const auto invoke = [] {
      wasm::invoke_with_signal_handler([] { std::raise(SIGFPE); }, [](int) { throw escaped_signal{}; }, nullptr,
                                       nullptr);
   };
   BOOST_CHECK_THROW(invoke(), escaped_signal);
   BOOST_TEST(external_signal_count == 0);

   std::raise(SIGFPE);
   BOOST_TEST(external_signal_count == 1);
}

TEST_CASE("reinstalling a managed signal handler preserves the displaced action", "[signals]") {
   const auto guard = signal_action_guard{SIGFPE};
   struct sigaction external{};
   external.sa_handler = &external_signal_handler;
   sigemptyset(&external.sa_mask);
   external.sa_flags = 0;
   BOOST_REQUIRE(::sigaction(SIGFPE, &external, nullptr) == 0);
   external_signal_count = 0;

   wasm::acquire_signal_handler_scope();
   {
      const auto release = wasm::scope_guard{[] { wasm::release_signal_handler_scope(); }};
      wasm::setup_signal_handler_impl();
   }

   struct sigaction restored{};
   BOOST_REQUIRE(::sigaction(SIGFPE, nullptr, &restored) == 0);
   BOOST_TEST((restored.sa_flags & SA_SIGINFO) == 0);
   BOOST_TEST(restored.sa_handler == &external_signal_handler);
   std::raise(SIGFPE);
   BOOST_TEST(external_signal_count == 1);
}

TEST_CASE("signal handler publication remains coherent during concurrent ownership changes", "[signals]") {
   constexpr auto iterations = 512;
   const auto guard = signal_action_guard{SIGFPE};
   auto start = std::barrier{2};
   auto active = std::barrier{2};
   auto release = std::barrier{2};
   auto restored = std::barrier{2};
   auto install_failed = std::atomic<bool>{false};
   external_signal_count = 0;

   auto installer = std::thread{[&] {
      for (auto iteration = 0; iteration < iterations; ++iteration) {
         struct sigaction external{};
         sigemptyset(&external.sa_mask);
         if ((iteration % 3) == 0) {
            external.sa_handler = &external_signal_handler;
            external.sa_flags = 0;
         } else {
            external.sa_sigaction = &external_detailed_signal_handler;
            external.sa_flags = SA_SIGINFO;
         }
         if (::sigaction(SIGFPE, &external, nullptr) != 0) {
            install_failed.store(true);
         }

         start.arrive_and_wait();
         try {
            wasm::invoke_with_signal_handler(
                [&] {
                   active.arrive_and_wait();
                   release.arrive_and_wait();
                },
                [](int) {}, nullptr, nullptr);
         } catch (...) {
            install_failed.store(true);
         }
         restored.arrive_and_wait();
      }
   }};

   for ([[maybe_unused]] auto iteration = 0; iteration < iterations; ++iteration) {
      start.arrive_and_wait();
      active.arrive_and_wait();
      if ((iteration % 3) == 2) {
         struct sigaction replacement{};
         replacement.sa_handler = &external_signal_handler;
         sigemptyset(&replacement.sa_mask);
         replacement.sa_flags = 0;
         if (::sigaction(SIGFPE, &replacement, nullptr) != 0) {
            install_failed.store(true);
         }
      }
      std::raise(SIGFPE);
      release.arrive_and_wait();
      restored.arrive_and_wait();
      std::raise(SIGFPE);
   }
   installer.join();

   BOOST_TEST(!install_failed.load());
   BOOST_TEST(external_signal_count == iterations * 2);
}

TEST_CASE("concurrent outer signal scopes restore handlers after the last invocation", "[signals]") {
   const auto guard = signal_action_guard{SIGFPE};
   struct sigaction external{};
   external.sa_handler = &external_signal_handler;
   sigemptyset(&external.sa_mask);
   external.sa_flags = 0;
   BOOST_REQUIRE(::sigaction(SIGFPE, &external, nullptr) == 0);
   external_signal_count = 0;

   auto entered = std::barrier{3};
   auto release_first = std::latch{1};
   auto release_second = std::latch{1};
   auto invocation_failed = std::atomic<bool>{false};
   const auto run = [&](std::latch& release) {
      try {
         wasm::invoke_with_signal_handler(
             [&] {
                entered.arrive_and_wait();
                release.wait();
             },
             [](int) {}, nullptr, nullptr);
      } catch (...) {
         invocation_failed.store(true);
      }
   };

   auto first = std::thread{run, std::ref(release_first)};
   auto second = std::thread{run, std::ref(release_second)};
   entered.arrive_and_wait();

   release_first.count_down();
   first.join();
   struct sigaction current{};
   const auto active_query = ::sigaction(SIGFPE, nullptr, &current);
   BOOST_CHECK(active_query == 0);
   if (active_query == 0) {
      BOOST_CHECK((current.sa_flags & SA_SIGINFO) != 0);
      BOOST_CHECK(current.sa_sigaction == &wasm::signal_handler);
   }
   std::raise(SIGFPE);
   BOOST_TEST(external_signal_count == 1);

   release_second.count_down();
   second.join();
   const auto restored_query = ::sigaction(SIGFPE, nullptr, &current);
   BOOST_CHECK(restored_query == 0);
   if (restored_query == 0) {
      BOOST_CHECK((current.sa_flags & SA_SIGINFO) == 0);
      BOOST_CHECK(current.sa_handler == &external_signal_handler);
   }
   std::raise(SIGFPE);
   BOOST_TEST(external_signal_count == 2);
   BOOST_TEST(!invocation_failed.load());
}

TEST_CASE("interpreter re-exported host imports translate guest memory faults", "[signals]") {
   check_reexported_host_import<wasm::interpreter>();
}

TEST_CASE("whole NULL callback preserves local nested catch and guard lifetime", "[signals]") {
   struct lifetime_state {
      wasm::wasm_allocator* inner_memory;
      bool constructor_suspended = false;
      bool destructor_suspended = false;
      bool inner_active = false;
      bool fail = false;
      bool caught = false;
      bool continued = false;
      bool outer_restored = false;
      wasm::signal_boundary_status status;
   };
   struct lifetime_probe {
      lifetime_state* state;
      explicit lifetime_probe(lifetime_state* value) : state(value) {
         state->constructor_suspended = wasm::signal_dest.load() == nullptr;
      }
      ~lifetime_probe() {
         state->destructor_suspended = wasm::signal_dest.load() == nullptr;
      }
   };
   auto outer_memory = wasm::wasm_allocator{};
   auto inner_memory = wasm::wasm_allocator{};
   for (const auto fail : {false, true}) {
      auto state = lifetime_state{.inner_memory = &inner_memory, .fail = fail};
      wasm::invoke_with_signal_handler(
          [&] {
             const auto previous = wasm::signal_dest.load();
             const auto callback = [](void* opaque) noexcept {
                auto& state = *static_cast<lifetime_state*>(opaque);
                const auto probe = lifetime_probe{&state};
                try {
                   wasm::invoke_with_signal_handler(
                       [&] {
                          state.inner_active = wasm::signal_dest.load() != nullptr;
                          if (state.fail)
                             wasm::longjmp_on_exception([] { throw escaped_signal{}; });
                       },
                       [](int) {}, nullptr, state.inner_memory);
                } catch (const escaped_signal&) {
                   state.caught = true;
                }
                state.continued = true;
             };
             state.status = wasm::invoke_without_signal_recovery(callback, &state);
             state.outer_restored = wasm::signal_dest.load() == previous;
          },
          [](int) {}, nullptr, &outer_memory);
      BOOST_TEST(state.constructor_suspended);
      BOOST_TEST(state.destructor_suspended);
      BOOST_TEST(state.inner_active);
      BOOST_TEST(state.caught == fail);
      BOOST_TEST(state.continued);
      BOOST_TEST(state.outer_restored);
      BOOST_CHECK(state.status.stage == wasm::signal_boundary_stage::none);
   }
   outer_memory.free();
   inner_memory.free();
}

TEST_CASE("uncaught typed host failure leaves only after the complete callback unwinds", "[signals]") {
   struct lifetime {
      int* destroyed;
      bool* suspended;
      ~lifetime() {
         ++*destroyed;
         *suspended = wasm::signal_dest.load() == nullptr;
      }
   };
   auto destroyed = 0;
   auto suspended = false;
   const auto invoke = [&] {
      wasm::invoke_with_signal_handler(
          [&] {
             wasm::longjmp_on_exception([&] {
                const auto object = lifetime{&destroyed, &suspended};
                throw escaped_signal{};
             });
          },
          [](int) {}, nullptr, nullptr);
   };
   BOOST_CHECK_THROW(invoke(), escaped_signal);
   BOOST_TEST(destroyed == 1);
   BOOST_TEST(suspended);
   BOOST_CHECK(wasm::signal_dest.load() == nullptr);
}

TEST_CASE("legacy signal error callback and typed tail stay in their NULL parent", "[signals]") {
   auto null_tail = false;
   const auto invoke = [&] {
      wasm::invoke_with_signal_handler([] { std::raise(SIGFPE); },
                                       [&](int) {
                                          null_tail = wasm::signal_dest.load() == nullptr;
                                          throw escaped_signal{};
                                       },
                                       nullptr, nullptr);
   };
   BOOST_CHECK_THROW(invoke(), escaped_signal);
   BOOST_TEST(null_tail);
}

TEST_CASE("NULL-mode adapter masks host lifetimes and preserves local catches", "[signals]") {
   struct state {
      int constructed = 0;
      int destroyed = 0;
      bool guarded = true;
   } active;
   const auto is_guarded = [] {
      sigset_t mask;
      return wasm::signal_dest.load() == nullptr && ::pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0 &&
             sigismember(&mask, SIGPROF) == 1;
   };
   struct probe {
      state* active;
      decltype(is_guarded)* guarded;
      probe(state* value, decltype(is_guarded)* check) : active(value), guarded(check) {
         ++active->constructed;
         active->guarded &= (*guarded)();
      }
      ~probe() {
         ++active->destroyed;
         active->guarded &= (*guarded)();
      }
   };
   sigset_t original, unblocked;
   sigemptyset(&unblocked);
   sigaddset(&unblocked, SIGPROF);
   BOOST_REQUIRE(::pthread_sigmask(SIG_UNBLOCK, &unblocked, &original) == 0);
   const auto restore = wasm::scope_guard{[&] { ::pthread_sigmask(SIG_SETMASK, &original, nullptr); }};
   BOOST_CHECK(wasm::signal_dest.load() == nullptr);
   for (const auto failure : {false, true}) {
      auto caught = false;
      auto continued = false;
      wasm::longjmp_on_exception([&] {
         const auto outer = probe{&active, &is_guarded};
         try {
            wasm::longjmp_on_exception([&] {
               const auto inner = probe{&active, &is_guarded};
               if (failure)
                  throw escaped_signal{};
            });
         } catch (const escaped_signal&) {
            caught = true;
            active.guarded &= is_guarded();
         }
         continued = true;
      });
      BOOST_TEST(caught == failure);
      BOOST_TEST(continued);
      sigset_t restored;
      BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &restored) == 0);
      BOOST_TEST(sigismember(&restored, SIGPROF) == 0);
      BOOST_CHECK(wasm::signal_dest.load() == nullptr);
   }
   BOOST_CHECK_THROW(wasm::longjmp_on_exception([&] {
                        const auto object = probe{&active, &is_guarded};
                        throw escaped_signal{};
                     }),
                     escaped_signal);
   sigset_t restored;
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &restored) == 0);
   BOOST_TEST(sigismember(&restored, SIGPROF) == 0);
   BOOST_CHECK(wasm::signal_dest.load() == nullptr);
   BOOST_TEST(active.constructed == 5);
   BOOST_TEST(active.destroyed == 5);
   BOOST_TEST(active.guarded);
}

TEST_CASE("NULL-mode queued SIGPROF delivery follows cleanup and preserves an originally blocked mask", "[signals]") {
   const auto action_restore = signal_action_guard{SIGPROF};
   struct sigaction action{};
   action.sa_handler = &deferred_prof_handler;
   sigemptyset(&action.sa_mask);
   BOOST_REQUIRE(::sigaction(SIGPROF, &action, nullptr) == 0);
   sigset_t original, prof;
   sigemptyset(&prof);
   sigaddset(&prof, SIGPROF);
   BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &original) == 0);
   const auto mask_restore = wasm::scope_guard{[&] { ::pthread_sigmask(SIG_SETMASK, &original, nullptr); }};
   struct cleanup {
      ~cleanup() {
         prof_cleanup = 1;
      }
   };
   for (const auto originally_blocked : {false, true}) {
      for (const auto failure : {false, true}) {
         BOOST_REQUIRE(::pthread_sigmask(originally_blocked ? SIG_BLOCK : SIG_UNBLOCK, &prof, nullptr) == 0);
         prof_count = 0;
         prof_cleanup = 0;
         prof_unsafe = 0;
         auto deferred = false;
         auto caught = false;
         try {
            wasm::longjmp_on_exception([&] {
               const auto object = cleanup{};
               ::raise(SIGPROF);
               deferred = prof_count == 0;
               if (failure)
                  throw escaped_signal{};
            });
         } catch (const escaped_signal&) {
            caught = true;
         }
         BOOST_TEST(caught == failure);
         BOOST_TEST(deferred);
         BOOST_TEST(prof_cleanup == 1);
         BOOST_CHECK(wasm::signal_dest.load() == nullptr);
         sigset_t restored, pending;
         BOOST_REQUIRE(::pthread_sigmask(SIG_SETMASK, nullptr, &restored) == 0);
         BOOST_REQUIRE(::sigpending(&pending) == 0);
         BOOST_TEST(sigismember(&restored, SIGPROF) == static_cast<int>(originally_blocked));
         BOOST_TEST(sigismember(&pending, SIGPROF) == static_cast<int>(originally_blocked));
         BOOST_TEST(prof_count == (originally_blocked ? 0 : 1));
         if (originally_blocked)
            BOOST_REQUIRE(::pthread_sigmask(SIG_UNBLOCK, &prof, nullptr) == 0);
         BOOST_TEST(prof_count == 1);
         BOOST_TEST(prof_unsafe == 0);
      }
   }
}

TEST_CASE("native asynchronous siginfo forwards even when the injected PC and address are owned", "[signals]") {
   BOOST_REQUIRE(wasm::release_signal_recovery_thread().stage == wasm::signal_boundary_stage::none);
   const auto fpe_guard = signal_action_guard{SIGFPE};
   const auto segv_guard = signal_action_guard{SIGSEGV};
   auto code = std::array<std::byte, 8>{};
   auto memory = std::array<std::byte, 8>{};
   auto storage = std::vector<std::byte>(std::max<std::size_t>(SIGSTKSZ, 128 * 1024));
   ucontext_t context{};
#if defined(__APPLE__)
   std::remove_pointer_t<mcontext_t> machine{};
   context.uc_mcontext = &machine;
#if defined(__aarch64__)
   machine.__ss.__pc = reinterpret_cast<std::uintptr_t>(code.data());
#else
   machine.__ss.__rip = reinterpret_cast<std::uintptr_t>(code.data());
#endif
#elif defined(__x86_64__)
   context.uc_mcontext.gregs[REG_RIP] = reinterpret_cast<std::uintptr_t>(code.data());
#elif defined(__aarch64__)
   context.uc_mcontext.pc = reinterpret_cast<std::uintptr_t>(code.data());
#endif
   struct injection {
      ucontext_t* context;
      siginfo_t info;
      int signal;
   };
   for (const auto detailed : {false, true}) {
      struct sigaction external{};
      sigemptyset(&external.sa_mask);
      if (detailed) {
         external.sa_flags = SA_SIGINFO;
         external.sa_sigaction = &external_detailed_signal_handler;
      } else {
         external.sa_handler = &external_signal_handler;
      }
      for (const auto signal : {SIGFPE, SIGSEGV}) {
         BOOST_REQUIRE(::sigaction(signal, &external, nullptr) == 0);
         BOOST_REQUIRE(wasm::prepare_signal_recovery_thread(storage).stage == wasm::signal_boundary_stage::none);
         for (const auto code_value : {SI_USER, SI_QUEUE}) {
            auto state = injection{&context, {}, signal};
            state.info.si_signo = signal;
            state.info.si_code = code_value;
            state.info.si_addr = memory.data();
            external_signal_count = 0;
            const auto result = wasm::invoke_native_with_signals(
                {code, memory, {}}, storage,
                [](void* opaque) noexcept {
                   auto& state = *static_cast<injection*>(opaque);
                   wasm::signal_handler(state.signal, &state.info, state.context);
                },
                &state);
            BOOST_CHECK(result.fault == wasm::native_signal_fault::none);
            BOOST_TEST(external_signal_count == 1);
         }
         BOOST_REQUIRE(wasm::release_signal_recovery_thread().stage == wasm::signal_boundary_stage::none);
      }
   }
}

#if FORGE_VM_WASM_INTERPRET_HAS_JIT && !defined(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
TEST_CASE("jit re-exported host imports translate guest memory faults", "[signals]") {
   check_reexported_host_import<wasm::jit>();
}
#endif
