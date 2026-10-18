#include "test_prelude.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <forge/vm/wasm/interpret/opcode_macros.hpp>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

extern "C" {
#include <softfloat.h>
}

import forge.vm.wasm.interpret.backend;

namespace vm = forge::vm::wasm::interpret;

static_assert(
    std::is_same_v<decltype(&vm::execute_numeric), vm::expr_value (*)(vm::opcodes, std::span<const vm::expr_value>)>);

namespace {
struct operation_fixture {
   vm::opcodes operation;
   std::uint8_t wire;
   std::string_view name;
};

const auto operations = std::to_array<operation_fixture>({
#define FORGE_NUMERIC_FIXTURE(name, code) operation_fixture{vm::name, code, #name},
    FORGE_VM_WASM_INTERPRET_COMPARISON_OPS(FORGE_NUMERIC_FIXTURE) FORGE_VM_WASM_INTERPRET_NUMERIC_OPS(
        FORGE_NUMERIC_FIXTURE) FORGE_VM_WASM_INTERPRET_CONVERSION_OPS(FORGE_NUMERIC_FIXTURE)
#undef FORGE_NUMERIC_FIXTURE
});

vm::value_type named_type(std::string_view name) {
   if (name.starts_with("f32"))
      return vm::f32;
   if (name.starts_with("f64"))
      return vm::f64;
   if (name.starts_with("i32"))
      return vm::i32;
   return vm::i64;
}

bool fp_fixture(const operation_fixture& fixture) {
   return fixture.name.starts_with("f32") || fixture.name.starts_with("f64") || fixture.name.ends_with("f32") ||
          fixture.name.ends_with("f64");
}

vm::value_type input_type(const operation_fixture& fixture) {
   const auto name = fixture.name;
   const auto conversion = name.find("_convert_") != name.npos || name.find("_trunc_") != name.npos ||
                           name.find("_demote_") != name.npos || name.find("_promote_") != name.npos ||
                           name.find("_reinterpret_") != name.npos;
   return named_type(conversion ? name.substr(name.rfind('_') + 1) : name);
}

vm::value_type output_type(const operation_fixture& fixture) {
   for (const auto suffix : {"_eq", "_ne", "_lt", "_gt", "_le", "_ge"})
      if (fixture.name.ends_with(suffix))
         return vm::i32;
   return named_type(fixture.name);
}

unsigned argument_count(const operation_fixture& fixture) {
   const auto name = fixture.name;
   for (const auto suffix : {"_abs", "_neg", "_ceil", "_floor", "_trunc", "_nearest", "_sqrt"})
      if (name.ends_with(suffix))
         return 1;
   if (name.find("_convert_") != name.npos || name.find("_trunc_") != name.npos || name.find("_demote_") != name.npos ||
       name.find("_promote_") != name.npos || name.find("_reinterpret_") != name.npos)
      return 1;
   return 2;
}

std::uint64_t bits(const vm::expr_value& value) {
   return std::bit_cast<std::uint64_t>(value);
}

vm::expr_value raw(std::uint64_t bits) {
   return std::bit_cast<vm::expr_value>(bits);
}

vm::expr_value baseline(const operation_fixture& fixture, std::span<const vm::expr_value> arguments) {
   auto wasm = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0};
   const auto input = input_type(fixture);
   const auto result = output_type(fixture);
   const auto count = static_cast<std::uint8_t>(arguments.size());
   const auto section = [&](std::uint8_t id, const vm::wasm_code& content) {
      wasm.push_back(id);
      wasm.push_back(static_cast<std::uint8_t>(content.size()));
      wasm.insert(wasm.end(), content.begin(), content.end());
   };
   auto type = vm::wasm_code{1, 0x60, count};
   type.insert(type.end(), count, static_cast<std::uint8_t>(input));
   type.insert(type.end(), {1, static_cast<std::uint8_t>(result)});
   section(1, type);
   section(3, {1, 0});
   section(7, {1, 3, 'r', 'u', 'n', 0, 0});
   auto body = vm::wasm_code{0};
   for (std::uint8_t i = 0; i < count; ++i)
      body.insert(body.end(), {0x20, i});
   body.insert(body.end(), {fixture.wire, 0x0b});
   auto code = vm::wasm_code{1, static_cast<std::uint8_t>(body.size())};
   code.insert(code.end(), body.begin(), body.end());
   section(10, code);
   auto allocator = vm::wasm_allocator{};
   auto interpreter = vm::backend<std::nullptr_t, vm::interpreter>{wasm, &allocator};
   auto output = vm::expr_value{};
   const auto call = [&]<typename T>(T first, T second) {
      const auto value = count == 1 ? interpreter.call_with_return("env", "run", first)
                                    : interpreter.call_with_return("env", "run", first, second);
      if (result == vm::i32)
         output.f32 = value->to_ui32();
      else if (result == vm::i64)
         output.f64 = value->to_ui64();
      else if (result == vm::f32)
         output.f32 = value->to_fui32();
      else
         output.f64 = value->to_fui64();
   };
   const auto second = count == 2 ? arguments[1] : vm::expr_value{};
   if (input == vm::f32)
      call(std::bit_cast<float>(arguments[0].f32), std::bit_cast<float>(second.f32));
   else if (input == vm::f64)
      call(std::bit_cast<double>(arguments[0].f64), std::bit_cast<double>(second.f64));
   else if (input == vm::i32)
      call(arguments[0].i32, second.i32);
   else
      call(arguments[0].i64, second.i64);
   return output;
}

