#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.executor;

#include "operational_oracles.hxx"

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
   if (!condition)
      throw std::runtime_error(std::string(message));
}

struct environment {
   std::fenv_t original{};
   int flags = 0;
   environment() {
      require(std::fegetenv(&original) == 0, "Cannot save oracle caller fenv");
      require(std::fesetround(FE_UPWARD) == 0 && std::feclearexcept(FE_ALL_EXCEPT) == 0 &&
                  std::feraiseexcept(FE_INVALID | FE_DIVBYZERO) == 0,
              "Cannot stage oracle caller fenv");
      flags = std::fetestexcept(FE_ALL_EXCEPT);
   }
   ~environment() {
      std::fesetenv(&original);
   }
   void check() const {
      require(std::fegetround() == FE_UPWARD && std::fetestexcept(FE_ALL_EXCEPT) == flags,
              "Oracle call changed caller fenv");
   }
};

struct host {
   std::uint64_t checksum = 0;
   unsigned calls = 0;
   void printui(std::uint64_t value) {
      checksum = value;
      ++calls;
   }
};
using registered = vm::registered_host_functions<host>;

vm::operand_stack_elem slot(vm::value_type type, std::uint64_t bits) {
   switch (type) {
   case vm::i32:
      return vm::i32_const_t{static_cast<std::uint32_t>(bits)};
   case vm::i64:
      return vm::i64_const_t{bits};
   case vm::f32:
      return vm::f32_const_t{static_cast<std::uint32_t>(bits)};
   case vm::f64:
      return vm::f64_const_t{bits};
   default:
      throw std::runtime_error("Invalid oracle slot type");
   }
}

std::uint64_t bits(const vm::operand_stack_elem& value) {
   // Copy only the active width, including for a partially initialized32 slot.
   if (value.is_a<vm::i32_const_t>()) {
      auto result = std::uint32_t{};
      std::memcpy(&result, &value.get<vm::i32_const_t>().data, sizeof(result));
      return result;
   }
   if (value.is_a<vm::f32_const_t>()) {
      auto result = std::uint32_t{};
      std::memcpy(&result, &value.get<vm::f32_const_t>().data, sizeof(result));
      return result;
   }
   auto result = std::uint64_t{};
   if (value.is_a<vm::i64_const_t>())
      std::memcpy(&result, &value.get<vm::i64_const_t>().data, sizeof(result));
   else
      std::memcpy(&result, &value.get<vm::f64_const_t>().data, sizeof(result));
   return result;
}

void expect(const oc::invocation_result& result, std::uint64_t expected) {
   if (result.trap != oc::trap_reason::none || result.host_error || !result.value || bits(*result.value) != expected)
      throw std::runtime_error("Oracle expected=" + std::to_string(expected) +
                               " actual=" + (result.value ? std::to_string(bits(*result.value)) : "void") +
                               " trap=" + std::to_string(static_cast<unsigned>(result.trap)));
}

struct invocation {
   const oc::artifact& code;
   oc::executor& executor;
   const oc::executor::binding<registered>& binding;
   vm::wasm_allocator memory;
   host state;
   unsigned calls = 0;

