module;

#include <chrono>
#include <cstdint>
#include <limits>

module forge.execution.wasm.limits;
import forge.execution.wasm.exceptions;

namespace forge::execution::wasm {

compile_limits::compile_limits()
   : vm{.max_mutable_global_bytes = 1024 * 1024,
        .max_table_elements = 16384,
        .max_section_elements = 16384,
        .max_type_section_elements = 16384,
        .max_import_section_elements = 64,
        .max_function_section_elements = 16384,
        .max_global_section_elements = 16384,
        .max_export_section_elements = 16384,
        .max_element_section_elements = 16384,
        .max_data_section_elements = 16384,
        .max_element_segment_elements = 16384,
        .max_data_segment_bytes = 4 * 1024 * 1024,
        .max_linear_memory_init = 256 * 65536,
        .max_func_local_bytes = 1024 * 1024,
        .max_local_sets = 16384,
        .max_nested_structures = 1024,
        .max_br_table_elements = 16384,
        .max_symbol_bytes = 1024,
        .max_memory_offset = std::numeric_limits<std::uint32_t>::max(),
        .max_code_bytes = 4 * 1024 * 1024,
        .max_pages = 256,
        .max_call_depth = 256} {}

void validate(const compile_limits& value) {
   if (value.max_module_bytes == 0 || value.max_module_bytes > std::numeric_limits<std::uint32_t>::max() ||
       value.vm.max_pages > 65536 || value.vm.max_call_depth == 0 ||
       value.vm.max_call_depth > 65536 || value.vm.max_pages == 0 ||
       value.vm.allow_code_after_function_end || value.vm.allow_u32_limits_flags ||
       value.vm.allow_invalid_empty_local_set || value.vm.allow_zero_blocktype) {
      throw exceptions::invalid_options{"Invalid WASM compilation limits"};
   }
}

void validate(const invocation_limits& value) {
   constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
   if (value.budget.count() <= 0 || value.budget > std::chrono::hours{24} ||
       value.max_input_bytes > maximum || value.max_output_bytes > maximum || value.max_console_bytes > maximum) {
      throw exceptions::invalid_options{"Invalid WASM invocation limits"};
   }
}

} // namespace forge::execution::wasm