struct fp_state {
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
   friend bool operator==(const fp_state&, const fp_state&) = default;
};

struct restore_native_state {
   std::fenv_t hardware{};
   int error = std::fegetenv(&hardware);
   ~restore_native_state() {
      if (error == 0)
         std::fesetenv(&hardware);
   }
};

struct restore_fp_state : restore_native_state {
   fp_state soft;
   ~restore_fp_state() {
      soft.apply();
   }
};

struct heap_observation {
   std::uint64_t live = 0;
   std::uint64_t high_water = 0;
};

heap_observation observe_heap() {
#if defined(__APPLE__)
   malloc_statistics_t statistics{};
   malloc_zone_statistics(malloc_default_zone(), &statistics);
   return {statistics.size_in_use, statistics.max_size_in_use};
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 33)
   const auto statistics = mallinfo2();
   return {statistics.uordblks + statistics.hblkhd, 0};
#else
   return {};
#endif
#else
   return {};
#endif
}
} // namespace

BOOST_AUTO_TEST_SUITE(numeric_execution)

BOOST_AUTO_TEST_CASE(cold_then_64_warm_calls_report_focused_cost_and_restore_host_fenv) {
   const auto one = std::array{raw(0)};
   BOOST_CHECK_THROW(vm::execute_numeric(vm::i32_add, {}), vm::exceptions::unsupported_numeric);
   BOOST_CHECK_THROW(vm::execute_numeric(vm::f32_add, one), vm::exceptions::numeric_arity);
   const auto restore = restore_fp_state{};
   BOOST_REQUIRE_EQUAL(restore.error, 0);
   BOOST_REQUIRE_EQUAL(std::fesetenv(FE_DFL_ENV), 0);
   BOOST_REQUIRE_EQUAL(std::fesetround(FE_UPWARD), 0);
   BOOST_REQUIRE_EQUAL(std::feraiseexcept(FE_INVALID | FE_INEXACT), 0);
   const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
   const auto soft = fp_state{};
   const auto arguments = std::array{raw(0x3f800000), raw(0x33800000)};
   const auto before = observe_heap();
   const auto begin = std::chrono::steady_clock::now();
   const auto cold = vm::execute_numeric(vm::f32_add, arguments);
   const auto prepared = std::chrono::steady_clock::now();
   const auto after_cold = observe_heap();
   auto correct = bits(cold) == 0x3f800000U;
   for (unsigned attempt = 0; attempt != 64; ++attempt)
      correct &= bits(vm::execute_numeric(vm::f32_add, arguments)) == 0x3f800000U;
   const auto finish = std::chrono::steady_clock::now();
   const auto after_warm = observe_heap();
   const auto rounding_after = std::fegetround();
   const auto flags_after = std::fetestexcept(FE_ALL_EXCEPT);
   BOOST_TEST(correct);
   BOOST_TEST(rounding_after == FE_UPWARD);
   BOOST_TEST(flags_after == flags);
   BOOST_TEST((fp_state{} == soft));
   BOOST_TEST_MESSAGE("numeric_cost cold_ns="
                      << std::chrono::duration_cast<std::chrono::nanoseconds>(prepared - begin).count()
                      << " warm_64_ns="
                      << std::chrono::duration_cast<std::chrono::nanoseconds>(finish - prepared).count()
                      << " allocator_live_before=" << before.live << " allocator_live_after_cold=" << after_cold.live
                      << " allocator_live_after_warm=" << after_warm.live
                      << " allocator_high_water=" << after_warm.high_water);
   // Process allocator observations, not exact per-call allocation/peak or RSS.
   // Darwin: default zone; glibc >= 2.33: mallinfo2; other hosts: zero/unavailable.
}

