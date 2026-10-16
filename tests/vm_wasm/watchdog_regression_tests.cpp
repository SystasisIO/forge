module;

#include "test_prelude.hpp"
#include "fresh_process.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <latch>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Owning test unit: inspect the private sole count without exposing a public
// accessor or adding runtime instrumentation for a test.
module forge.vm.wasm.interpret.backend;

import :signals;

#include "test_support.hpp"

#define FORGE_VM_WASM_INTERPRET_TEST_FILE watchdog_regression_tests

namespace {
class expires_on_guard_destruction {
 public:
   class guard {
    public:
      explicit guard(std::function<void()> callback) : _callback(std::move(callback)) {}
      ~guard() {
         _callback();
      }

    private:
      std::function<void()> _callback;
   };

   template <typename F> guard scoped_run(F&& callback) {
      return guard{std::forward<F>(callback)};
   }
};
} // namespace

TEST_CASE("expired watchdog runs callback before guard destruction", "[watchdog_expiration]") {
   auto expired = std::atomic_bool{false};
   auto timer = forge::vm::wasm::interpret::watchdog{std::chrono::milliseconds{0}};

   {
      auto guard = timer.scoped_run([&expired] { expired = true; });
   }

   BOOST_TEST(expired.load());
}

TEST_CASE("timed run reports expiry triggered during guard destruction", "[watchdog_expiration]") {
   namespace wasm = forge::vm::wasm::interpret;
   auto code = wasm::wasm_code{
       0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // header
       0x01, 0x04, 0x01, 0x60, 0x00, 0x00,             // one function type
       0x03, 0x02, 0x01, 0x00,                         // one function
       0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b              // empty function body
   };
   auto memory = wasm::wasm_allocator{};

   {
      auto instance = wasm::backend<std::nullptr_t, wasm::interpreter>{code, &memory};
      BOOST_CHECK_THROW(instance.timed_run(expires_on_guard_destruction{}, [] {}), wasm::exceptions::timeout);
   }

   memory.free();
}

namespace {
namespace wasm = forge::vm::wasm::interpret;
using steady_clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void append_leb(wasm::wasm_code& bytes, std::uint32_t value) {
   do {
      const auto low = static_cast<std::uint8_t>(value & 0x7fU);
      value >>= 7;
      bytes.push_back(static_cast<std::uint8_t>(low | (value ? 0x80U : 0U)));
   } while (value);
}

void append_section(wasm::wasm_code& code, std::uint8_t id, const wasm::wasm_code& payload) {
   code.push_back(id);
   append_leb(code, static_cast<std::uint32_t>(payload.size()));
   code.insert(code.end(), payload.begin(), payload.end());
}

// Genuine validated WASM: healthy()->42, loop()->bridge then infinite br 0,
// oob()->i32.load at the first byte after its one page, host()->bridge then 42,
// numeric()->i32.div_s by zero. No synthetic native signal or trap metadata.
wasm::wasm_code interruption_fixture() {
   auto code = wasm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0};
   append_section(code, 1, {2, 0x60, 0, 0, 0x60, 0, 1, 0x7f});
   append_section(code, 2, {1, 3, 'e', 'n', 'v', 6, 'b', 'r', 'i', 'd', 'g', 'e', 0, 0});
   append_section(code, 3, {5, 1, 0, 1, 1, 1});
   append_section(code, 5, {1, 0, 1});
   auto exports = wasm::wasm_code{5};
   auto index = std::uint8_t{1};
   for (const auto name : {std::string_view{"healthy"}, std::string_view{"loop"}, std::string_view{"oob"},
                           std::string_view{"host"}, std::string_view{"numeric"}}) {
      append_leb(exports, static_cast<std::uint32_t>(name.size()));
      exports.insert(exports.end(), name.begin(), name.end());
      exports.push_back(0);
      exports.push_back(index++);
   }
   append_section(code, 7, exports);
   auto bodies = wasm::wasm_code{5};
   const auto append = [&](const wasm::wasm_code& body) {
      append_leb(bodies, static_cast<std::uint32_t>(body.size()));
      bodies.insert(bodies.end(), body.begin(), body.end());
   };
   append({0, 0x41, 0x2a, 0x0b});
   append({0, 0x10, 0, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b});
   append({0, 0x41, 0x80, 0x80, 0x04, 0x28, 2, 0, 0x0b});
   append({0, 0x10, 0, 0x41, 0x2a, 0x0b});
   append({0, 0x41, 1, 0x41, 0, 0x6d, 0x0b});
   append_section(code, 10, bodies);
   return code;
}