   oc::invocation_result call(std::string_view field, std::span<const vm::operand_stack_elem> arguments) {
      const auto exported = std::ranges::find(code.exports(), field, &oc::compiled_export::name);
      require(exported != code.exports().end(), "Missing operational oracle export");
      state = {};
      const auto caller = environment{};
      ++calls;
      try {
         auto result = executor.invoke(binding, &state, exported->function_index, arguments, memory,
                                       std::chrono::steady_clock::now() + 30s);
         caller.check();
         if (result.trap == oc::trap_reason::none) {
            const auto& signature = code.image().types[code.functions()[exported->function_index].type_index];
            require(bool(result.value) == !signature.ret.empty(), "Oracle result arity changed");
            if (result.value) {
               const auto type = signature.ret.front();
               require((type == vm::i32 && result.value->is_a<vm::i32_const_t>()) ||
                           (type == vm::i64 && result.value->is_a<vm::i64_const_t>()) ||
                           (type == vm::f32 && result.value->is_a<vm::f32_const_t>()) ||
                           (type == vm::f64 && result.value->is_a<vm::f64_const_t>()),
                       "Oracle result type changed");
            }
         }
         std::cout << "oracle " << field << " call=" << calls << " trap=" << static_cast<unsigned>(result.trap) << '\n';
         return result;
      } catch (...) {
         caller.check();
         throw;
      }
   }
   oc::invocation_result call(std::string_view field, std::initializer_list<vm::operand_stack_elem> arguments) {
      return call(field, std::span{arguments.begin(), arguments.size()});
   }
   void trap(std::string_view field, std::span<const vm::operand_stack_elem> arguments, oc::trap_reason reason) {
      const auto result = call(field, arguments);
      require(result.trap == reason && !result.value && !result.host_error && state.calls == 0,
              "Oracle trap lost its exact kind or allowed a host effect/result");
      expect(call("global_initial", {}), 7); // Same prepared binding after every failure.
   }
};

// Independent bytecode execution, explicitly interpreter even in a JIT-on graph.
// It uses the same source/profile, but never the AOT code or executor result.
struct reference {
   vm::prepared_module::preparation allocation{oc::artifact::default_preparation_limits()};
   std::array<vm::host_binding, 1> imports{{{"env", "printui", {{vm::i64}, {}}, [](void*, vm::execution_interface) {
                                                throw std::runtime_error("Unexpected reference host effect");
                                             }}}};
   vm::prepared_module module;
   vm::wasm_allocator memory;
   vm::prepared_module::context context;

   explicit reference(const oc::artifact& code)
       : module(code.image().source, code.image().configuration.execution, std::span{imports},
                vm::prepared_module::purpose::execution, allocation, vm::execution_mode::interpreter),
         context(module, code.image().configuration.execution.max_call_depth, vm::execution_mode::interpreter) {
      allocation.seal();
      context.set_wasm_allocator(&memory);
      context.set_max_pages(code.image().configuration.execution.max_pages);
   }
   std::uint64_t call(std::string_view field, std::span<const vm::operand_stack_elem> arguments) {
      const auto caller = environment{};
      context.initialize(nullptr);
      const auto value = context.call(nullptr, field, arguments);
      caller.check();
      require(bool(value), "Reference returned no value");
      return bits(*value);
   }
};

