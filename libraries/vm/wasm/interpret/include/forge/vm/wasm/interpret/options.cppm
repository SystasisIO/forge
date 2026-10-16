module;

#include <cstdint>
#include <limits>
#include <boost/describe.hpp>

export module forge.vm.wasm.interpret.options;

export namespace forge::vm::wasm::interpret {

enum class max_func_local_bytes_flags_t { params = 1, locals = 2, stack = 4 };
constexpr max_func_local_bytes_flags_t operator|(max_func_local_bytes_flags_t lhs, max_func_local_bytes_flags_t rhs) {
   return static_cast<max_func_local_bytes_flags_t>(static_cast<int>(lhs) | static_cast<int>(rhs));
}
constexpr max_func_local_bytes_flags_t operator&(max_func_local_bytes_flags_t lhs, max_func_local_bytes_flags_t rhs) {
   return static_cast<max_func_local_bytes_flags_t>(static_cast<int>(lhs) & static_cast<int>(rhs));
}

struct options {
   std::uint64_t max_mutable_global_bytes = std::numeric_limits<std::uint64_t>::max();
   std::uint32_t max_table_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_section_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_type_section_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_import_section_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_function_section_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_global_section_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_export_section_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_element_section_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_data_section_elements = std::numeric_limits<std::uint32_t>::max();
   // code is the same as functions
   // memory and tables are both 1.
   std::uint32_t max_element_segment_elements = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_data_segment_bytes = std::numeric_limits<std::uint32_t>::max();
   std::uint64_t max_linear_memory_init = std::numeric_limits<std::uint64_t>::max();
   std::uint64_t max_func_local_bytes = std::numeric_limits<std::uint64_t>::max();
   std::uint32_t max_local_sets = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_nested_structures = std::numeric_limits<std::uint32_t>::max();
   std::uint32_t max_br_table_elements = std::numeric_limits<std::uint32_t>::max();
   // The maximum length of symbols used for import and export
   std::uint32_t max_symbol_bytes = std::numeric_limits<std::uint32_t>::max();
   // The maximum offset used for load and store
   std::uint32_t max_memory_offset = std::numeric_limits<std::uint32_t>::max();
   // The maximum size of a function body
   std::uint32_t max_code_bytes = std::numeric_limits<std::uint32_t>::max();
   // The maximum size of linear memory in page units.
   std::uint32_t max_pages = std::numeric_limits<std::uint32_t>::max();
   // The maximum function call depth
   std::uint32_t max_call_depth = 251;
   // Can mutable globals be exported
   bool forbid_export_mutable_globals = false;
   // Very strange non-conforming behavior
   bool allow_code_after_function_end = false;
   // Whether limits types such as for tables and memory are restricted to a
   // single byte 0|1 (matching the spec) or can be a 32-bit value.
   bool allow_u32_limits_flags = false;
   // Determines whether an local set of size 0 with an invalid type should be accepted.
   bool allow_invalid_empty_local_set = false;
   bool allow_zero_blocktype = false;
   // Determines which components are counted towards max_function_local_bytes
   max_func_local_bytes_flags_t max_func_local_bytes_flags =
       max_func_local_bytes_flags_t::locals | max_func_local_bytes_flags_t::stack;
   std::uint64_t max_control_depth = std::numeric_limits<std::uint64_t>::max();
   std::uint64_t max_legacy_nested_structures = std::numeric_limits<std::uint64_t>::max();
   bool parse_custom_section_name = false;
   // resolve_options preserves the absence of checker traits in an arbitrary
   // options source. Direct options values enable their explicit limits.
   bool check_mutable_global_bytes = true;
   bool check_func_local_bytes = true;
   bool check_control_depth = true;
   bool check_legacy_nested_structures = true;

   friend bool operator==(const options&, const options&) = default;
};

BOOST_DESCRIBE_STRUCT(options, (),
                      (max_mutable_global_bytes, max_table_elements, max_section_elements, max_type_section_elements,
                       max_import_section_elements, max_function_section_elements, max_global_section_elements,
                       max_export_section_elements, max_element_section_elements, max_data_section_elements,
                       max_element_segment_elements, max_data_segment_bytes, max_linear_memory_init,
                       max_func_local_bytes, max_local_sets, max_nested_structures, max_br_table_elements,
                       max_symbol_bytes, max_memory_offset, max_code_bytes, max_pages, max_call_depth,
                       forbid_export_mutable_globals, allow_code_after_function_end, allow_u32_limits_flags,
                       allow_invalid_empty_local_set, allow_zero_blocktype, max_func_local_bytes_flags,
                       max_control_depth, max_legacy_nested_structures, parse_custom_section_name,
                       check_mutable_global_bytes, check_func_local_bytes, check_control_depth,
                       check_legacy_nested_structures))

struct default_options {};

struct compatibility_options {
   static constexpr std::uint32_t max_mutable_global_bytes = 1024;
   static constexpr std::uint32_t max_table_elements = 1024;
   // Function and import sections use the narrower compatibility limits below.
   static constexpr std::uint32_t max_section_elements = 8191;
   static constexpr std::uint32_t max_function_section_elements = 1023;
   static constexpr std::uint32_t max_import_section_elements = 1023;
   static constexpr std::uint32_t max_element_segment_elements = 8191;
   static constexpr std::uint32_t max_data_segment_bytes = 8191;
   static constexpr std::uint32_t max_linear_memory_init = 64 * 1024;
   static constexpr std::uint32_t max_func_local_bytes = 8192;
   static constexpr std::uint32_t max_local_sets = 1023;
   static constexpr std::uint32_t max_control_depth = 1023;
   static constexpr std::uint32_t max_legacy_nested_structures = 1023;
   static constexpr std::uint32_t max_br_table_elements = 8191;
   static constexpr std::uint32_t max_symbol_bytes = 8191;
   static constexpr std::uint32_t max_memory_offset = (33 * 1024 * 1024 - 1);
   static constexpr std::uint32_t max_pages = 528; // 33 MiB
   static constexpr std::uint32_t max_call_depth = 251;

   static constexpr bool forbid_export_mutable_globals = true;
   static constexpr bool allow_code_after_function_end = true;
   static constexpr bool allow_u32_limits_flags = true;
   static constexpr bool allow_invalid_empty_local_set = true;
   static constexpr bool allow_zero_blocktype = true;
};

} // namespace forge::vm::wasm::interpret