struct interruption_host {
   std::function<void()> hook;
   std::function<void()> on_cleanup;
   std::uint32_t calls = 0;
   std::uint32_t cleaned = 0;

   void bridge() {
      ++calls;
      const auto cleanup = wasm::scope_guard{[&] {
         ++cleaned;
         if (on_cleanup)
            on_cleanup();
      }};
      if (hook)
         hook();
   }
};

using interruption_functions = wasm::registered_host_functions<interruption_host>;

wasm::wasm_code registered_interruption_fixture() {
   static const auto registered = [] {
      interruption_functions::add<&interruption_host::bridge>("env", "bridge");
      return true;
   }();
   static_cast<void>(registered);
   return interruption_fixture();
}

template <typename Implementation> struct interruption_owner {
   wasm::wasm_allocator memory;
   interruption_host host;
   wasm::backend<interruption_functions, Implementation> instance;

   interruption_owner() : instance(registered_interruption_fixture(), host, &memory) {}

   void healthy() {
      const auto result = instance.call_with_return(host, "env", "healthy");
      BOOST_REQUIRE(result.has_value());
      BOOST_TEST(result->to_ui32() == 42U);
   }
};

template <typename F> void each_backend(F&& function) {
   function.template operator()<wasm::interpreter>();
#if FORGE_VM_WASM_INTERPRET_HAS_JIT
   function.template operator()<wasm::jit>();
#endif
}

// The existing test-only fresh exec bounds even an unexpected guest-loop or
// callback-teardown hang. Every child disables cores and is solely reaped.
bool child_case(std::string_view name) {
   if (const auto* mode = fresh_process::mode()) {
      BOOST_REQUIRE(std::string_view{mode} == name);
      fresh_process::disable_core_dumps();
      return true;
   }
   const auto result = fresh_process::run("watchdog_external_interruption/" + std::string{name}, std::string{name});
   BOOST_REQUIRE_EQUAL(result.error, 0);
   BOOST_REQUIRE(!result.timed_out);
   BOOST_REQUIRE(WIFEXITED(result.status));
   BOOST_TEST(WEXITSTATUS(result.status) == 0);
   return false;
}

struct host_failure : std::runtime_error {
   std::uint32_t identity = 17;
   host_failure() : std::runtime_error("owning host failure") {}
};