void integers(invocation& run) {
   auto baseline = reference{run.code};
   unsigned families = 0;
   for (const bool wide : {false, true}) {
      const auto type = static_cast<vm::value_type>(wide ? vm::i64 : vm::i32);
      const auto sign = wide ? 0x8000000000000000ULL : 0x80000000ULL;
      const auto mask = wide ? UINT64_MAX : std::uint64_t{UINT32_MAX};
      const auto vectors = std::array<std::array<std::uint64_t, 2>, 7>{
          {{0, 1}, {1, 1}, {27, 3}, {sign, 3}, {mask, mask}, {1, wide ? 64U : 32U}, {sign - 1, 1}}};
      const auto check = [&](unsigned op, unsigned arity) {
         ++families;
         const auto field = "int_" + std::to_string(op);
         for (const auto& vector : vectors) {
            const auto arguments = std::array{slot(type, vector[0]), slot(type, vector[1])};
            const auto view = std::span{arguments}.first(arity);
            expect(run.call(field, view), baseline.call(field, view));
         }
      };
      check(wide ? 0x50 : 0x45, 1);
      for (unsigned op = wide ? 0x51 : 0x46; op <= (wide ? 0x5a : 0x4f); ++op)
         check(op, 2);
      for (unsigned op = wide ? 0x79 : 0x67; op <= (wide ? 0x8a : 0x78); ++op)
         check(op, op <= (wide ? 0x7b : 0x69) ? 1 : 2);
      for (unsigned op = wide ? 0x7f : 0x6d; op <= (wide ? 0x82 : 0x70); ++op) {
         const auto zero = std::array{slot(type, 1), slot(type, 0)};
         run.trap("int_" + std::to_string(op), zero, oc::trap_reason::integer_divide_by_zero);
      }
      const auto overflow = std::array{slot(type, sign), slot(type, mask)};
      run.trap(wide ? "int_127" : "int_109", overflow, oc::trap_reason::integer_overflow);
      expect(run.call(wide ? "int_129" : "int_111", overflow), 0); // signed remainder MIN/-1 is not overflow
      expect(run.call(wide ? "int_134" : "int_116", {slot(type, 1), slot(type, wide ? 64 : 32)}), 1);
      expect(run.call(wide ? "int_134" : "int_116", {slot(type, 1), slot(type, mask)}), sign);
      expect(run.call(wide ? "int_124" : "int_106", {slot(type, mask), slot(type, 1)}), 0);
      expect(run.call(wide ? "int_121" : "int_103", {slot(type, 0)}), wide ? 64 : 32);
      expect(run.call(wide ? "int_122" : "int_104", {slot(type, 0)}), wide ? 64 : 32);
      expect(run.call(wide ? "int_123" : "int_105", {slot(type, mask)}), wide ? 64 : 32);
      expect(run.call(wide ? "int_83" : "int_72", {slot(type, sign), slot(type, 0)}), 1);
      expect(run.call(wide ? "int_84" : "int_73", {slot(type, sign), slot(type, 0)}), 0);
      expect(run.call(wide ? "int_135" : "int_117", {slot(type, sign), slot(type, mask)}), mask);
      expect(run.call(wide ? "int_136" : "int_118", {slot(type, sign), slot(type, mask)}), 1);
      expect(run.call(wide ? "int_137" : "int_119", {slot(type, sign), slot(type, 1)}), 1);
      expect(run.call(wide ? "int_138" : "int_120", {slot(type, 1), slot(type, 1)}), sign);
   }
   for (const unsigned op : {0xa7, 0xac, 0xad}) {
      ++families;
      const auto type = static_cast<vm::value_type>(op == 0xa7 ? vm::i64 : vm::i32);
      for (const auto value : {0ULL, 1ULL, 0x80000000ULL, 0xffffffffffffffffULL}) {
         const auto arguments = std::array{slot(type, value)};
         const auto field = "int_" + std::to_string(op);
         expect(run.call(field, arguments), baseline.call(field, arguments));
      }
   }
   require(families == 61, "Integer family inventory changed");
   expect(run.call("int_172", {slot(vm::i32, 0x80000000)}), 0xffffffff80000000ULL);
   expect(run.call("int_173", {slot(vm::i32, 0x80000000)}), 0x80000000ULL);
   expect(run.call("int_167", {slot(vm::i64, 0x1234567887654321ULL)}), 0x87654321ULL);
}

void cfg(invocation& run) {
   auto baseline = reference{run.code};
   for (const auto left : {0U, 1U, 7U, UINT32_MAX}) {
      const auto arguments = std::array{slot(vm::i32, left), slot(vm::i32, 27)};
      const auto expected = std::array<std::uint32_t, 7>{
          left ? 27U : 9U, left ? 12U : 0xffffffe3U, left == 0 ? 13U : 12U, 0, 27, left ? left : 27U, 12};
      for (const unsigned i : {0, 1, 2, 4, 5, 6}) {
         const auto field = "cfg_" + std::to_string(i);
         expect(run.call(field, arguments), expected[i]);
         require(baseline.call(field, arguments) == expected[i], "CFG independent reference disagrees");
      }
   }
   expect(run.call("cfg_3", {slot(vm::i32, 10), slot(vm::i32, 0)}), 55);
   for (const std::uint32_t right : {0U, 27U, 0x80000000U, UINT32_MAX}) {
      for (unsigned left = 0; left < 18; ++left) {
         const auto index = std::min(left, 15U);
         const auto n = index + 3;
         const auto expected = std::array<std::uint32_t, 8>{
             right + n, right ^ n, right * n, right % n, right << (n & 31), std::rotr(right, n), right | n, right & n};
         const auto arguments = std::array{slot(vm::i32, left), slot(vm::i32, right)};
         expect(run.call("dense", arguments), expected[index % 8]);
         require(baseline.call("dense", arguments) == expected[index % 8], "Dense table reference disagrees");
      }
   }
}

