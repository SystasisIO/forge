#include "test_prelude.hpp"
#include "fresh_process.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <functional>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

extern "C" {
#include <softfloat.h>
}

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.floating_point_environment;

#define FORGE_VM_WASM_INTERPRET_TEST_USES_BACKEND
#include "test_support.hpp"

namespace vm = forge::vm::wasm::interpret;

static_assert(std::is_nothrow_constructible_v<vm::floating_point_environment>);
static_assert(std::is_nothrow_destructible_v<vm::floating_point_environment>);
static_assert(!std::is_copy_constructible_v<vm::floating_point_environment>);
static_assert(!std::is_move_constructible_v<vm::floating_point_environment>);

namespace {
struct state {
   std::uint_fast8_t rounding = softfloat_roundingMode;
   std::uint_fast8_t tininess = softfloat_detectTininess;
   std::uint_fast8_t flags = softfloat_exceptionFlags;
   std::uint_fast8_t precision = extF80_roundingPrecision;

   void apply() const {
      softfloat_roundingMode = rounding;
      softfloat_detectTininess = tininess;
      softfloat_exceptionFlags = flags;
      extF80_roundingPrecision = precision;
   }

   bool operator==(const state&) const = default;
};

constexpr auto canonical = state{softfloat_round_near_even, softfloat_tininess_afterRounding, 0, 80};
constexpr auto external = state{softfloat_round_min, softfloat_tininess_beforeRounding, softfloat_flag_infinite, 32};
constexpr auto changed = state{softfloat_round_max, softfloat_tininess_beforeRounding, softfloat_flag_invalid, 64};

struct restore_state {
   state saved;
   ~restore_state() {
      saved.apply();
   }
};

struct host_failure {
   const void* identity;
};

struct converted_float {
   std::uint32_t bits;
   bool fail = false;
};

struct converter_host {
   const void* failure_identity = nullptr;
   unsigned conversions = 0;
   bool entered_canonical = true;
   bool mutated_all_four = true;
   bool conversion_scope_destroyed = false;
};

struct mutating_converter : vm::type_converter<converter_host> {
   using type_converter::to_wasm;
   using type_converter::type_converter;