BOOST_AUTO_TEST_CASE(all_62_shared_fp_operations_match_actual_interpreter_semantics) {
   unsigned visited = 0;
   for (const auto& fixture : operations) {
      if (!fp_fixture(fixture))
         continue;
      BOOST_TEST_CONTEXT(fixture.name) {
         const auto type = input_type(fixture);
         const auto first = type == vm::f32   ? std::uint64_t{std::bit_cast<std::uint32_t>(13.25F)}
                            : type == vm::f64 ? std::bit_cast<std::uint64_t>(13.25)
                                              : std::uint64_t{13};
         const auto second =
             type == vm::f32 ? std::uint64_t{std::bit_cast<std::uint32_t>(3.5F)} : std::bit_cast<std::uint64_t>(3.5);
         const auto arguments = std::array{raw(first), raw(second)};
         const auto view = std::span{arguments}.first(argument_count(fixture));
         BOOST_TEST(bits(vm::execute_numeric(fixture.operation, view)) == bits(baseline(fixture, view)));
         ++visited;
      }
   }
   BOOST_TEST(visited == 62U);
}

BOOST_AUTO_TEST_CASE(nan_signed_zero_comparisons_and_edges_follow_current_wamr_profile) {
   for (const auto operation : {vm::f32_min, vm::f32_max, vm::f32_add, vm::f32_copysign, vm::f64_min, vm::f64_max,
                                vm::f64_add, vm::f64_copysign, vm::f32_eq, vm::f64_ne}) {
      const auto& fixture = *std::find_if(operations.begin(), operations.end(),
                                          [&](const auto& value) { return value.operation == operation; });
      const auto wide = input_type(fixture) == vm::f64;
      for (const auto first : wide ? std::array<std::uint64_t, 3>{0, 0x8000000000000000, 0x7ff8000000012345}
                                   : std::array<std::uint64_t, 3>{0, 0x80000000, 0x7fc12345}) {
         const auto arguments = std::array{raw(first), raw(wide ? 0x8000000000000000 : 0x80000000)};
         BOOST_TEST(bits(vm::execute_numeric(operation, arguments)) == bits(baseline(fixture, arguments)));
      }
   }
   const auto nan32 = std::array{raw(0x7fa12345)};
   const auto nan64 = std::array{raw(0x7ff0000000012345)};
   BOOST_TEST(bits(vm::execute_numeric(vm::i32_reinterpret_f32, nan32)) == 0x7fa12345U);
   BOOST_TEST(bits(vm::execute_numeric(vm::i64_reinterpret_f64, nan64)) == 0x7ff0000000012345U);
   BOOST_CHECK_THROW(vm::execute_numeric(vm::i32_trunc_s_f32, nan32), vm::exceptions::interpreter);
   const auto overflow = std::array{raw(std::bit_cast<std::uint64_t>(2147483648.0))};
   BOOST_CHECK_THROW(vm::execute_numeric(vm::i32_trunc_s_f64, overflow), vm::exceptions::interpreter);
}