void memory(invocation& run) {
   constexpr auto widths = std::array<unsigned, 14>{4, 8, 4, 8, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4};
   // Literal little-endian oracles for the archived data segment at offset3.
   constexpr auto loaded = std::array<std::uint64_t, 14>{
       0x2345ff80, 0xbadcfe812345ff80ULL, 0x2345ff80, 0xbadcfe812345ff80ULL, 0xffffff80, 0x80,       0xffffff80,
       0xff80,     0xffffffffffffff80ULL, 0x80,       0xffffffffffffff80ULL, 0xff80,     0x2345ff80, 0x2345ff80};
   for (unsigned i = 0; i < widths.size(); ++i) {
      const auto field = "load_" + std::to_string(i);
      expect(run.call(field, {slot(vm::i32, 0)}), loaded[i]);
      require(run.state.calls == 1 && run.state.checksum == 42, "Load effect missing");
      expect(run.call(field, {slot(vm::i32, 65536 - 3 - widths[i])}), 0);
      for (const auto address : {65536U - 2 - widths[i], UINT32_MAX}) {
         const auto arguments = std::array{slot(vm::i32, address)};
         run.trap(field, arguments, oc::trap_reason::memory_bounds);
      }
   }
   expect(run.call("load_12", {slot(vm::i32, 1)}), 0xffffffff812345ffULL);
   expect(run.call("load_13", {slot(vm::i32, 1)}), 0x812345ffULL);
   constexpr auto store_types =
       std::array<vm::value_type, 9>{vm::i32, vm::i64, vm::f32, vm::f64, vm::i32, vm::i32, vm::i64, vm::i64, vm::i64};
   constexpr auto store_widths = std::array<unsigned, 9>{4, 8, 4, 8, 1, 2, 1, 2, 4};
   constexpr auto value = 0xfedcba9876543210ULL;
   for (unsigned i = 0; i < store_widths.size(); ++i) {
      const auto width = store_widths[i];
      const auto mask = width == 8 ? UINT64_MAX : (std::uint64_t{1} << (width * 8)) - 1;
      const auto field = "store_" + std::to_string(i);
      expect(run.call(field, {slot(vm::i32, 0), slot(store_types[i], value)}), value & mask);
      require(run.state.calls == 1 && run.state.checksum == 42, "Store effect missing");
      const auto* bytes = run.memory.get_base_ptr<const std::uint8_t>();
      require(bytes && bytes[2] == 0x12 &&
                  bytes[3 + width] ==
                      std::array<std::uint8_t, 9>{0x80, 0xff, 0x45, 0x23, 0x81, 0xfe, 0xdc, 0xba, 0x98}[width],
              "Store overwrote an adjacent byte");
      for (unsigned j = 0; j < width; ++j)
         require(bytes[3 + j] == static_cast<std::uint8_t>(value >> (j * 8)), "Store byte order/width changed");
      expect(run.call(field, {slot(vm::i32, 65536 - 3 - width), slot(store_types[i], value)}), value & mask);
      const auto arguments = std::array{slot(vm::i32, 65536 - 2 - width), slot(store_types[i], value)};
      const auto failed = run.call(field, arguments);
      require(failed.trap == oc::trap_reason::memory_bounds && !failed.value && !failed.host_error &&
                  run.state.calls == 0,
              "OOB store returned a value or reached host effect");
      bytes = run.memory.get_base_ptr<const std::uint8_t>();
      for (unsigned j = 65536 - (width - 1); j < 65536; ++j)
         require(bytes[j] == 0, "OOB store partially modified valid memory");
      expect(run.call("global_initial", {}), 7);
      const auto wrapping = std::array{slot(vm::i32, UINT32_MAX), slot(store_types[i], value)};
      run.trap(field, wrapping, oc::trap_reason::memory_bounds);
   }
   for (const bool wide : {false, true}) {
      expect(run.call(wide ? "global64" : "global32", {slot(wide ? vm::i64 : vm::i32, UINT64_MAX)}),
             wide ? UINT64_MAX : std::uint64_t{UINT32_MAX});
      const auto payload = wide ? 0xfff0000000012345ULL : 0xffa12345ULL;
      expect(run.call(wide ? "float_global64" : "float_global32", {slot(wide ? vm::f64 : vm::f32, payload)}), payload);
      expect(run.call(wide ? "global_initial64" : "global_initial", {}), 7);
      expect(run.call(wide ? "float_initial64" : "float_initial32", {}), wide ? 0x7ff8000000012345ULL : 0x7fc12345ULL);
   }
   expect(run.call("grow", {slot(vm::i32, 0)}), 1);
   expect(run.call("grow", {slot(vm::i32, 1)}), 1);
   require(run.memory.get_current_page() == 2, "Successful growth did not publish2 pages");
   const auto* grown = run.memory.get_base_ptr<const std::uint8_t>();
   require(std::all_of(grown + 65536, grown + 131072, [](auto value) { return value == 0; }), "New page not zeroed");
   expect(run.call("grow_size", {slot(vm::i32, 2)}), 3);
   for (const auto pages : {3U, UINT32_MAX}) {
      expect(run.call("grow", {slot(vm::i32, pages)}), UINT32_MAX);
      require(run.memory.get_current_page() == 1, "Failed growth changed pages");
   }
   expect(run.call("grow_reload", {slot(vm::i32, 1)}), 0x2345ff80);
   expect(run.call("grow_reload", {slot(vm::i32, 0)}), 0x2345ff80);
   expect(run.call("grow_new_page", {slot(vm::i32, 1)}), 0);
   const auto not_grown = std::array{slot(vm::i32, 0)};
   run.trap("grow_new_page", not_grown, oc::trap_reason::memory_bounds);
   auto second = invocation{run.code, run.executor, run.binding};
   expect(second.call("load_0", {slot(vm::i32, 0)}), 0x2345ff80);
   expect(run.call("load_0", {slot(vm::i32, 0)}), 0x2345ff80);
   require(second.memory.get_base_ptr<char>() != run.memory.get_base_ptr<char>(),
           "Distinct public allocators unexpectedly shared a mapping");
}

