#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <utility>

import forge.vm.wasm.interpret.backend;

#if FORGE_VM_WASM_INTERPRET_HAS_JIT && !defined(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)

namespace wasm = forge::vm::wasm::interpret;

BOOST_AUTO_TEST_CASE(jit_executes_integer_popcount) {
   auto code = wasm::wasm_code{
       0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
       1, 11, 2, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 1, 0x7e, 1, 0x7e,
       3, 3, 2, 0, 1,
       7, 23, 2,
       8, 'p', 'o', 'p', 'c', 'n', 't', '3', '2', 0, 0,
       8, 'p', 'o', 'p', 'c', 'n', 't', '6', '4', 0, 1,
       10, 13, 2,
       5, 0, 0x20, 0, 0x69, 0x0b, // i32.popcnt(local.get 0)
       5, 0, 0x20, 0, 0x7b, 0x0b, // i64.popcnt(local.get 0)
   };
   auto instance = wasm::backend<wasm::standalone_function_t, wasm::jit>{code, nullptr};
   BOOST_REQUIRE(!instance.get_module().native_function_stack_bytes().empty());

   const std::pair<std::uint32_t, std::uint32_t> i32_cases[] = {
       {0, 0}, {1, 1}, {UINT32_C(0x80000000), 1}, {UINT32_MAX, 32}, {UINT32_C(0xdeadbeef), 24},
   };
   for (const auto& [input, expected] : i32_cases) {
      const auto result = instance.call_with_return("env", "popcnt32", input);
      BOOST_REQUIRE(result.has_value());
      BOOST_TEST(result->to_ui32() == expected);
   }

   const std::pair<std::uint64_t, std::uint64_t> i64_cases[] = {
       {0, 0}, {1, 1}, {UINT64_C(0x8000000000000000), 1}, {UINT64_MAX, 64},
       {UINT64_C(0xdeadbeefdeadbeef), 48},
   };
   for (const auto& [input, expected] : i64_cases) {
      const auto result = instance.call_with_return("env", "popcnt64", input);
      BOOST_REQUIRE(result.has_value());
      BOOST_TEST(result->to_ui64() == expected);
   }
}

#endif