   float to_wasm(converted_float value) {
      auto& active = this->get_host();
      ++active.conversions;
      active.entered_canonical &= state{} == canonical;
      changed.apply();
      active.mutated_all_four &= state{} == changed;
      const auto cleanup = vm::scope_guard{[&] { active.conversion_scope_destroyed = true; }};
      if (value.fail)
         throw host_failure{active.failure_identity};
      return std::bit_cast<float>(value.bits);
   }
};

using converter_functions = vm::registered_host_functions<converter_host, vm::execution_interface, mutating_converter>;

// Only test-fixture byte construction: production parsing is the existing VM parser.
void leb(vm::wasm_code& bytes, std::uint32_t value) {
   do {
      auto next = static_cast<std::uint8_t>(value & 0x7f);
      value >>= 7;
      bytes.push_back(next | (value ? 0x80 : 0));
   } while (value);
}

void section(vm::wasm_code& code, std::uint8_t id, const vm::wasm_code& payload) {
   code.push_back(id);
   leb(code, payload.size());
   code.insert(code.end(), payload.begin(), payload.end());
}

vm::wasm_code fixture(bool start = false) {
   auto code = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0};
   section(code, 1, {4, 0x60, 0, 0, 0x60, 0, 1, 0x7d, 0x60, 2, 0x7d, 0x7d, 1, 0x7d, 0x60, 0, 1, 0x7f});
   section(code, 2, {1, 3, 'e', 'n', 'v', 6, 'm', 'u', 't', 'a', 't', 'e', 0, 0});
   section(code, 3, {8, 2, 1, 0, 0, 0, 0, 3, 3});
   section(code, 4, {1, 0x70, 0, 3});
   section(code, 5, {1, 0, 1});
   section(code, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
   auto exports = vm::wasm_code{9};
   const auto names = std::array{"mutate", "add", "outer", "trap", "outertrap", "start", "loop", "get", "memory"};
   for (std::uint32_t i = 0; i < names.size(); ++i) {
      const auto name = std::string_view{names[i]};
      leb(exports, name.size());
      exports.insert(exports.end(), name.begin(), name.end());
      exports.push_back(0);
      leb(exports, i);
   }
   section(code, 7, exports);
   if (start)
      section(code, 8, {5});
   section(code, 9, {1, 0, 0x41, 0, 0x0b, 3, 0, 1, 2});
   auto body = vm::wasm_code{8};
   const auto append = [&](vm::wasm_code instructions) {
      leb(body, instructions.size());
      body.insert(body.end(), instructions.begin(), instructions.end());
   };
   append({0, 0x20, 0, 0x20, 1, 0x92, 0x0b});
   append({0, 0x10, 0, 0x43, 0, 0, 0x80, 0x3f, 0x43, 0, 0, 0x80, 0x33, 0x92, 0x0b});
   append({0, 0, 0x0b});
   append({0, 0x10, 0, 0, 0x0b});
   append({0, 0x10, 0, 0x43, 0, 0, 0x80, 0x3f, 0x43, 0, 0, 0x80, 0x33, 0x92, 0xbc, 0x24, 0, 0x0b});
   append({0, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b});
   append({0, 0x23, 0, 0x0b});
   // Actual guest instruction at the first inaccessible byte, not an unsafe
   // C++ host dereference. The checkpoint only covers guest/trivial code.
   append({0, 0x41, 0x80, 0x80, 4, 0x28, 2, 0, 0x0b});
   section(code, 10, body);
   return code;
}

vm::wasm_code converter_fixture() {
   auto code = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0};
   section(code, 1, {1, 0x60, 2, 0x7d, 0x7d, 1, 0x7d});
   section(code, 3, {1, 0});
   section(code, 7, {1, 3, 'a', 'd', 'd', 0, 0});
   section(code, 10, {1, 7, 0, 0x20, 0, 0x20, 1, 0x92, 0x0b});
   return code;
}

template <typename Implementation> void check_root_converter(bool fail) {
   const auto restore = restore_state{};
   external.apply();
   auto token = 0;
   auto active = converter_host{&token};
   auto memory = vm::wasm_allocator{};
   auto code = converter_fixture();
   auto instance = vm::backend<converter_functions, Implementation>{code, active, &memory};
   const auto call = [&](bool should_fail) {
      return instance.call_with_return(active, "env", "add", converted_float{0x3f800000U, should_fail},
                                       std::bit_cast<float>(0x33800000U));
   };
   if (fail) {
      bool caught = false;
      try {
         static_cast<void>(call(true));
      } catch (const host_failure& error) {
         caught = error.identity == &token;
      }
      BOOST_TEST(caught);
      BOOST_TEST(active.conversion_scope_destroyed);
      BOOST_TEST((state{} == external));
      active.conversion_scope_destroyed = false;
   }
   // RNE halfway result also detects loss of post-conversion recanonicalization:
   // the converter deliberately selects upward rounding before guest entry.
   BOOST_TEST(call(false)->to_fui32() == 0x3f800000U);
   BOOST_TEST(active.conversions == (fail ? 2U : 1U));
   BOOST_TEST(active.entered_canonical);
   BOOST_TEST(active.mutated_all_four);
   BOOST_TEST(active.conversion_scope_destroyed);
   BOOST_TEST((state{} == external));
}

template <typename Implementation> struct host {
   std::function<void()> nested;
   bool entered_canonical = true;
   bool restored_nested_state = true;
   bool exception_scope_destroyed = false;
   unsigned calls = 0;
   const void* failure_identity = nullptr;
   volatile char* fault = nullptr;
   int foreign_signal = 0;

   void mutate() {
      ++calls;
      entered_canonical &= state{} == canonical;
      changed.apply();
      if (nested) {
         nested();
         restored_nested_state &= state{} == changed;
      }
      if (failure_identity) {
         const auto unwound = vm::scope_guard{[&] { exception_scope_destroyed = true; }};
         throw host_failure{failure_identity};
      }
      if (fault)
         static_cast<void>(*fault);
      if (foreign_signal)
         std::raise(foreign_signal);
   }
};