class twice_on_guard_destruction {
 public:
   struct guard {
      std::function<void()> callback;
      ~guard() {
         callback();
         callback();
      }
   };
   template <typename F> guard scoped_run(F&& callback) {
      return {std::forward<F>(callback)};
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(watchdog_external_interruption)

BOOST_AUTO_TEST_CASE(duration_and_absolute_delivery_preserve_mutex_reentrancy_contract) {
   if (!child_case("duration_and_absolute_delivery_preserve_mutex_reentrancy_contract"))
      return;
   const auto requester = std::this_thread::get_id();
   for (const auto prestopped : {false, true}) {
      auto stop = std::stop_source{};
      if (prestopped)
         stop.request_stop();
      auto application_mutex = std::mutex{};
      auto calls = std::atomic_uint{0};
      auto inline_delivery = std::atomic<bool>{false};
      auto callback_finished = std::atomic<bool>{false};
      auto protected_value = 0U;
      auto observed_value = 0U;
      auto observed_reason = wasm::watchdog::interruption::none;
      auto finished_while_locked = false;
      auto owner_lock = std::unique_lock{application_mutex};
      {
         auto guard = wasm::watchdog{2s, stop.get_token()}.scoped_run([&](wasm::watchdog::interruption reason) {
            calls.fetch_add(1);
            if (std::this_thread::get_id() == requester) {
               // Reject inline delivery without deadlocking the test on the
               // broken implementation. Only the actual timer path locks.
               inline_delivery.store(true);
               return;
            }
            auto callback_lock = std::lock_guard{application_mutex};
            observed_reason = reason;
            observed_value = protected_value;
            callback_finished.store(true);
         });
         if (!prestopped)
            stop.request_stop(); // Must return while this SAME mutex is held.
         finished_while_locked = callback_finished.load();
         protected_value = 17U;
         owner_lock.unlock(); // Release before the guard joins its callback.
      }
      BOOST_TEST(calls.load() == 1U);
      BOOST_TEST(!inline_delivery.load());
      BOOST_TEST(!finished_while_locked);
      BOOST_TEST(callback_finished.load());
      BOOST_TEST(observed_value == 17U);
      BOOST_CHECK(observed_reason == wasm::watchdog::interruption::canceled);
   }
   auto stop = std::stop_source{};
   auto calls = std::atomic_uint{0};
   auto inline_delivery = std::atomic<bool>{false};
   auto observed_reason = std::atomic<wasm::watchdog::interruption>{wasm::watchdog::interruption::none};
   auto completed_before_stop_return = false;
   {
      auto guard = wasm::watchdog{steady_clock::now() + 2s, {stop.get_token(), {}}}.scoped_run(
          [&](wasm::watchdog::interruption reason) {
             calls.fetch_add(1);
             inline_delivery.store(std::this_thread::get_id() == requester);
             observed_reason.store(reason);
          });
      stop.request_stop();
      completed_before_stop_return = calls.load() == 1U && inline_delivery.load();
   }
   BOOST_TEST(calls.load() == 1U);
   BOOST_TEST(completed_before_stop_return);
   BOOST_CHECK(observed_reason.load() == wasm::watchdog::interruption::canceled);
}

BOOST_AUTO_TEST_CASE(canonical_exception_values_and_duration_stop_are_preserved) {
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::canceled) == 4010003U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::busy) == 4010004U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::shared_instance) == 4010005U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::unusable) == 4010006U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::invocation_state) == 4010007U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::interrupted) == 4010008U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::execution_unavailable) == 4000011U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::numeric_arity) == 4040000U);
   BOOST_TEST(static_cast<std::uint32_t>(wasm::exceptions::code::unsupported_numeric) == 4040001U);
   BOOST_TEST(static_cast<unsigned>(wasm::watchdog::interruption::deadline) == 0U);
   BOOST_TEST(static_cast<unsigned>(wasm::watchdog::interruption::canceled) == 1U);
   auto canceled = std::stop_source{};
   canceled.request_stop();
   auto owner = interruption_owner<wasm::interpreter>{};
   BOOST_CHECK_THROW(owner.instance.timed_run(wasm::watchdog{1s, canceled.get_token()}, [] {}),
                     wasm::exceptions::canceled);
   owner.healthy();
   auto expired = std::atomic<bool>{false};
   {
      auto guard = wasm::watchdog{steady_clock::duration::max()}.scoped_run([&] { expired.store(true); });
   }
   BOOST_TEST(!expired.load());
}

BOOST_AUTO_TEST_CASE(scoped_invocation_unwinds_private_ancestor_interruption_without_host_poison) {
   if (!child_case("scoped_invocation_unwinds_private_ancestor_interruption_without_host_poison"))
      return;
   each_backend([]<typename Implementation> {
      auto outer = interruption_owner<Implementation>{};
      auto code = registered_interruption_fixture();
      auto pointer = wasm::wasm_code_ptr{code.data(), code.size()};
      auto inner = wasm::backend<interruption_functions, Implementation>{
          pointer, code.size(), nullptr, wasm::default_options{}, false, false};
      auto inner_host = interruption_host{};
      auto external = std::stop_source{};
      auto entered = false;
      outer.host.hook = [&] {
         inner.with_invocation(&inner_host, {}, 1s, {}, [&] {
            entered = true;
            external.request_stop();
            // The fault belongs to A, not the active B invocation or its host.
            outer.instance.call(outer.host, "env", "healthy");
         });
      };
      BOOST_CHECK_THROW(outer.instance.timed_run(steady_clock::now() + 2s, {{}, external.get_token()},
                                                 [&] { outer.instance.call(outer.host, "env", "loop"); }),
                        wasm::exceptions::interrupted);
      BOOST_TEST(entered);
      BOOST_TEST(outer.host.cleaned == 1U);
      BOOST_TEST(wasm::timed_run_timeout_count.load() == 0U);
      outer.healthy();
      // Scope B is detached and remains reusable: ancestor transport is not a
      // guest error or host failure recorded against the inner instance.
      inner.with_invocation(&inner_host, {}, 1s, {}, [&] {
         const auto result = inner.call_with_return(inner_host, "env", "healthy");
         BOOST_REQUIRE(result.has_value());
         BOOST_TEST(result->to_ui32() == 42U);
      });
   });
}