BOOST_AUTO_TEST_CASE(reinterpret_preserves_all_raw_bits_including_signaling_nan_payloads) {
   for (const auto value :
        std::array<std::uint64_t, 6>{0, 0x80000000, 0x7fa12345, 0xffc12345, 0xffffffff, 0xdeadbeef7fa12345}) {
      const auto arguments = std::array{raw(value)};
      BOOST_TEST(bits(vm::execute_numeric(vm::i32_reinterpret_f32, arguments)) == (value & 0xffffffffU));
      BOOST_TEST(bits(vm::execute_numeric(vm::f32_reinterpret_i32, arguments)) == (value & 0xffffffffU));
   }
   for (const auto value : std::array<std::uint64_t, 5>{0, 0x8000000000000000, 0x7ff0000000012345, 0xfff8000000012345,
                                                        0xffffffffffffffff}) {
      const auto arguments = std::array{raw(value)};
      BOOST_TEST(bits(vm::execute_numeric(vm::i64_reinterpret_f64, arguments)) == value);
      BOOST_TEST(bits(vm::execute_numeric(vm::f64_reinterpret_i64, arguments)) == value);
   }
}

BOOST_AUTO_TEST_CASE(partially_initialized_32_bit_slots_do_not_require_upper_union_bytes) {
   // Deliberately no value-initialization, bit_cast, aggregate copy or read of
   // the input slot as a whole: the existing public API accepts this form.
   vm::expr_value value;
   const auto argument = std::span<const vm::expr_value>{&value, 1};
   value.f32 = 0x7fa12345;
   BOOST_TEST(bits(vm::execute_numeric(vm::i32_reinterpret_f32, argument)) == 0x7fa12345U);
   value.f32 = 0x80000000;
   BOOST_TEST(bits(vm::execute_numeric(vm::i32_reinterpret_f32, argument)) == 0x80000000U);
   value.i32 = -13;
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_reinterpret_i32, argument)) == 0xfffffff3U);
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_convert_s_i32, argument)) == std::bit_cast<std::uint32_t>(-13.0F));
   BOOST_TEST(bits(vm::execute_numeric(vm::f64_convert_s_i32, argument)) == std::bit_cast<std::uint64_t>(-13.0));
   value.f32 = 0x7fa12345;
   BOOST_CHECK_EXCEPTION(vm::execute_numeric(vm::i32_trunc_s_f32, argument), vm::exceptions::guest_trap,
                         [](const auto& error) {
                            return error.kind == vm::exceptions::guest_trap_kind::invalid_conversion &&
                                   error.code() == vm::exceptions::make_error_code(vm::exceptions::code::interpreter);
                         });
   std::array<vm::expr_value, 2> pair;
   pair[0].f32 = 0x3f800000;
   pair[1].f32 = 0x33800000;
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_add, pair)) == 0x3f800000U);
}

