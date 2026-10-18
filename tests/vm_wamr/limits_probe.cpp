#include "details/engine_module.hxx"
#include "details/engine_limits.hxx"
#include "../vm_wasm_execution/fixtures.hxx"
#include <iostream>

import forge.vm.wasm.interpret.options;

namespace vm = forge::vm::wasm::interpret;
namespace detail = vm::detail;
using metric = detail::engine_limits::metric;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
fixtures::bytes fixture() {
   fixtures::bytes out{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(out, 1, {1, 0x60, 0, 0});
   fixtures::bytes imports{1};
   fixtures::text(imports, "env"); fixtures::text(imports, "noop");
   imports.insert(imports.end(), {0, 0});
   fixtures::section(out, 2, imports);
   fixtures::section(out, 3, {1, 0});
   fixtures::section(out, 4, {1, 0x70, 0, 3});
   fixtures::section(out, 5, {1, 1, 2, 4});
   fixtures::section(out, 6, {1, 0x7e, 1, 0x42, 0, 0x0b});
   fixtures::bytes exports{2};
   fixtures::text(exports, "apply"); exports.insert(exports.end(), {0, 1});
   fixtures::text(exports, "global_value"); exports.insert(exports.end(), {3, 0});
   fixtures::section(out, 7, exports);
   fixtures::section(out, 9, {1, 0, 0x41, 0, 0x0b, 1, 1});
   fixtures::section(out, 10, {1, 2, 0, 0x0b});
   fixtures::section(out, 11, {1, 0, 0x41, 0xff, 0xff, 3, 0x0b, 2, 0x41, 0x42});
   return out;
}
}

int main() {
   try {
      auto host = std::make_shared<detail::engine_bindings>(std::vector{
         detail::engine_bindings::describe("env", "noop", static_cast<void (*)()>(nullptr),
            +[](detail::engine_bindings::call&) { throw std::logic_error("Admission must never invoke imports"); })});
      detail::engine_module module(detail::engine_runtime::acquire(), fixture(), host);
      const detail::engine_limits measured(module.get());
      vm::options accepted{};
      accepted.max_section_elements = 0; // Explicit section bounds take precedence, including zero.
      accepted.max_type_section_elements = 1;
      accepted.max_import_section_elements = 1;
      accepted.max_function_section_elements = 1;
      accepted.max_global_section_elements = 1;
      accepted.max_export_section_elements = 2;
      accepted.max_element_section_elements = 1;
      accepted.max_data_section_elements = 1;
      accepted.max_mutable_global_bytes = 8;
      accepted.max_table_elements = 3;
      accepted.max_element_segment_elements = 1;
      accepted.max_data_segment_bytes = 2;
      accepted.max_linear_memory_init = 65537;
      accepted.max_symbol_bytes = 12;
      accepted.max_pages = 2;
      measured.check_metadata(accepted);
      const auto rejected = [&](auto options, metric expected) {
         try { measured.check_metadata(options); }
         catch (const detail::engine_limits::exceeded& error) {
            require(error.field == expected && error.actual == measured.get(expected) && error.actual > error.allowed,
               "Wrong limit classification/accounting");
            return;
         }
         throw std::runtime_error("Metadata limit was ignored");
      };
#define CHECK(field, observed) { auto options = accepted; --options.field; rejected(options, metric::observed); }
      CHECK(max_type_section_elements, types)
      CHECK(max_import_section_elements, imports)
      CHECK(max_function_section_elements, functions)
      CHECK(max_global_section_elements, globals)
      CHECK(max_export_section_elements, exports)
      CHECK(max_element_section_elements, elements)
      CHECK(max_data_section_elements, data)
      CHECK(max_mutable_global_bytes, mutable_global_bytes)
      CHECK(max_table_elements, table_elements)
      CHECK(max_element_segment_elements, element_segment_elements)
      CHECK(max_data_segment_bytes, data_segment_bytes)
      CHECK(max_linear_memory_init, linear_memory_init)
      CHECK(max_symbol_bytes, symbol_bytes)
      CHECK(max_pages, pages)
#undef CHECK
      struct fallback { std::uint32_t max_section_elements; };
      measured.check_metadata(fallback{2});
      rejected(fallback{1}, metric::exports);
      rejected(fallback{0}, metric::types);
      require(measured.exports_mutable_global(), "Mutable global export was missed");
      accepted.forbid_export_mutable_globals = true;
      bool forbidden = false;
      try { measured.check_metadata(accepted); }
      catch (const std::invalid_argument&) { forbidden = true; }
      require(forbidden, "Mutable export policy was ignored");
      std::cout << "PASS 14 metadata limits, zero/explicit/fallback boundaries and mutable export policy\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n'; return 1;
   }
}