BOOST_AUTO_TEST_CASE(preexisting_requests_never_run_work) {
   if (!child_case("preexisting_requests_never_run_work"))
      return;
   each_backend([]<typename Implementation> {
      auto owner = interruption_owner<Implementation>{};
      auto cancel = std::stop_source{};
      auto external = std::stop_source{};
      cancel.request_stop();
      external.request_stop();
      auto work_calls = 0;
      const auto work = [&] { ++work_calls; };
      BOOST_CHECK_THROW(
          owner.instance.timed_run(steady_clock::now() - 1ms, {cancel.get_token(), external.get_token()}, work),
          wasm::exceptions::canceled);
      owner.healthy();
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() - 1ms, {{}, external.get_token()}, work),
                        wasm::exceptions::timeout);
      owner.healthy();
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()}, work),
                        wasm::exceptions::interrupted);
      owner.healthy();
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {cancel.get_token(), {}}, work),
                        wasm::exceptions::canceled);
      BOOST_TEST(work_calls == 0);
      owner.healthy();
   });
}

BOOST_AUTO_TEST_CASE(genuine_loop_external_cancel_and_deadline) {
   if (!child_case("genuine_loop_external_cancel_and_deadline"))
      return;
   each_backend([]<typename Implementation> {
      auto owner = interruption_owner<Implementation>{};
      auto external = std::stop_source{};
      owner.host.hook = [&] { external.request_stop(); };
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()},
                                                 [&] { owner.instance.call(owner.host, "env", "loop"); }),
                        wasm::exceptions::interrupted);
      BOOST_TEST(owner.host.calls == 1U);
      BOOST_TEST(owner.host.cleaned == 1U);
      owner.healthy();
      auto cancel = std::stop_source{};
      owner.host.hook = [&] { cancel.request_stop(); };
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {cancel.get_token(), {}},
                                                 [&] { owner.instance.call(owner.host, "env", "loop"); }),
                        wasm::exceptions::canceled);
      BOOST_TEST(owner.host.cleaned == 2U);
      owner.healthy();
      owner.host.hook = {};
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 20ms, {},
                                                 [&] { owner.instance.call(owner.host, "env", "loop"); }),
                        wasm::exceptions::timeout);
      BOOST_TEST(owner.host.cleaned == 3U);
      owner.healthy();
   });
}

