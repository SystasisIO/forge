#include <boost/test/unit_test.hpp>

#include <cstdint>

import forge.vm.wasm.interpret.backend;

BOOST_AUTO_TEST_CASE(prepared_module_validation_checks_data_without_an_execution_context) {
   // One page of memory and one byte of active data placed just beyond it.
   auto code = forge::vm::wasm::interpret::wasm_code{
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
      0x05, 0x03, 0x01, 0x00, 0x01,
      0x0b, 0x09, 0x01, 0x00, 0x41, 0x80, 0x80, 0x04, 0x0b, 0x01, 0x58};
   auto pointer = forge::vm::wasm::interpret::wasm_code_ptr{code.data(), code.size()};
   auto parsed = forge::vm::wasm::interpret::backend<>{
      pointer, code.size(), nullptr, forge::vm::wasm::interpret::default_options{}, true, false};

   BOOST_CHECK_THROW(parsed.validate(), forge::vm::wasm::interpret::exceptions::memory);
}

BOOST_AUTO_TEST_CASE(prepared_module_validation_does_not_execute_start) {
   // The start function loops forever. Admission must not enter it.
   auto code = forge::vm::wasm::interpret::wasm_code{
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
      0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
      0x03, 0x02, 0x01, 0x00,
      0x08, 0x01, 0x00,
      0x0a, 0x09, 0x01, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b};
   auto pointer = forge::vm::wasm::interpret::wasm_code_ptr{code.data(), code.size()};
   auto parsed = forge::vm::wasm::interpret::backend<>{
      pointer, code.size(), nullptr, forge::vm::wasm::interpret::default_options{}, true, false};

   BOOST_CHECK_NO_THROW(parsed.validate());
   BOOST_CHECK_NO_THROW(parsed.validate());
}

BOOST_AUTO_TEST_CASE(prepared_module_validation_rejects_an_empty_backend) {
   auto empty = forge::vm::wasm::interpret::backend<>{};

   BOOST_CHECK_THROW(empty.validate(), forge::vm::wasm::interpret::exceptions::interpreter);
}
