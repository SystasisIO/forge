#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.host_function;

namespace {
namespace wasm = forge::vm::wasm::interpret;

struct validation_profile_host {
   static inline std::uint32_t calls = 0;

   std::uint32_t probe(std::uint32_t value) {
      ++calls;
      return value;
   }
};

struct empty_validation_profile_host {};

using validation_profile = wasm::registered_host_functions<validation_profile_host>;
using empty_validation_profile = wasm::registered_host_functions<empty_validation_profile_host>;

void register_validation_profile() {
   static const auto registered = [] {
      validation_profile::add<&validation_profile_host::probe>("env", "probe");
      return true;
   }();
   static_cast<void>(registered);
}

wasm::wasm_code imported_start(std::string_view module = "env", std::string_view name = "probe",
                               std::uint8_t parameter = 0x7f, std::uint8_t result = 0x7f) {
   auto code = wasm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 9, 2, 0x60, 1, parameter, 1, result, 0x60, 0, 0};
   auto imports = std::vector<std::uint8_t>{1, static_cast<std::uint8_t>(module.size())};
   imports.insert(imports.end(), module.begin(), module.end());
   imports.push_back(static_cast<std::uint8_t>(name.size()));
   imports.insert(imports.end(), name.begin(), name.end());
   imports.insert(imports.end(), {0, 0});
   code.insert(code.end(), {2, static_cast<std::uint8_t>(imports.size())});
   code.insert(code.end(), imports.begin(), imports.end());
   code.insert(code.end(), {3, 2, 1, 1, 8, 1, 1, 10, 9, 1, 7, 0,
                            static_cast<std::uint8_t>(parameter == 0x7e ? 0x42 : 0x41), 0, 0x10, 0, 0x1a, 0x0b});
   return code;
}

template <typename Implementation>
concept has_parser = requires {
   typename Implementation::template parser<std::nullptr_t, wasm::default_options, wasm::null_debug_info>;
};

template <typename Implementation>
concept has_context = requires { typename Implementation::template context<std::nullptr_t>; };

static_assert(!has_parser<wasm::interpreter>);
static_assert(!has_parser<wasm::null_backend>);
static_assert(!has_context<wasm::null_backend>);
static_assert(std::is_same_v<wasm::interpreter::context<std::nullptr_t>, wasm::runtime_context>);
static_assert(std::is_base_of_v<wasm::jit, wasm::jit_profile>);
static_assert(std::is_default_constructible_v<wasm::profile_instr_map>);

wasm::wasm_code memory_instruction(std::uint8_t opcode, std::span<const std::uint8_t> index, bool has_memory = true) {
   auto code = wasm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 5, 1, 0x60, 0, 1, 0x7f, 3, 2, 1, 0};
   if (has_memory)
      code.insert(code.end(), {5, 3, 1, 0, 1});
   code.insert(code.end(), {7, 7, 1, 3, 'r', 'u', 'n', 0, 0});
   auto body = std::vector<std::uint8_t>{0};
   if (opcode == 0x40)
      body.insert(body.end(), {0x41, 0});
   body.push_back(opcode);
   body.insert(body.end(), index.begin(), index.end());
   body.push_back(0x0b);
   code.insert(code.end(), {10, static_cast<std::uint8_t>(body.size() + 2), 1, static_cast<std::uint8_t>(body.size())});
   code.insert(code.end(), body.begin(), body.end());
   return code;
}

template <typename Implementation>
void check_memory_index_encoding(std::uint8_t opcode, std::span<const std::uint8_t> index) {
   auto code = memory_instruction(opcode, index);
   const auto original = code;
   auto memory = wasm::wasm_allocator{};
   auto runtime = wasm::backend<std::nullptr_t, Implementation>{code, &memory};
   const auto result = runtime.call_with_return("env", "run");
   BOOST_REQUIRE(result);
   BOOST_TEST(result->to_ui32() == 1);
   BOOST_CHECK(code == original);
}
} // namespace