BOOST_AUTO_TEST_CASE(memory_host_numeric_and_ordinary_timeout_are_not_readiness) {
   if (!child_case("memory_host_numeric_and_ordinary_timeout_are_not_readiness"))
      return;
   each_backend([]<typename Implementation> {
      auto owner = interruption_owner<Implementation>{};
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {},
                                                 [&] { owner.instance.call(owner.host, "env", "oob"); }),
                        wasm::exceptions::memory);
      owner.healthy();
      // A real guest memory outcome retained before an external request remains
      // memory even when this very scope subsequently acquires a fired cause.
      auto external = std::stop_source{};
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()},
                                                 [&] {
                                                    try {
                                                       owner.instance.call(owner.host, "env", "oob");
                                                    } catch (const wasm::exceptions::memory&) {
                                                       external.request_stop();
                                                       throw;
                                                    }
                                                 }),
                        wasm::exceptions::memory);
      owner.healthy();
      external = std::stop_source{};
      owner.host.hook = [&] {
         external.request_stop();
         throw host_failure{};
      };
      BOOST_CHECK_EXCEPTION(owner.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()},
                                                     [&] { owner.instance.call(owner.host, "env", "host"); }),
                            host_failure, [](const host_failure& error) { return error.identity == 17U; });
      BOOST_TEST(owner.host.cleaned == 1U);
      owner.healthy();
      external = std::stop_source{};
      owner.host.hook = [&] {
         external.request_stop();
         throw wasm::exceptions::timeout{"ordinary host timeout"};
      };
      BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()},
                                                 [&] { owner.instance.call(owner.host, "env", "host"); }),
                        wasm::exceptions::timeout);
      BOOST_TEST(owner.host.cleaned == 2U);
      owner.healthy();
      owner.host.hook = {};
      const auto numeric = [&] { owner.instance.call(owner.host, "env", "numeric"); };
      // The legacy JIT maps its arithmetic SIGFPE to memory; the interpreter
      // throws interpreter. Preserve those existing categories, not a new trap.
      if constexpr (Implementation::is_jit)
         BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {}, numeric), wasm::exceptions::memory);
      else
         BOOST_CHECK_THROW(owner.instance.timed_run(steady_clock::now() + 1s, {}, numeric),
                           wasm::exceptions::interpreter);
      owner.healthy();
      external = std::stop_source{};
      const auto retained_numeric = [&] {
         try {
            numeric();
         } catch (...) {
            external.request_stop();
            throw;
         }
      };
      if constexpr (Implementation::is_jit)
         BOOST_CHECK_THROW(
             owner.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()}, retained_numeric),
             wasm::exceptions::memory);
      else
         BOOST_CHECK_THROW(
             owner.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()}, retained_numeric),
             wasm::exceptions::interpreter);
      owner.healthy();
   });
}

BOOST_AUTO_TEST_CASE(separate_module_same_contract_nested_reasons_and_cleanup) {
   if (!child_case("separate_module_same_contract_nested_reasons_and_cleanup"))
      return;
   each_backend([]<typename Implementation> {
      auto outer = interruption_owner<Implementation>{};
      auto inner = interruption_owner<Implementation>{}; // Exact same WASM, independent module/mapping.
      auto external = std::stop_source{};
      auto cancel = std::stop_source{};
      outer.host.hook = [&] {
         external.request_stop();
         inner.instance.timed_run(steady_clock::now() + 1s, {}, [&] { inner.instance.call(inner.host, "env", "oob"); });
      };
      BOOST_CHECK_THROW(outer.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()},
                                                 [&] { outer.instance.call(outer.host, "env", "host"); }),
                        wasm::exceptions::memory); // Never consume the ancestor readiness reason.
      outer.healthy();
      inner.healthy();
      external = std::stop_source{};
      inner.host.hook = [&] { cancel.request_stop(); };
      outer.host.hook = [&] {
         external.request_stop();
         inner.instance.timed_run(steady_clock::now() + 1s, {cancel.get_token(), {}},
                                  [&] { inner.instance.call(inner.host, "env", "loop"); });
      };
      BOOST_CHECK_THROW(outer.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()},
                                                 [&] { outer.instance.call(outer.host, "env", "host"); }),
                        wasm::exceptions::canceled); // Inner cause, not ancestor interrupted.
      BOOST_TEST(outer.host.cleaned == 2U);
      BOOST_TEST(inner.host.cleaned == 1U);
      outer.healthy();
      inner.healthy();
      auto nested_work = 0;
      outer.instance.timed_run(steady_clock::now() + 1s, {}, [&] {
         BOOST_CHECK_THROW(outer.instance.timed_run(steady_clock::now() + 1s, {}, [&] { ++nested_work; }),
                           wasm::exceptions::execution_unavailable);
         BOOST_CHECK_THROW(outer.instance.timed_run(wasm::null_watchdog{}, [&] { ++nested_work; }),
                           wasm::exceptions::execution_unavailable);
      });
      BOOST_TEST(nested_work == 0);
      // Both legacy scopes retain their former same-mapping behavior.
      outer.instance.timed_run(wasm::null_watchdog{},
                               [&] { outer.instance.timed_run(wasm::null_watchdog{}, [&] { ++nested_work; }); });
      BOOST_TEST(nested_work == 1);
      outer.healthy();
   });
}