BOOST_AUTO_TEST_CASE(arithmetic_nan_is_canonical_but_bitwise_sign_operations_preserve_payload) {
   for (const auto operation : {vm::f32_add, vm::f32_sub, vm::f32_mul, vm::f32_div, vm::f32_min, vm::f32_max})
      for (const auto nan : {0x7fa12345U, 0xffc12345U}) {
         const auto arguments = std::array{raw(nan), raw(0x3f800000)};
         BOOST_TEST(bits(vm::execute_numeric(operation, arguments)) == 0x7fc00000U);
      }
   for (const auto operation : {vm::f64_add, vm::f64_sub, vm::f64_mul, vm::f64_div, vm::f64_min, vm::f64_max})
      for (const auto nan : {0x7ff0000000012345ULL, 0xfff8000000012345ULL}) {
         const auto arguments = std::array{raw(nan), raw(0x3ff0000000000000)};
         BOOST_TEST(bits(vm::execute_numeric(operation, arguments)) == 0x7ff8000000000000ULL);
      }
   for (const auto operation : {vm::f32_ceil, vm::f32_floor, vm::f32_trunc, vm::f32_nearest, vm::f32_sqrt}) {
      const auto arguments = std::array{raw(0xffa12345)};
      BOOST_TEST(bits(vm::execute_numeric(operation, arguments)) == 0x7fc00000U);
   }
   for (const auto operation : {vm::f64_ceil, vm::f64_floor, vm::f64_trunc, vm::f64_nearest, vm::f64_sqrt}) {
      const auto arguments = std::array{raw(0xfff0000000012345)};
      BOOST_TEST(bits(vm::execute_numeric(operation, arguments)) == 0x7ff8000000000000ULL);
   }
   const auto nan32 = std::array{raw(0xffa12345)};
   const auto nan64 = std::array{raw(0xfff0000000012345)};
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_neg, nan32)) == 0x7fa12345U);
   BOOST_TEST(bits(vm::execute_numeric(vm::f64_neg, nan64)) == 0x7ff0000000012345ULL);
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_abs, nan32)) == 0x7fa12345U);
   BOOST_TEST(bits(vm::execute_numeric(vm::f64_abs, nan64)) == 0x7ff0000000012345ULL);
   BOOST_TEST(bits(vm::execute_numeric(vm::f64_promote_f32, nan32)) == 0x7ff8000000000000ULL);
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_demote_f64, nan64)) == 0x7fc00000U);
   const auto sign32 = std::array{raw(0x7fa12345), raw(0x80000000)};
   const auto sign64 = std::array{raw(0x7ff0000000012345), raw(0x8000000000000000)};
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_copysign, sign32)) == 0xffa12345U);
   BOOST_TEST(bits(vm::execute_numeric(vm::f64_copysign, sign64)) == 0xfff0000000012345ULL);
   for (const auto operation : {vm::f32_min, vm::f64_min, vm::f32_max, vm::f64_max}) {
      const auto wide = operation == vm::f64_min || operation == vm::f64_max;
      const auto negative = wide ? 0x8000000000000000ULL : 0x80000000ULL;
      const auto minimum = operation == vm::f32_min || operation == vm::f64_min;
      for (const auto arguments : {std::array{raw(0), raw(negative)}, std::array{raw(negative), raw(0)}})
         BOOST_TEST(bits(vm::execute_numeric(operation, arguments)) == (minimum ? negative : 0));
   }
}

BOOST_AUTO_TEST_CASE(all_trapping_conversions_have_exact_interpreter_code_and_recover_next_call) {
   for (const auto operation : {vm::i32_trunc_s_f32, vm::i32_trunc_u_f32, vm::i32_trunc_s_f64, vm::i32_trunc_u_f64,
                                vm::i64_trunc_s_f32, vm::i64_trunc_u_f32, vm::i64_trunc_s_f64, vm::i64_trunc_u_f64}) {
      const auto& fixture = *std::find_if(operations.begin(), operations.end(),
                                          [&](const auto& value) { return value.operation == operation; });
      const auto wide = input_type(fixture) == vm::f64;
      const auto invalid = std::array{raw(wide ? 0x7ff0000000012345 : 0x7fa12345)};
      const auto overflow = std::array{raw(wide ? 0x7ff0000000000000 : 0x7f800000)};
      const auto exact = [&](vm::exceptions::guest_trap_kind expected) {
         return [expected](const vm::exceptions::guest_trap& error) {
            return error.kind == expected &&
                   error.code() == vm::exceptions::make_error_code(vm::exceptions::code::interpreter);
         };
      };
      BOOST_CHECK_EXCEPTION(vm::execute_numeric(operation, invalid), vm::exceptions::guest_trap,
                            exact(vm::exceptions::guest_trap_kind::invalid_conversion));
      BOOST_CHECK_EXCEPTION(vm::execute_numeric(operation, overflow), vm::exceptions::guest_trap,
                            exact(vm::exceptions::guest_trap_kind::integer_overflow));
      const auto valid =
          std::array{raw(wide ? std::bit_cast<std::uint64_t>(13.0) : std::bit_cast<std::uint32_t>(13.0F))};
      BOOST_TEST(bits(vm::execute_numeric(operation, valid)) == 13U);
   }
}

