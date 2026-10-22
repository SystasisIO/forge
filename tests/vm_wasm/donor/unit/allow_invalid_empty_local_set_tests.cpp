#include "test_prelude.hpp"
import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.stack_elem;
import forge.vm.wasm.interpret.utils;
import forge.vm.wasm.interpret.backend;
#define FORGE_VM_WASM_INTERPRET_TEST_USES_BACKEND
#include "test_support.hpp"

#define FORGE_VM_WASM_INTERPRET_TEST_FILE allow_invalid_empty_local_set_tests

using namespace forge::vm::wasm::interpret;

extern wasm_allocator wa;

namespace {

/*
 * (module
 *  (func (local /illegal/))
 * )
 */
std::vector<uint8_t> bad_local_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60,
   0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x0a, 0x06, 0x01, 0x04, 0x01, 0x00,
   0x00, 0x0b
};

struct empty_options {};
struct static_options_false {
   static constexpr bool allow_invalid_empty_local_set = false;
};
struct static_options_true {
   static constexpr bool allow_invalid_empty_local_set = true;
};
struct dynamic_options {
   bool allow_invalid_empty_local_set;
};

}

BACKEND_TEST_CASE("Test allow_invalid_empty_local_set default", "[allow_invalid_empty_local_set_test]") {
   using backend_t = backend<std::nullptr_t, TestType>;
   BOOST_CHECK_THROW(backend_t(bad_local_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_invalid_empty_local_set empty", "[allow_invalid_empty_local_set_test]") {
   using backend_t = backend<std::nullptr_t, TestType, empty_options>;
   BOOST_CHECK_THROW(backend_t(bad_local_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_invalid_empty_local_set static fail", "[allow_invalid_empty_local_set_test]") {
   using backend_t = backend<std::nullptr_t, TestType, static_options_false>;
   BOOST_CHECK_THROW(backend_t(bad_local_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_invalid_empty_local_set static pass", "[allow_invalid_empty_local_set_test]") {
   using backend_t = backend<std::nullptr_t, TestType, static_options_true>;
   BOOST_CHECK_THROW(backend_t(bad_local_wasm, &wa), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_invalid_empty_local_set dynamic fail", "[allow_invalid_empty_local_set_test]") {
   using backend_t = backend<std::nullptr_t, TestType, dynamic_options>;
   BOOST_CHECK_THROW(backend_t(bad_local_wasm, nullptr, dynamic_options{false}), exceptions::parse);
}

BACKEND_TEST_CASE("Test allow_invalid_empty_local_set dynamic pass", "[allow_invalid_empty_local_set_test]") {
   using backend_t = backend<std::nullptr_t, TestType, dynamic_options>;
   BOOST_CHECK_THROW(backend_t(bad_local_wasm, nullptr, dynamic_options{true}), exceptions::parse);
}