BOOST_AUTO_TEST_CASE(one_shot_race_and_custom_legacy_duplicate_callback) {
   if (!child_case("one_shot_race_and_custom_legacy_duplicate_callback"))
      return;
   for (auto iteration = 0; iteration != 20; ++iteration) {
      auto external = std::stop_source{};
      auto cancel = std::stop_source{};
      auto calls = std::atomic_uint{0};
      auto cause = std::atomic<wasm::watchdog::interruption>{wasm::watchdog::interruption::none};
      const auto deadline = steady_clock::now() + (iteration % 2 ? 1ms : 1000ms);
      auto timer = wasm::watchdog{deadline, {cancel.get_token(), external.get_token()}};
      {
         auto guard = timer.scoped_run([&](wasm::watchdog::interruption reason) {
            cause.store(reason);
            calls.fetch_add(1);
         });
         auto start = std::latch{3};
         auto first = std::jthread{[&] {
            start.arrive_and_wait();
            external.request_stop();
         }};
         auto second = std::jthread{[&] {
            start.arrive_and_wait();
            cancel.request_stop();
         }};
         start.arrive_and_wait();
      }
      BOOST_TEST(calls.load() == 1U);
      BOOST_TEST((cause.load() == wasm::watchdog::interruption::canceled ||
                  cause.load() == wasm::watchdog::interruption::interrupted ||
                  cause.load() == wasm::watchdog::interruption::deadline));
   }
   each_backend([]<typename Implementation> {
      auto owner = interruption_owner<Implementation>{};
      BOOST_CHECK_THROW(owner.instance.timed_run(twice_on_guard_destruction{}, [] {}), wasm::exceptions::timeout);
      owner.healthy();
      // No stale timeout count/reason remains after a duplicate custom callback.
      owner.instance.timed_run(steady_clock::now() + 1s, {}, [&] { owner.healthy(); });
   });
}

BOOST_AUTO_TEST_CASE(legacy_deadline_callback_stays_on_timer_thread) {
   if (!child_case("legacy_deadline_callback_stays_on_timer_thread"))
      return;
   const auto caller = std::this_thread::get_id();
   for (auto iteration = 0; iteration != 20; ++iteration) {
      auto callback_thread = std::thread::id{};
      auto callbacks = 0U;
      {
         auto timer = wasm::watchdog{iteration % 2 ? 1ms : 0ms};
         auto guard = timer.scoped_run([&] {
            callback_thread = std::this_thread::get_id();
            ++callbacks;
         });
         if (iteration % 2)
            std::this_thread::sleep_for(2ms);
      } // Terminal join publishes callback writes before these assertions.
      BOOST_TEST(callbacks == 1U);
      BOOST_TEST(callback_thread != caller);
      BOOST_TEST(callback_thread != std::thread::id{});
   }
}

BOOST_AUTO_TEST_CASE(concurrent_outer_publication_and_inner_retirement_leave_no_stale_count) {
   if (!child_case("concurrent_outer_publication_and_inner_retirement_leave_no_stale_count"))
      return;
   each_backend([]<typename Implementation> {
      auto outer = interruption_owner<Implementation>{};
      auto inner = interruption_owner<Implementation>{};
      for (auto iteration = 0; iteration != 24; ++iteration) {
         auto external = std::stop_source{};
         auto cancel = std::stop_source{};
         auto inner_cleanup = std::binary_semaphore{0};
         auto reached_cleanup = std::atomic_bool{false};
         auto inner_canceled = false;
         auto inner_error = std::exception_ptr{};
         // Real inner host cleanup wakes the outer requester near guest reentry
         // and inner recovery/retirement. This is finite actual overlap/stress,
         // NOT a claim of deterministic reproduction of the old split-store gap.
         inner.host.hook = [&] { cancel.request_stop(); };
         inner.host.on_cleanup = [&] { inner_cleanup.release(); };
         auto requester = std::jthread{[&] {
            reached_cleanup.store(inner_cleanup.try_acquire_for(1s), std::memory_order_release);
            external.request_stop();
         }};
         outer.host.hook = [&] {
            try {
               inner.instance.timed_run(steady_clock::now() + 1s, {cancel.get_token(), {}},
                                        [&] { inner.instance.call(inner.host, "env", "loop"); });
            } catch (const wasm::exceptions::canceled&) {
               inner_canceled = true;
            } catch (...) {
               inner_error = std::current_exception();
            }
            requester.join(); // Requester no longer borrows sources/context on guest reentry.
            if (inner_error)
               std::rethrow_exception(inner_error);
         };
         auto outer_interrupted = false;
         auto outer_error = std::exception_ptr{};
         try {
            outer.instance.timed_run(steady_clock::now() + 1s, {{}, external.get_token()},
                                     [&] { outer.instance.call(outer.host, "env", "loop"); });
         } catch (const wasm::exceptions::interrupted&) {
            outer_interrupted = true;
         } catch (...) {
            outer_error = std::current_exception();
         }
         if (requester.joinable())
            requester.join(); // Also drain an unexpected pre-host failure before assertion/owner release.
         inner.host.on_cleanup = {};
         BOOST_TEST(outer_interrupted);
         BOOST_TEST(!outer_error);
         BOOST_TEST(reached_cleanup.load(std::memory_order_acquire));
         BOOST_TEST(inner_canceled);
         BOOST_TEST(wasm::timed_run_timeout_count.load(std::memory_order_acquire) == 0U);
         outer.healthy();
         inner.healthy();
      }
   });
}