template <typename Implementation> using functions = vm::registered_host_functions<host<Implementation>>;
template <typename Implementation> using backend = vm::backend<functions<Implementation>, Implementation>;

template <typename Implementation> void register_host() {
   static const auto registered = [] {
      functions<Implementation>::template add<&host<Implementation>::mutate>("env", "mutate");
      return true;
   }();
   static_cast<void>(registered);
}

template <typename Implementation> void check_entrypoints() {
   const auto restore = restore_state{};
   register_host<Implementation>();
   external.apply();
   auto memory = vm::wasm_allocator{};
   auto active = host<Implementation>{};
   auto code = fixture();
   auto instance = backend<Implementation>{code, active, &memory};
   auto nested_canonical = true;
   active.nested = [&] {
      const auto result = instance.call_with_return(active, "env", "add", std::bit_cast<float>(0x3f800000U),
                                                    std::bit_cast<float>(0x33800000U));
      nested_canonical &= result->to_fui32() == 0x3f800000U;
      try {
         instance.call(active, "env", "trap");
      } catch (const vm::exceptions::interpreter&) {
         nested_canonical &= state{} == changed;
      }
   };
   const auto result = instance.call_with_return(active, "env", "outer");
   BOOST_TEST(result->to_fui32() == 0x3f800000U);
   BOOST_TEST((state{} == external));
   BOOST_TEST(instance.call(&active, 2U));
   BOOST_TEST((state{} == external));
   BOOST_TEST(instance.call_indirect(&active, 2U));
   BOOST_TEST((state{} == external));
   BOOST_TEST(instance.call(active, "env", "mutate"));
   BOOST_TEST((state{} == external));
   BOOST_TEST(instance.call_indirect(&active, 0U));
   BOOST_TEST((state{} == external));
   BOOST_TEST(active.calls == 5U);
   BOOST_TEST(active.entered_canonical);
   BOOST_TEST(active.restored_nested_state);
   BOOST_TEST(nested_canonical);
}

template <typename Implementation> void check_exception_and_fault() {
   const auto restore = restore_state{};
   register_host<Implementation>();
   external.apply();
   auto memory = vm::wasm_allocator{};
   auto active = host<Implementation>{};
   auto code = fixture();
   auto instance = backend<Implementation>{code, active, &memory};
   auto token = 0;
   active.failure_identity = &token;
   const auto check_failure = [&](std::string_view name) {
      bool caught = false;
      try {
         instance.call(active, "env", name);
      } catch (const host_failure& failure) {
         caught = failure.identity == &token;
      }
      BOOST_TEST(caught);
      BOOST_TEST(active.exception_scope_destroyed);
      active.exception_scope_destroyed = false;
      BOOST_TEST((state{} == external));
   };
   check_failure("outer");
   check_failure("mutate");
   active.failure_identity = nullptr;
   BOOST_CHECK_THROW(instance.call(active, "env", "outertrap"), vm::exceptions::interpreter);
   BOOST_TEST((state{} == external));
   BOOST_CHECK_THROW(instance.call(active, "env", "trap"), vm::exceptions::interpreter);
   BOOST_TEST((state{} == external));
   BOOST_CHECK_THROW(instance.call(active, "env", "memory"), vm::exceptions::memory);
   BOOST_TEST((state{} == external));
   BOOST_TEST(instance.call_with_return(active, "env", "outer")->to_fui32() == 0x3f800000U);
   BOOST_TEST((state{} == external));
}

