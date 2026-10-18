#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <wasm_export.h>

namespace forge::vm::wasm::interpret::detail {

// Observations of the upstream-validated bytecode module, not a second parser
// or another options record. Runtime call-depth admission is separate.
class engine_limits {
 public:
   enum class metric : std::size_t {
      types,
      imports,
      functions,
      globals,
      exports,
      elements,
      data,
      mutable_global_bytes,
      table_elements,
      element_segment_elements,
      data_segment_bytes,
      linear_memory_init,
      symbol_bytes,
      pages,
      code_bytes,
      local_sets,
      nested_structures,
      control_depth,
      legacy_depth,
      br_table_elements,
      memory_offset,
      function_bytes,
      function_storage_bytes,
      function_results,
      instance_metadata_bytes,
      count
   };
   class exceeded : public std::length_error {
    public:
      exceeded(metric field, std::uint64_t actual, std::uint64_t allowed);
      metric field;
      std::uint64_t actual;
      std::uint64_t allowed;
   };
   class invalid_instantiation : public std::out_of_range {
    public:
      explicit invalid_instantiation(bool memory)
          : std::out_of_range("WASM active segment is outside its initial storage"), memory(memory) {}
      bool memory;
   };
   explicit engine_limits(wasm_module_t);
   std::uint64_t get(metric) const;
   bool exports_mutable_global() const noexcept;
   std::uint64_t function_bytes(unsigned flags) const;
   std::uint32_t interpreter_stack_bytes(std::uint32_t depth) const;
   void validate_instantiation() const;

   template <typename Profile> void check_profile() const {
      check(metric::pages, Profile::max_pages);
      check(metric::instance_metadata_bytes, Profile::max_instance_metadata_bytes);
      check(metric::function_results, Profile::max_function_results);
      check(metric::function_storage_bytes, Profile::max_function_storage_bytes);
      check(metric::control_depth, Profile::max_control_depth);
   }

   template <typename Profile, typename Options> void check_profile(const Options& options) const {
      if constexpr (requires { options.max_pages; })
         if (options.max_pages != UINT32_MAX && options.max_pages > Profile::max_pages)
            throw exceeded(metric::pages, options.max_pages, Profile::max_pages);
      check_profile<Profile>();
   }

   template <typename Options> void check_instructions(const Options& options) const {
#define FORGE_CHECK_INSTRUCTION(field, observation)                                                                    \
   if constexpr (requires { options.field; })                                                                          \
      check(metric::observation, options.field);
      FORGE_CHECK_INSTRUCTION(max_code_bytes, code_bytes)
      FORGE_CHECK_INSTRUCTION(max_local_sets, local_sets)
      FORGE_CHECK_INSTRUCTION(max_nested_structures, nested_structures)
      FORGE_CHECK_INSTRUCTION(max_br_table_elements, br_table_elements)
      FORGE_CHECK_INSTRUCTION(max_memory_offset, memory_offset)
#undef FORGE_CHECK_INSTRUCTION
      if constexpr (requires { options.max_control_depth; }) {
         bool enabled = true;
         if constexpr (requires { options.check_control_depth; })
            enabled = options.check_control_depth;
         if (enabled)
            check(metric::control_depth, options.max_control_depth);
      }
      if constexpr (requires { options.max_legacy_nested_structures; }) {
         bool enabled = true;
         if constexpr (requires { options.check_legacy_nested_structures; })
            enabled = options.check_legacy_nested_structures;
         if (enabled)
            check(metric::legacy_depth, options.max_legacy_nested_structures);
      }
      if constexpr (requires { options.max_func_local_bytes; }) {
         bool enabled = true;
         if constexpr (requires { options.check_func_local_bytes; })
            enabled = options.check_func_local_bytes;
         if (enabled) {
            unsigned flags = 6; // Public default: locals | operand stack.
            if constexpr (requires { options.max_func_local_bytes_flags; })
               flags = static_cast<unsigned>(options.max_func_local_bytes_flags);
            const auto actual = function_bytes(flags);
            if (actual > options.max_func_local_bytes)
               throw exceeded(metric::function_bytes, actual, options.max_func_local_bytes);
         }
      }
   }

   template <typename Options> void check_metadata(const Options& options) const {
      std::uint64_t fallback = UINT32_MAX;
      if constexpr (requires { options.max_section_elements; })
         fallback = options.max_section_elements;
#define FORGE_CHECK_SECTION(field, observation)                                                                        \
   if constexpr (requires { options.field; })                                                                          \
      check(metric::observation, options.field);                                                                       \
   else                                                                                                                \
      check(metric::observation, fallback);
      FORGE_CHECK_SECTION(max_type_section_elements, types)
      FORGE_CHECK_SECTION(max_import_section_elements, imports)
      FORGE_CHECK_SECTION(max_function_section_elements, functions)
      FORGE_CHECK_SECTION(max_global_section_elements, globals)
      FORGE_CHECK_SECTION(max_export_section_elements, exports)
      FORGE_CHECK_SECTION(max_element_section_elements, elements)
      FORGE_CHECK_SECTION(max_data_section_elements, data)
#undef FORGE_CHECK_SECTION
#define FORGE_CHECK_METADATA(field, observation)                                                                       \
   if constexpr (requires { options.field; })                                                                          \
      check(metric::observation, options.field);
      FORGE_CHECK_METADATA(max_table_elements, table_elements)
      FORGE_CHECK_METADATA(max_element_segment_elements, element_segment_elements)
      FORGE_CHECK_METADATA(max_data_segment_bytes, data_segment_bytes)
      FORGE_CHECK_METADATA(max_linear_memory_init, linear_memory_init)
      FORGE_CHECK_METADATA(max_symbol_bytes, symbol_bytes)
      FORGE_CHECK_METADATA(max_pages, pages)
#undef FORGE_CHECK_METADATA
      if constexpr (requires { options.max_mutable_global_bytes; }) {
         bool enabled = true;
         if constexpr (requires { options.check_mutable_global_bytes; })
            enabled = options.check_mutable_global_bytes;
         if (enabled)
            check(metric::mutable_global_bytes, options.max_mutable_global_bytes);
      }
      if constexpr (requires { options.forbid_export_mutable_globals; })
         if (options.forbid_export_mutable_globals && _exports_mutable_global)
            throw std::invalid_argument("Exported mutable global is forbidden by the WASM profile");
   }

 private:
   void check(metric, std::uint64_t) const;
   std::array<std::uint64_t, static_cast<std::size_t>(metric::count)> _values{};
   std::array<std::uint64_t, 8> _function_bytes{};
   bool _exports_mutable_global = false;
   bool _valid_memory_segments = true;
   bool _valid_table_segments = true;
   std::uint64_t _frame_bytes = 0;
};

} // namespace forge::vm::wasm::interpret::detail