BOOST_AUTO_TEST_CASE(teardown_waits_for_callback_and_late_request_is_inert) {
   if (!child_case("teardown_waits_for_callback_and_late_request_is_inert"))
      return;
   auto mutex = std::mutex{};
   auto condition = std::condition_variable{};
   auto external = std::stop_source{};
   auto guard_ready = false;
   auto callback_entered = false;
   auto leave_work = false;
   auto about_to_close = false;
   auto release_callback = false;
   auto guard_returned = false;
   auto owner = std::jthread{[&] {
      auto timer = wasm::watchdog{steady_clock::now() + 3s, {{}, external.get_token()}};
      {
         auto guard = timer.scoped_run([&](wasm::watchdog::interruption) {
            auto lock = std::unique_lock{mutex};
            callback_entered = true;
            condition.notify_all();
            condition.wait_for(lock, 2s, [&] { return release_callback; });
         });
         auto lock = std::unique_lock{mutex};
         guard_ready = true;
         condition.notify_all();
         condition.wait_for(lock, 2s, [&] { return leave_work; });
         about_to_close = true;
         condition.notify_all();
      }
      auto lock = std::lock_guard{mutex};
      guard_returned = true;
      condition.notify_all();
   }};
   auto requester = std::jthread{[&] {
      {
         auto lock = std::unique_lock{mutex};
         condition.wait_for(lock, 2s, [&] { return guard_ready; });
      }
      external.request_stop();
   }};
   auto entered = false;
   auto closing = false;
   auto returned_early = false;
   {
      auto lock = std::unique_lock{mutex};
      entered = condition.wait_for(lock, 1s, [&] { return callback_entered; });
      leave_work = true;
      condition.notify_all();
      closing = condition.wait_for(lock, 1s, [&] { return about_to_close; });
      returned_early = condition.wait_for(lock, 20ms, [&] { return guard_returned; });
      release_callback = true;
      condition.notify_all();
   }
   requester.join();
   owner.join(); // Terminal cleanup precedes every assertion, including failures.
   BOOST_TEST(entered);
   BOOST_TEST(closing);
   BOOST_TEST(!returned_early);
   BOOST_TEST(guard_returned);
   auto late = std::stop_source{};
   auto calls = std::atomic_uint{0};
   {
      auto timer = wasm::watchdog{steady_clock::now() + 1s, {{}, late.get_token()}};
      auto guard = timer.scoped_run([&] { calls.fetch_add(1); });
   }
   late.request_stop();
   BOOST_TEST(calls.load() == 0U);
   each_backend([&]<typename Implementation> {
      auto instance = interruption_owner<Implementation>{};
      auto source = std::stop_source{};
      instance.instance.timed_run(steady_clock::now() + 1s, {{}, source.get_token()}, [&] { instance.healthy(); });
      source.request_stop();
      instance.healthy();
      instance.instance.timed_run(steady_clock::now() + 1s, {}, [&] { instance.healthy(); });
   });
}

BOOST_AUTO_TEST_SUITE_END()