template <typename Implementation> void check_nested_host_exception() {
   const auto restore = restore_state{};
   register_host<Implementation>();
   external.apply();
   auto memory = vm::wasm_allocator{};
   auto active = host<Implementation>{};
   auto code = fixture();
   auto instance = backend<Implementation>{code, active, &memory};
   auto token = 0;
   bool nesting = false;
   bool caught_nested = false;
   active.nested = [&] {
      if (nesting)
         return;
      nesting = true;
      active.failure_identity = &token;
      try {
         instance.call(active, "env", "mutate");
      } catch (const host_failure& error) {
         caught_nested = error.identity == &token && active.exception_scope_destroyed;
      }
      active.failure_identity = nullptr;
      nesting = false;
      BOOST_TEST((state{} == changed));
   };
   BOOST_TEST(instance.call_with_return(active, "env", "outer")->to_fui32() == 0x3f800000U);
   BOOST_TEST(caught_nested);
   BOOST_TEST(active.entered_canonical);
   BOOST_TEST(active.restored_nested_state);
   BOOST_TEST((state{} == external));
}

template <typename Implementation> void check_foreign_host_fault() {
   register_host<Implementation>();
   // Child-only fatal probes: never convert a raw host fault into a VM trap or
   // assert cleanup of C++ RAII across fatal signal delivery.
   const auto backend_name = Implementation::is_jit ? "jit" : "interpreter";
   if (const auto* child_mode = fresh_process::mode()) {
      const auto hardware_mode = std::string{"fp-hardware-"} + backend_name;
      const auto signal_mode = std::string{"fp-bus-"} + backend_name;
      if (hardware_mode != child_mode && signal_mode != child_mode)
         return; // Fresh wildcard filter can discover both enabled backend types.
      fresh_process::disable_core_dumps();
      std::signal(SIGSEGV, SIG_DFL);
      std::signal(SIGBUS, SIG_DFL);
      auto memory = vm::wasm_allocator{};
      auto active = host<Implementation>{};
      auto code = fixture();
      auto instance = backend<Implementation>{code, active, &memory};
      if (hardware_mode == child_mode)
         active.fault = memory.template get_base_ptr<char>() + vm::page_size;
      else
         active.foreign_signal = SIGBUS;
      try {
         instance.call(active, "env", "outer");
      } catch (...) {
         std::_Exit(98); // A typed catch is an incorrect ownership claim.
      }
      std::_Exit(99);
   }
   for (const auto hardware : {true, false}) {
      const auto mode = std::string{hardware ? "fp-hardware-" : "fp-bus-"} + backend_name;
      const auto child =
          fresh_process::run("floating_point_environment/foreign_host_fault_is_fatal_not_a_typed_guest_trap*", mode);
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFSIGNALED(child.status));
      const auto signal = WTERMSIG(child.status);
      if (hardware)
         BOOST_TEST((signal == SIGSEGV || signal == SIGBUS));
      else
         BOOST_TEST(signal == SIGBUS);
   }
}

template <typename Implementation> void check_start_and_timeout() {
   const auto restore = restore_state{};
   register_host<Implementation>();
   external.apply();
   auto memory = vm::wasm_allocator{};
   auto active = host<Implementation>{};
   auto code = fixture(true);
   auto instance = backend<Implementation>{code, active, &memory};
   BOOST_TEST(active.calls == 1U);
   BOOST_TEST(active.entered_canonical);
   BOOST_TEST(instance.call_with_return(active, "env", "get")->to_ui32() == 0x3f800000U);
   BOOST_TEST((state{} == external));
   BOOST_CHECK_THROW(
       instance.timed_run(vm::watchdog{std::chrono::milliseconds{10}}, [&] { instance.call(active, "env", "loop"); }),
       vm::exceptions::timeout);
   BOOST_TEST((state{} == external));
   auto caught_nested_timeout = false;
   active.nested = [&] {
      try {
         instance.timed_run(vm::watchdog{std::chrono::milliseconds{10}}, [&] { instance.call(active, "env", "loop"); });
      } catch (const vm::exceptions::timeout&) {
         caught_nested_timeout = state{} == changed;
      }
   };
   BOOST_TEST(instance.call_with_return(active, "env", "outer")->to_fui32() == 0x3f800000U);
   BOOST_TEST(caught_nested_timeout);
   BOOST_TEST((state{} == external));
}