BOOST_AUTO_TEST_CASE(validation_host_profile_bare_accepts_unlinked_function_imports_without_effects) {
   register_validation_profile();
   validation_profile_host::calls = 0;
   using validator = wasm::backend<std::nullptr_t, wasm::null_backend>;
   for (auto code : {imported_start(), imported_start("other", "missing", 0x7e, 0x7e)}) {
      const auto original = code;
      auto value = validator{code, nullptr};
      BOOST_CHECK_NO_THROW(value.validate());
      BOOST_CHECK(code == original);
   }
   BOOST_TEST(validation_profile_host::calls == 0U);
}

BOOST_AUTO_TEST_CASE(validation_host_profile_explicit_empty_rejects_function_imports) {
   using validator = wasm::backend<empty_validation_profile, wasm::null_backend>;
   auto code = imported_start();
   const auto original = code;
   BOOST_CHECK_THROW((validator{code, nullptr}), wasm::exceptions::link);
   BOOST_CHECK(code == original);
}

BOOST_AUTO_TEST_CASE(validation_host_profile_valid_signature_never_executes_host_or_start) {
   register_validation_profile();
   validation_profile_host::calls = 0;
   using validator = wasm::backend<validation_profile, wasm::null_backend>;
   auto code = imported_start();
   const auto original = code;
   auto value = validator{code, nullptr};
   BOOST_CHECK_NO_THROW(value.validate());
   BOOST_CHECK_NO_THROW(value.validate());
   BOOST_CHECK(code == original);
   BOOST_TEST(validation_profile_host::calls == 0U);
}

BOOST_AUTO_TEST_CASE(validation_host_profile_rejects_unknown_name_module_and_incompatible_signatures) {
   register_validation_profile();
   validation_profile_host::calls = 0;
   using validator = wasm::backend<validation_profile, wasm::null_backend>;
   for (auto code : {imported_start("env", "missing"), imported_start("other"), imported_start("env", "probe", 0x7e),
                     imported_start("env", "probe", 0x7f, 0x7e)}) {
      const auto original = code;
      BOOST_CHECK_THROW((validator{code, nullptr}), wasm::exceptions::link);
      BOOST_CHECK(code == original);
   }
   BOOST_TEST(validation_profile_host::calls == 0U);
}

BOOST_AUTO_TEST_CASE(validation_host_profile_empty_and_registered_accept_no_function_imports) {
   register_validation_profile();
   auto code = wasm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   using empty_validator = wasm::backend<empty_validation_profile, wasm::null_backend>;
   using registered_validator = wasm::backend<validation_profile, wasm::null_backend>;
   BOOST_CHECK_NO_THROW((empty_validator{code, nullptr}));
   BOOST_CHECK_NO_THROW((registered_validator{code, nullptr}));
}

BOOST_AUTO_TEST_CASE(validation_only_backend_rejects_malformed_wasm_without_an_allocator) {
   auto invalid = wasm::wasm_code{0, 0, 0, 0};
   using validator = wasm::backend<std::nullptr_t, wasm::null_backend>;

   BOOST_CHECK_THROW((validator{invalid, nullptr}), wasm::exceptions::parse);
}

BOOST_AUTO_TEST_CASE(prepared_module_validation_only_preserves_source_and_does_not_run_start) {
   auto code = wasm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0,  0, 0, 1, 4, 1, 0x60, 0,    0, 3,    2,
                               1, 0,    8,    1,    0, 10, 9, 1, 7, 0, 3, 0x40, 0x0c, 0, 0x0b, 0x0b};
   const auto original = code;
   auto prepared = wasm::prepared_module{code, wasm::options{}, {}, wasm::prepared_module::purpose::validation_only};

   BOOST_CHECK_NO_THROW(prepared.validate());
   BOOST_CHECK_NO_THROW(prepared.validate());
   BOOST_CHECK(code == original);
}

BOOST_AUTO_TEST_CASE(prepared_module_validation_checks_data_without_an_execution_context) {
   // One page of memory and one byte of active data placed just beyond it.
   auto code =
       forge::vm::wasm::interpret::wasm_code{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x00,
                                             0x01, 0x0b, 0x09, 0x01, 0x00, 0x41, 0x80, 0x80, 0x04, 0x0b, 0x01, 0x58};
   auto pointer = forge::vm::wasm::interpret::wasm_code_ptr{code.data(), code.size()};
   auto parsed = forge::vm::wasm::interpret::backend<>{
       pointer, code.size(), nullptr, forge::vm::wasm::interpret::default_options{}, true, false};

   BOOST_CHECK_THROW(parsed.validate(), forge::vm::wasm::interpret::exceptions::memory);
}