struct numeric_case {
   unsigned opcode;
   vm::value_type input, output;
   unsigned arity;
};

std::vector<numeric_case> numerics() {
   auto cases = std::vector<numeric_case>{};
   for (const bool wide : {false, true}) {
      const auto type = static_cast<vm::value_type>(wide ? vm::f64 : vm::f32);
      for (unsigned op = wide ? 0x61 : 0x5b; op <= (wide ? 0x66 : 0x60); ++op)
         cases.push_back({op, type, vm::i32, 2});
      for (unsigned op = wide ? 0x99 : 0x8b; op <= (wide ? 0xa6 : 0x98); ++op)
         cases.push_back({op, type, type, op >= (wide ? 0xa0 : 0x92) ? 2U : 1U});
   }
   for (const auto& entry : std::to_array<numeric_case>(
            {{0xa8, vm::f32, vm::i32, 1}, {0xa9, vm::f32, vm::i32, 1}, {0xaa, vm::f64, vm::i32, 1},
             {0xab, vm::f64, vm::i32, 1}, {0xae, vm::f32, vm::i64, 1}, {0xaf, vm::f32, vm::i64, 1},
             {0xb0, vm::f64, vm::i64, 1}, {0xb1, vm::f64, vm::i64, 1}, {0xb2, vm::i32, vm::f32, 1},
             {0xb3, vm::i32, vm::f32, 1}, {0xb4, vm::i64, vm::f32, 1}, {0xb5, vm::i64, vm::f32, 1},
             {0xb7, vm::i32, vm::f64, 1}, {0xb8, vm::i32, vm::f64, 1}, {0xb9, vm::i64, vm::f64, 1},
             {0xba, vm::i64, vm::f64, 1}, {0xb6, vm::f64, vm::f32, 1}, {0xbb, vm::f32, vm::f64, 1},
             {0xbc, vm::f32, vm::i32, 1}, {0xbd, vm::f64, vm::i64, 1}, {0xbe, vm::i32, vm::f32, 1},
             {0xbf, vm::i64, vm::f64, 1}}))
      cases.push_back(entry);
   require(cases.size() == 62, "FP operation inventory changed");
   return cases;
}

