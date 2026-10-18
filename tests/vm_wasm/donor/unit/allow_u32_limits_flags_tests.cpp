#include "test_prelude.hpp"
import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.stack_elem;
import forge.vm.wasm.interpret.utils;
import forge.vm.wasm.interpret.backend;
#define FORGE_VM_WASM_INTERPRET_TEST_USES_BACKEND
#include "test_support.hpp"

#define FORGE_VM_WASM_INTERPRET_TEST_FILE allow_u32_limits_flags_tests

using namespace forge::vm::wasm::interpret;

extern wasm_allocator wa;

namespace {

// All of these wasms use 0x3FFE/0x3FFF instead of 0x00/0x01 for the flags

/*
 * (module
 *  (memory 1)
 * )
 */
std::vector<uint8_t> mem_no_max_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x04, 0x01, 0xFE,
   0x7F, 0x01
};

/*
 * (module
 *  (memory 1 2)
 * )
 */
std::vector<uint8_t> mem_max_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x05, 0x01, 0xFF,
   0x7F, 0x01, 0x02
};

/*
 * (module
 *  (table 1)
 * )
 */
std::vector<uint8_t> table_no_max_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x04, 0x05, 0x01, 0x70,
   0xFE, 0x7F, 0x01
};

/*
 * (module
 *  (table 1 2)
 * )
 */
std::vector<uint8_t> table_max_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x04, 0x06, 0x01, 0x70,
   0xFF, 0x7F, 0x01, 0x02
};

struct empty_options {};
struct static_options_false {
   static constexpr bool allow_u32_limits_flags = false;
};
struct static_options_true {
   static constexpr bool allow_u32_limits_flags = true;
};
struct dynamic_options {
   bool allow_u32_limits_flags;
};

}

BACKEND_TEST_CASE("Test allow_u32_limits_flags default", "[allow_u32_limits_flags_test]") {
   using backend_t = backend<std::nullptr_t, TestType>;
   BOOST_CHECK_THROW(backend_t(mem_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(mem_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_max_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_u32_limits_flags empty", "[allow_u32_limits_flags_test]") {
   using backend_t = backend<std::nullptr_t, TestType, empty_options>;
   BOOST_CHECK_THROW(backend_t(mem_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(mem_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_max_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_u32_limits_flags static fail", "[allow_u32_limits_flags_test]") {
   using backend_t = backend<std::nullptr_t, TestType, static_options_false>;
   BOOST_CHECK_THROW(backend_t(mem_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(mem_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_max_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_u32_limits_flags static pass", "[allow_u32_limits_flags_test]") {
   using backend_t = backend<std::nullptr_t, TestType, static_options_true>;
   BOOST_CHECK_THROW(backend_t(mem_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(mem_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_no_max_wasm, &wa), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_max_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_u32_limits_flags dynamic fail", "[allow_u32_limits_flags_test]") {
   using backend_t = backend<std::nullptr_t, TestType, dynamic_options>;
   BOOST_CHECK_THROW(backend_t(mem_max_wasm, nullptr, dynamic_options{false}), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(mem_no_max_wasm, nullptr, dynamic_options{false}), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_max_wasm, nullptr, dynamic_options{false}), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_no_max_wasm, nullptr, dynamic_options{false}), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_u32_limits_flags dynamic pass", "[allow_u32_limits_flags_test]") {
   using backend_t = backend<std::nullptr_t, TestType, dynamic_options>;
   BOOST_CHECK_THROW(backend_t(mem_max_wasm, nullptr, dynamic_options{true}), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(mem_no_max_wasm, nullptr, dynamic_options{true}), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_max_wasm, nullptr, dynamic_options{true}), exceptions::parse);
   BOOST_CHECK_THROW(backend_t(table_no_max_wasm, nullptr, dynamic_options{true}), exceptions::parse);
}