BOOST_AUTO_TEST_CASE(prepared_module_validation_does_not_execute_start) {
   // The start function loops forever. Admission must not enter it.
   auto code = forge::vm::wasm::interpret::wasm_code{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01,
                                                     0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x08, 0x01, 0x00, 0x0a,
                                                     0x09, 0x01, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b};
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

BOOST_AUTO_TEST_CASE(memory_index_zero_accepts_all_current_u32_encodings) {
   for (const auto opcode : {0x3f, 0x40}) {
      for (std::size_t length = 1; length <= 5; ++length) {
         auto index = std::vector<std::uint8_t>(length, 0x80);
         index.back() = 0;
         auto code = memory_instruction(opcode, index);
         auto validator = wasm::backend<std::nullptr_t, wasm::null_backend>{code, nullptr};
         BOOST_CHECK_NO_THROW(validator.validate());
         check_memory_index_encoding<wasm::interpreter>(opcode, index);
#if FORGE_VM_WASM_INTERPRET_HAS_JIT
         check_memory_index_encoding<wasm::jit>(opcode, index);
#endif
      }
   }
}

BOOST_AUTO_TEST_CASE(memory_index_validation_rejects_nonzero_overflow_and_missing_memory) {
   for (const auto opcode : {0x3f, 0x40}) {
      for (const auto& index : {std::vector<std::uint8_t>{1}, std::vector<std::uint8_t>{0x80, 0x80, 0x80, 0x80, 0x10},
                                std::vector<std::uint8_t>{0x80, 0x80, 0x80, 0x80, 0x80, 0}}) {
         auto code = memory_instruction(opcode, index);
         BOOST_CHECK_THROW((wasm::backend<std::nullptr_t, wasm::null_backend>{code, nullptr}), wasm::exceptions::parse);
      }
      const std::uint8_t zero = 0;
      auto no_memory = memory_instruction(opcode, {&zero, 1}, false);
      BOOST_CHECK_THROW((wasm::backend<std::nullptr_t, wasm::null_backend>{no_memory, nullptr}),
                        wasm::exceptions::parse);
   }
}

BOOST_AUTO_TEST_CASE(unreachable_branch_table_uses_common_operand_types) {
   // Donor unreached-invalid.87: unreachable supplies bottom to f32/f64 labels.
   const auto fixture = wasm::wasm_code{0, 0x61, 0x73, 0x6d, 1,    0, 0, 0,    1, 4,    1, 0x60, 0,    0,    3,   2, 1,
                                        0, 10,   0x1d, 1,    0x1b, 0, 2, 0x7c, 2, 0x7d, 0, 0x41, 1,    0x0e, 2,   0, 1,
                                        1, 0x0b, 0x1a, 0x44, 0,    0, 0, 0,    0, 0,    0, 0,    0x0b, 0x1a, 0x0b};
   auto code = fixture;
   auto validator = wasm::backend<std::nullptr_t, wasm::null_backend>{code, nullptr};
   BOOST_CHECK_NO_THROW(validator.validate());
   BOOST_CHECK(code == fixture);

   // Each floating-point control matches only one target, not the whole table.
   for (const auto& operand : {std::vector<std::uint8_t>{0x41, 0}, std::vector<std::uint8_t>{0x43, 0, 0, 0, 0},
                               std::vector<std::uint8_t>{0x44, 0, 0, 0, 0, 0, 0, 0, 0}}) {
      auto invalid = fixture;
      invalid.insert(invalid.begin() + 28, operand.begin(), operand.end());
      invalid[19] += operand.size();
      invalid[21] += operand.size();
      BOOST_CHECK_THROW((wasm::backend<std::nullptr_t, wasm::null_backend>{invalid, nullptr}), wasm::exceptions::parse);
   }
}