void floating_point(invocation& run) {
   const auto cases = numerics();
   for (const auto& fixture : cases) {
      const auto first = fixture.input == vm::f32   ? std::uint64_t{0x41540000}
                         : fixture.input == vm::f64 ? 0x402a800000000000ULL
                                                    : 13ULL;
      const auto second = fixture.input == vm::f32 ? std::uint64_t{0x40600000} : 0x400c000000000000ULL;
      auto arguments = std::array{slot(fixture.input, first), slot(fixture.input, second)};
      auto raw = std::array<vm::expr_value, 2>{};
      const auto width = fixture.input == vm::i32 || fixture.input == vm::f32 ? 4U : 8U;
      std::memcpy(&raw[0], &first, width);
      std::memcpy(&raw[1], &second, width);
      const auto caller = environment{};
      const auto value =
          vm::execute_numeric(static_cast<vm::opcodes>(fixture.opcode), std::span{raw}.first(fixture.arity));
      caller.check();
      auto expected = std::uint64_t{};
      std::memcpy(&expected, &value, fixture.output == vm::i32 || fixture.output == vm::f32 ? 4 : 8);
      for (const auto prefix : {"fp_", "relay_"})
         expect(
             run.call(std::string(prefix) + std::to_string(fixture.opcode), std::span{arguments}.first(fixture.arity)),
             expected);
   }
   const auto literal = [&](unsigned op, std::uint64_t a, std::uint64_t b, std::uint64_t expected) {
      const auto found = std::ranges::find(cases, op, &numeric_case::opcode);
      require(found != cases.end(), "Missing literal FP operation");
      const auto args = std::array{slot(found->input, a), slot(found->input, b)};
      for (const auto prefix : {"fp_", "relay_"})
         expect(run.call(std::string(prefix) + std::to_string(op), std::span{args}.first(found->arity)), expected);
   };
   literal(0x92, 0x3f800000, 0x33800000, 0x3f800000); // nearest, not caller's upward rounding
   for (const bool wide : {false, true}) {
      const auto sign = wide ? 0x8000000000000000ULL : 0x80000000ULL;
      const auto nan = wide ? 0x7ff0000000012345ULL : 0x7fa12345ULL;
      const auto canonical = wide ? 0x7ff8000000000000ULL : 0x7fc00000ULL;
      literal(wide ? 0xbd : 0xbc, nan, 0, nan);
      literal(wide ? 0xbf : 0xbe, nan, 0, nan);
      literal(wide ? 0xbd : 0xbc, sign, 0, sign);
      literal(wide ? 0xa0 : 0x92, nan, 0, canonical);
      literal(wide ? 0xa4 : 0x96, sign, 0, sign);
      literal(wide ? 0xa5 : 0x97, sign, 0, 0);
      literal(wide ? 0xa6 : 0x98, nan, sign, nan | sign);
      literal(wide ? 0x61 : 0x5b, nan, nan, 0);
      expect(run.call(wide ? "zero64" : "zero32", {}), 0);
      for (const auto selector : {0U, 1U})
         expect(run.call(wide ? "float_branch64" : "float_branch32",
                         {slot(vm::i32, selector), slot(wide ? vm::f64 : vm::f32, nan)}),
                selector ? nan : sign);
   }
   for (const auto& fixture : cases) {
      if (fixture.opcode < 0xa8 || fixture.opcode > 0xb1)
         continue;
      const auto wide = fixture.input == vm::f64;
      const auto invalid = std::array{slot(fixture.input, wide ? 0x7ff8000000000000ULL : 0x7fc00000ULL)};
      const auto overflow = std::array{slot(fixture.input, wide ? 0x7ff0000000000000ULL : 0x7f800000ULL)};
      for (const auto prefix : {"fp_", "relay_"}) {
         run.trap(std::string(prefix) + std::to_string(fixture.opcode), invalid, oc::trap_reason::invalid_conversion);
         run.trap(std::string(prefix) + std::to_string(fixture.opcode), overflow, oc::trap_reason::integer_overflow);
         const auto valid = std::array{slot(fixture.input, wide ? 0x402a800000000000ULL : 0x41540000ULL)};
         expect(run.call(std::string(prefix) + std::to_string(fixture.opcode), valid), 13);
      }
   }
}