BOOST_AUTO_TEST_CASE(arity_and_non_fp_domain_fail_with_distinct_existing_owner_categories) {
   const auto one = std::array{raw(0)};
   const auto two = std::array{raw(0), raw(0)};
   BOOST_CHECK_THROW(vm::execute_numeric(vm::f32_add, one), vm::exceptions::numeric_arity);
   BOOST_CHECK_THROW(vm::execute_numeric(vm::f64_sqrt, two), vm::exceptions::numeric_arity);
   BOOST_CHECK_THROW(vm::execute_numeric(vm::f32_neg, {}), vm::exceptions::numeric_arity);
   for (const auto operation : {vm::i32_add, vm::i64_extend_s_i32, vm::i32_wrap_i64, vm::f32_const, vm::i32_load,
                                vm::call, static_cast<vm::opcodes>(1000)})
      BOOST_CHECK_THROW(vm::execute_numeric(operation, two), vm::exceptions::unsupported_numeric);
   BOOST_CHECK_THROW(vm::execute_numeric(vm::i32_add, {}), vm::exceptions::unsupported_numeric);
   for (const auto& fixture : operations) {
      for (const auto count : {0U, 1U, 2U}) {
         const auto arguments = std::span{two}.first(count);
         if (fp_fixture(fixture)) {
            if (count != argument_count(fixture))
               BOOST_CHECK_EXCEPTION(vm::execute_numeric(fixture.operation, arguments), vm::exceptions::numeric_arity,
                                     [](const auto& error) {
                                        return error.code() ==
                                               vm::exceptions::make_error_code(vm::exceptions::code::numeric_arity);
                                     });
         } else {
            BOOST_CHECK_EXCEPTION(vm::execute_numeric(fixture.operation, arguments),
                                  vm::exceptions::unsupported_numeric, [](const auto& error) {
                                     return error.code() ==
                                            vm::exceptions::make_error_code(vm::exceptions::code::unsupported_numeric);
                                  });
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(raw_numeric_restores_softfloat_and_native_fenv_on_success_and_trap) {
   const auto restore = restore_fp_state{};
   BOOST_REQUIRE_EQUAL(restore.error, 0);
   const auto ambient = fp_state{softfloat_round_max, softfloat_tininess_beforeRounding, softfloat_flag_underflow, 32};
   ambient.apply();
   BOOST_REQUIRE_EQUAL(std::fesetenv(FE_DFL_ENV), 0);
   BOOST_REQUIRE_EQUAL(std::fesetround(FE_UPWARD), 0);
   BOOST_REQUIRE_EQUAL(std::feraiseexcept(FE_INVALID | FE_INEXACT), 0);
   const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
   const auto arguments = std::array{raw(0x3f800000), raw(0x33800000)};
   const auto result = vm::execute_numeric(vm::f32_add, arguments);
   const auto round_after = std::fegetround();
   const auto flags_after = std::fetestexcept(FE_ALL_EXCEPT);
   BOOST_TEST(bits(result) == 0x3f800000U);
   BOOST_TEST((fp_state{} == ambient));
   BOOST_TEST(round_after == FE_UPWARD);
   BOOST_TEST(flags_after == flags);
   const auto invalid = std::array{raw(0x7fc12345)};
   BOOST_CHECK_THROW(vm::execute_numeric(vm::i32_trunc_s_f32, invalid), vm::exceptions::interpreter);
   const auto trap_round = std::fegetround();
   const auto trap_flags = std::fetestexcept(FE_ALL_EXCEPT);
   BOOST_TEST((fp_state{} == ambient));
   BOOST_TEST(trap_round == FE_UPWARD);
   BOOST_TEST(trap_flags == flags);
   BOOST_TEST(bits(vm::execute_numeric(vm::f32_add, arguments)) == 0x3f800000U);
}

BOOST_AUTO_TEST_CASE(raw_numeric_is_synchronous_and_preserves_nested_host_callback_state) {
   const auto restore = restore_fp_state{};
   BOOST_REQUIRE_EQUAL(restore.error, 0);
   const auto caller = std::this_thread::get_id();
   auto called = 0U;
   auto same_thread = false;
   auto preserved = false;
   auto recovered = false;
   auto unwound = 0U;
   const auto ambient = fp_state{softfloat_round_max, softfloat_tininess_beforeRounding, softfloat_flag_invalid, 64};
   auto bindings = std::vector<vm::host_binding>{
       {"env", "numeric", {{vm::f32, vm::f32}, {vm::f32}}, [&](void*, vm::execution_interface interface) {
           const auto host_restore = restore_fp_state{};
           struct exit_observer {
              unsigned& count;
              ~exit_observer() {
                 ++count;
              }
           } on_exit{unwound};
           ++called;
           same_thread = std::this_thread::get_id() == caller;
           const auto second = interface.pop_operand().to_fui32();
           const auto first = interface.pop_operand().to_fui32();
           ambient.apply();
           if (std::fesetround(FE_UPWARD) != 0)
              throw vm::exceptions::interpreter{"test host rounding setup failed"};
           const auto arguments = std::array{raw(first), raw(second)};
           const auto invalid = std::array{raw(0x7fa12345)};
           try {
              static_cast<void>(vm::execute_numeric(vm::i32_trunc_s_f32, invalid));
           } catch (const vm::exceptions::guest_trap& error) {
              recovered = error.kind == vm::exceptions::guest_trap_kind::invalid_conversion &&
                          error.code() == vm::exceptions::make_error_code(vm::exceptions::code::interpreter);
           }
           const auto result = vm::execute_numeric(vm::f32_add, arguments);
           preserved = fp_state{} == ambient && std::fegetround() == FE_UPWARD;
           interface.push_operand(vm::f32_const_t{result.f32});
        }}};
   const auto code = vm::wasm_code{0,    0x61, 0x73, 0x6d, 1,    0,    0,    0,  1,    11, 2,   0x60, 2,    0x7d, 0x7d,
                                   1,    0x7d, 0x60, 0,    1,    0x7d, 2,    15, 1,    3,  'e', 'n',  'v',  7,    'n',
                                   'u',  'm',  'e',  'r',  'i',  'c',  0,    0,  3,    2,  1,   1,    7,    7,    1,
                                   3,    'r',  'u',  'n',  0,    1,    10,   22, 1,    20, 0,   0x43, 0,    0,    0x80,
                                   0x3f, 0x43, 0,    0,    0x80, 0x33, 0x10, 0,  0x43, 0,  0,   0x80, 0x3f, 0x92, 0x0b};
   auto program = vm::prepared_module{code, vm::options{}, std::move(bindings), vm::prepared_module::purpose::execution,
                                      vm::execution_mode::interpreter};
   auto context = vm::prepared_module::context{program, 2, vm::execution_mode::interpreter};
   context.initialize(nullptr);
   for (unsigned attempt = 0; attempt != 2; ++attempt) {
      const auto result = context.call(nullptr, "run", {});
      BOOST_REQUIRE(result.has_value());
      BOOST_TEST(result->to_fui32() == 0x40000000U);
      BOOST_TEST(same_thread);
      BOOST_TEST(preserved);
      BOOST_TEST(recovered);
      BOOST_TEST(unwound == attempt + 1);
   }
   BOOST_TEST(called == 2U);
}

BOOST_AUTO_TEST_CASE(concurrent_numeric_calls_keep_per_call_context_and_thread_fp_state) {
   const auto restore = restore_fp_state{};
   BOOST_REQUIRE_EQUAL(restore.error, 0);
   const auto ambient = fp_state{softfloat_round_max, softfloat_tininess_beforeRounding, softfloat_flag_underflow, 32};
   ambient.apply();
   auto errors = std::array<std::exception_ptr, 2>{};
   auto correct = std::array<bool, 2>{};
   const auto work = [&](unsigned index) {
      try {
         const auto restore = restore_native_state{};
         if (restore.error || std::fesetenv(FE_DFL_ENV) != 0)
            throw vm::exceptions::interpreter{"test FP snapshot failed"};
         const auto rounding = index == 0 ? FE_DOWNWARD : FE_UPWARD;
         if (std::fesetround(rounding) != 0)
            throw vm::exceptions::interpreter{"test FP rounding failed"};
         const auto arguments = std::array{raw(0x3f800000), raw(0x33800000)};
         const auto soft_before = fp_state{};
         const auto result = vm::execute_numeric(vm::f32_add, arguments);
         correct[index] = bits(result) == 0x3f800000U && fp_state{} == soft_before && std::fegetround() == rounding;
      } catch (...) {
         errors[index] = std::current_exception();
      }
   };
   {
      const auto first = std::jthread{work, 0U};
      const auto second = std::jthread{work, 1U};
   }
   for (unsigned index = 0; index != 2; ++index) {
      BOOST_TEST(!errors[index]);
      BOOST_TEST(correct[index]);
   }
}

BOOST_AUTO_TEST_SUITE_END()
