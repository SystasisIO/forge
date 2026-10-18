module;

#include "test_prelude.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <functional>
#include <latch>
#include <limits>
#include <thread>

module forge.vm.wasm.interpret.backend;

import :signals;
import forge.vm.wasm.interpret.allocator;

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
      wasm::invoke_with_signal_handler([&] { instance("env", "read", std::numeric_limits<std::uint32_t>::max()); },
                                       [](int) { throw escaped_signal{}; }, {}, &memory);
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
          const auto expected = wasm::memory_range;
          wasm::invoke_with_signal_handler([] {}, [](int) {}, {}, &inner_memory);

          BOOST_TEST(wasm::memory_range.data() == expected.data());
          BOOST_TEST(wasm::memory_range.size() == expected.size());
       },
       [](int) {}, {}, &outer_memory);

   outer_memory.free();
   inner_memory.free();
}

TEST_CASE("null code scopes restore an enclosing explicit code span", "[signals]") {
   auto bytes = std::array<std::byte, 8>{};
   const auto previous = wasm::code_memory_range;
   auto calls = 0;

   wasm::invoke_with_signal_handler(
       [&] {
          const auto expected = wasm::code_memory_range;
          BOOST_TEST(expected.data() == bytes.data());
          BOOST_TEST(expected.size() == bytes.size());
          wasm::invoke_with_signal_handler(
              [&] { ++calls; BOOST_TEST(wasm::code_memory_range.empty()); },
              [](int) {}, nullptr, nullptr);
          BOOST_TEST(wasm::code_memory_range.data() == expected.data());
          BOOST_TEST(wasm::code_memory_range.size() == expected.size());
          BOOST_CHECK_THROW(wasm::invoke_with_signal_handler(
              [] { throw escaped_signal{}; }, [](int) {}, nullptr, nullptr), escaped_signal);
          BOOST_TEST(wasm::code_memory_range.data() == expected.data());
          BOOST_TEST(wasm::code_memory_range.size() == expected.size());
          wasm::invoke_with_signal_handler([&] { ++calls; }, [](int) {}, {}, nullptr);
       },
       [](int) {}, std::span<std::byte>{bytes}, nullptr);

   BOOST_TEST(calls == 2);
   BOOST_TEST(wasm::code_memory_range.data() == previous.data());
   BOOST_TEST(wasm::code_memory_range.size() == previous.size());
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

TEST_CASE("signal handlers are restored after an external replacement", "[signals]") {
   const auto guard = signal_action_guard{SIGFPE};

   struct sigaction external{};
   external.sa_handler = &external_signal_handler;
   sigemptyset(&external.sa_mask);
   external.sa_flags = 0;
   BOOST_REQUIRE(::sigaction(SIGFPE, &external, nullptr) == 0);
   external_signal_count = 0;

   const auto invoke = [] {
      wasm::invoke_with_signal_handler([] { std::raise(SIGFPE); }, [](int) { throw escaped_signal{}; }, {},
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
                [](int) {}, {}, nullptr);
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
             [](int) {}, {}, nullptr);
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

#if FORGE_VM_WASM_INTERPRET_HAS_JIT && !defined(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
TEST_CASE("jit re-exported host imports translate guest memory faults", "[signals]") {
   check_reexported_host_import<wasm::jit>();
}
#endif