void benchmark(invocation& run, bool memory) {
   const auto field = memory ? "benchmark_memory" : "benchmark_integer";
   const auto golden = memory ? 11713105455123543422ULL : 15065454813286283065ULL;
   auto arguments =
       std::array{slot(vm::i64, 0x345c850000000000ULL), slot(vm::i64, 0x345c850000000000ULL), slot(vm::i64, 1)};
   const auto checksum = [&] {
      const auto result = run.call(field, arguments);
      require(result.trap == oc::trap_reason::none && !result.value && !result.host_error && run.state.calls == 1,
              "Benchmark did not make exactly one successful host effect");
      return run.state.checksum;
   };
   require(checksum() == golden, "Archived independent benchmark golden changed");
   bool rejected = false;
   try {
      require(run.state.checksum == (golden ^ 1), "Deliberately wrong checksum");
   } catch (const std::runtime_error&) {
      rejected = true;
   }
   require(rejected, "Wrong-checksum negative control did not reject");
   arguments[2] = slot(vm::i64, 0);
   require(checksum() != golden, "Changed runtime operand did not affect benchmark");
   arguments[2] = slot(vm::i64, 1);
   require(checksum() == golden, "Benchmark failed reuse after negative controls");
}
} // namespace

void operational_oracles::run(std::string_view mode, oc::artifact code) {
   registered::add<&host::printui>("env", "printui");
   require(code.image().configuration.execution.max_call_depth == 1024 &&
               code.image().configuration.execution.max_func_local_bytes == 32768,
           "Operational oracle changed shared execution limits");
   code.prepare();
   const auto binding = oc::executor::bind<registered>(code);
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto executor = oc::executor{scheduler, {64ULL * 1024 * 1024, 64 * 1024}};
   forge::asio::blocking::run(runtime, executor.start(std::chrono::steady_clock::now() + 5s));
   auto failure = std::exception_ptr{};
   bool prepared = false;
   try {
      executor.prepare_thread();
      prepared = true;
      auto run = invocation{code, executor, binding};
      if (mode == "oracle_integer")
         integers(run);
      else if (mode == "oracle_cfg")
         cfg(run);
      else if (mode == "oracle_memory")
         memory(run);
      else if (mode == "oracle_fp62")
         floating_point(run);
      else if (mode == "oracle_benchmark_integer" || mode == "oracle_benchmark_memory")
         benchmark(run, mode == "oracle_benchmark_memory");
      else
         throw std::runtime_error("Unknown operational oracle mode");
      expect(run.call("global_initial", {}), 7);
      std::cout << "PASS " << mode << " public executor calls=" << run.calls << '\n';
   } catch (...) {
      failure = std::current_exception();
   }
   if (prepared)
      executor.release_thread();
   forge::asio::blocking::run(runtime, executor.shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   if (failure)
      std::rethrow_exception(failure);
}