template <typename Implementation> void check_host_timeout_cleanup() {
   const auto restore = restore_state{};
   register_host<Implementation>();
   external.apply();
   auto memory = vm::wasm_allocator{};
   auto active = host<Implementation>{};
   auto code = fixture();
   auto instance = backend<Implementation>{code, active, &memory};
   bool cleaned = false;
   active.nested = [&] {
      const auto guard = vm::scope_guard{[&] { cleaned = true; }};
      // The real watchdog expires while user C++/FP scopes are live. Its
      // protected-code interruption must only trap upon safe guest reentry.
      std::this_thread::sleep_for(std::chrono::milliseconds{30});
   };
   BOOST_CHECK_THROW(
       instance.timed_run(vm::watchdog{std::chrono::milliseconds{10}}, [&] { instance.call(active, "env", "outer"); }),
       vm::exceptions::timeout);
   BOOST_TEST(cleaned);
   BOOST_TEST((state{} == external));
   active.nested = {};
   BOOST_TEST(instance.call_with_return(active, "env", "outer")->to_fui32() == 0x3f800000U);
   BOOST_TEST((state{} == external));
}
} // namespace

BOOST_AUTO_TEST_SUITE(floating_point_environment)

BOOST_AUTO_TEST_CASE(nested_scope_restores_all_four_values_on_normal_and_exceptional_exit) {
   const auto restore = restore_state{};
   external.apply();
   {
      const auto outer = vm::floating_point_environment{};
      BOOST_TEST((state{} == canonical));
      changed.apply();
      try {
         const auto inner = vm::floating_point_environment{};
         BOOST_TEST((state{} == canonical));
         static_cast<void>(f32_add(float32_t{0x3f800000}, float32_t{0x33800000}));
         throw host_failure{nullptr};
      } catch (const host_failure&) {
      }
      BOOST_TEST((state{} == changed));
   }
   BOOST_TEST((state{} == external));
}

BOOST_AUTO_TEST_CASE_TEMPLATE(host_mutation_nested_vm_and_all_entrypoints, Implementation,
                              forge_vm_wasm_interpret_backend_types) {
   check_entrypoints<Implementation>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(root_mutating_argument_converter_restores_caller_and_recanonicalizes_guest,
                              Implementation, forge_vm_wasm_interpret_backend_types) {
   check_root_converter<Implementation>(false);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(root_throwing_argument_converter_unwinds_and_restores_exact_caller_state, Implementation,
                              forge_vm_wasm_interpret_backend_types) {
   check_root_converter<Implementation>(true);
}

BOOST_AUTO_TEST_CASE_TEMPLATE(exact_host_exception_guest_trap_and_guest_memory_fault_restore, Implementation,
                              forge_vm_wasm_interpret_backend_types) {
   check_exception_and_fault<Implementation>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(nested_host_typed_exception_is_caught_before_outer_transport, Implementation,
                              forge_vm_wasm_interpret_backend_types) {
   check_nested_host_exception<Implementation>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(foreign_host_fault_is_fatal_not_a_typed_guest_trap, Implementation,
                              forge_vm_wasm_interpret_backend_types) {
   check_foreign_host_fault<Implementation>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(start_and_watchdog_timeout_restore, Implementation,
                              forge_vm_wasm_interpret_backend_types) {
   check_start_and_timeout<Implementation>();
}

BOOST_AUTO_TEST_CASE_TEMPLATE(watchdog_expires_in_host_and_traps_only_after_cpp_cleanup, Implementation,
                              forge_vm_wasm_interpret_backend_types) {
   check_host_timeout_cleanup<Implementation>();
}

BOOST_AUTO_TEST_SUITE_END()
