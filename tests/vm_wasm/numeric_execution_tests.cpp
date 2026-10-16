#include "test_prelude.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <forge/vm/wasm/interpret/opcode_macros.hpp>
#include <span>
#include <string_view>
#include <vector>

extern "C" {
#include <softfloat.h>
}

import forge.vm.wasm.interpret.backend;

namespace vm = forge::vm::wasm::interpret;

namespace {
struct operation_fixture {
   vm::opcodes operation;
   std::uint8_t wire;
   std::string_view name;
};

const auto operations = std::to_array<operation_fixture>({
#define FORGE_NUMERIC_FIXTURE(name, code) operation_fixture{vm::name, code, #name},
   FORGE_VM_WASM_INTERPRET_COMPARISON_OPS(FORGE_NUMERIC_FIXTURE)
   FORGE_VM_WASM_INTERPRET_NUMERIC_OPS(FORGE_NUMERIC_FIXTURE)
   FORGE_VM_WASM_INTERPRET_CONVERSION_OPS(FORGE_NUMERIC_FIXTURE)
#undef FORGE_NUMERIC_FIXTURE
});

vm::value_type named_type(std::string_view name) {
   if (name.starts_with("f32")) return vm::f32;
   if (name.starts_with("f64")) return vm::f64;
   if (name.starts_with("i32")) return vm::i32;
   return vm::i64;
}

bool fp_fixture(const operation_fixture& fixture) {
   return fixture.name.starts_with("f32") || fixture.name.starts_with("f64") ||
          fixture.name.ends_with("f32") || fixture.name.ends_with("f64");
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
   if (name.find("_convert_") != name.npos || name.find("_trunc_") != name.npos ||
       name.find("_demote_") != name.npos || name.find("_promote_") != name.npos ||
       name.find("_reinterpret_") != name.npos)
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
      if (result == vm::i32) output.f32 = value->to_ui32();
      else if (result == vm::i64) output.f64 = value->to_ui64();
      else if (result == vm::f32) output.f32 = value->to_fui32();
      else output.f64 = value->to_fui64();
   };
   const auto second = count == 2 ? arguments[1] : vm::expr_value{};
   if (input == vm::f32) call(std::bit_cast<float>(arguments[0].f32), std::bit_cast<float>(second.f32));
   else if (input == vm::f64) call(std::bit_cast<double>(arguments[0].f64), std::bit_cast<double>(second.f64));
   else if (input == vm::i32) call(arguments[0].i32, second.i32);
   else call(arguments[0].i64, second.i64);
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
} // namespace

BOOST_AUTO_TEST_SUITE(numeric_execution)

BOOST_AUTO_TEST_CASE(all_62_shared_fp_operations_match_actual_interpreter_semantics) {
   unsigned visited = 0;
   for (const auto& fixture : operations) {
      if (!fp_fixture(fixture))
         continue;
      BOOST_TEST_CONTEXT(fixture.name) {
         const auto type = input_type(fixture);
         const auto first = type == vm::f32 ? std::uint64_t{std::bit_cast<std::uint32_t>(13.25F)} :
                            type == vm::f64 ? std::bit_cast<std::uint64_t>(13.25) : std::uint64_t{13};
         const auto second = type == vm::f32 ? std::uint64_t{std::bit_cast<std::uint32_t>(3.5F)} :
                                             std::bit_cast<std::uint64_t>(3.5);
         const auto arguments = std::array{raw(first), raw(second)};
         const auto view = std::span{arguments}.first(argument_count(fixture));
         BOOST_TEST(bits(vm::execute_numeric(fixture.operation, view)) == bits(baseline(fixture, view)));
         ++visited;
      }
   }
   BOOST_TEST(visited == 62U);
}

BOOST_AUTO_TEST_CASE(raw_nan_signed_zero_comparisons_and_conversion_edges_preserve_existing_helpers) {
   for (const auto operation : {vm::f32_min, vm::f32_max, vm::f32_add, vm::f32_copysign,
                                vm::f64_min, vm::f64_max, vm::f64_add, vm::f64_copysign,
                                vm::f32_eq, vm::f64_ne}) {
      const auto& fixture = *std::find_if(operations.begin(), operations.end(), [&](const auto& value) {
         return value.operation == operation;
      });
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

BOOST_AUTO_TEST_CASE(arity_and_non_fp_domain_fail_with_distinct_existing_owner_categories) {
   const auto one = std::array{raw(0)};
   const auto two = std::array{raw(0), raw(0)};
   BOOST_CHECK_THROW(vm::execute_numeric(vm::f32_add, one), vm::exceptions::numeric_arity);
   BOOST_CHECK_THROW(vm::execute_numeric(vm::f64_sqrt, two), vm::exceptions::numeric_arity);
   BOOST_CHECK_THROW(vm::execute_numeric(vm::f32_neg, {}), vm::exceptions::numeric_arity);
   for (const auto operation : {vm::i32_add, vm::i64_extend_s_i32, vm::i32_wrap_i64,
                                vm::f32_const, vm::i32_load, vm::call, static_cast<vm::opcodes>(1000)})
      BOOST_CHECK_THROW(vm::execute_numeric(operation, two), vm::exceptions::unsupported_numeric);
}

BOOST_AUTO_TEST_CASE(shared_fp_guard_restores_caller_state_through_real_null_recovery_callback) {
   const auto original = fp_state{};
   const auto ambient = fp_state{softfloat_round_max, softfloat_tininess_beforeRounding,
                                 softfloat_flag_underflow, 32};
   ambient.apply();
   struct callback_state {
      std::array<vm::expr_value, 2> arguments{raw(0x3f800000), raw(0x33800000)};
      vm::expr_value result{};
      std::exception_ptr error;
      unsigned entered = 0;
   } state;
   const auto status = vm::invoke_without_signal_recovery(+[](void* data) noexcept {
      auto& state = *static_cast<callback_state*>(data);
      try {
         state.result = vm::execute_numeric(vm::f32_add, state.arguments);
         ++state.entered;
         BOOST_TEST((fp_state{} == fp_state{softfloat_round_max, softfloat_tininess_beforeRounding,
                                            softfloat_flag_underflow, 32}));
         const auto invalid = std::array{raw(0x7fc12345)};
         BOOST_CHECK_THROW(vm::execute_numeric(vm::i32_trunc_s_f32, invalid), vm::exceptions::interpreter);
      } catch (...) {
         state.error = std::current_exception();
      }
   }, &state);
   const auto restored = fp_state{};
   original.apply();
   BOOST_TEST(static_cast<unsigned>(status.stage) == static_cast<unsigned>(vm::signal_boundary_stage::none));
   BOOST_TEST(status.error == 0);
   BOOST_TEST(!state.error);
   BOOST_TEST(state.entered == 1U);
   BOOST_TEST(bits(state.result) == 0x3f800000U); // Ties to even, despite caller round_max.
   BOOST_TEST((restored == ambient));
}

BOOST_AUTO_TEST_SUITE_END()
