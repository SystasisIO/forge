#include "details/engine_limits.hxx"

#include <algorithm>
#include <cstring>
#include <wasm_runtime.h>
#include <wasm_interp.h>
#include <aot_runtime.h>
#if WASM_ENABLE_GC != 0
#include <gc_type.h>
#endif

namespace forge::vm::wasm::interpret::detail {

engine_limits::exceeded::exceeded(metric field, std::uint64_t actual, std::uint64_t allowed)
    : std::length_error("WASM module exceeds its admission limit"), field(field), actual(actual), allowed(allowed) {}

engine_limits::engine_limits(wasm_module_t source) {
   if (!source || source->module_type != Wasm_Module_Bytecode)
      throw std::invalid_argument("Metadata admission requires the original validated WASM module");
   const auto& module = *reinterpret_cast<const WASMModule*>(source);
   if (module.import_table_count || module.import_memory_count || module.import_global_count)
      throw std::invalid_argument("Only function imports are supported by this admission profile");
   const auto set = [&](metric key, std::uint64_t value) { _values[static_cast<std::size_t>(key)] = value; };
   const auto maximum = [&](metric key, std::uint64_t value) { set(key, std::max(get(key), value)); };
   set(metric::types, module.type_count);
   set(metric::imports, module.import_count);
   set(metric::functions, module.function_count);
   set(metric::globals, module.global_count);
   set(metric::exports, module.export_count);
   set(metric::elements, module.table_seg_count);
   set(metric::data, module.data_seg_count);
   set(metric::legacy_depth, module.forge_max_legacy_depth);
   static_assert(sizeof(void*) == 8, "The shared native profile requires a 64-bit host");
   // Prospective AOT layout: get_tbl_inst_offset/get_module_inst_extra_offset
   // use this upstream header, numeric global size and table stride on 64-bit hosts.
   auto metadata =
       align_uint64(offsetof(AOTModuleInstance, global_table_data.bytes) +
                        std::uint64_t(module.memory_count) * sizeof(AOTMemoryInstance) + module.global_data_size,
                    8);
   for (std::uint32_t i = 0; i < module.table_count; ++i) {
      const auto& table = module.tables[i].table_type;
      metadata += wasm_table_instance_size(table.possible_grow ? table.max_size : table.init_size, sizeof(void*));
   }
   set(metric::instance_metadata_bytes, metadata + sizeof(AOTModuleInstanceExtra));
   for (std::uint32_t i = 0; i < module.type_count; ++i) {
#if WASM_ENABLE_GC != 0
      if (!wasm_type_is_func_type(module.types[i]))
         throw std::invalid_argument("Only numeric function types are supported by this profile");
#endif
      const auto& type = *reinterpret_cast<const WASMFuncType*>(module.types[i]);
      maximum(metric::function_results, type.result_count);
      maximum(metric::function_storage_bytes, std::uint64_t(type.param_cell_num + type.ret_cell_num) * 4);
   }
   for (std::uint32_t i = 0; i < module.function_count; ++i) {
      const auto& function = *module.functions[i];
      maximum(metric::code_bytes, function.forge_body_bytes);
      maximum(metric::local_sets, function.forge_local_sets);
      maximum(metric::nested_structures, function.max_block_num);
      maximum(metric::control_depth, function.max_block_num ? function.max_block_num - 1 : 0);
      maximum(metric::br_table_elements, function.forge_br_table_elements);
      maximum(metric::memory_offset, function.forge_memory_offset);
      maximum(metric::function_storage_bytes,
              std::uint64_t(function.param_cell_num + function.local_cell_num + function.forge_operand_cells) * 4);
      std::uint64_t cells =
          std::uint64_t(function.param_cell_num) + function.local_cell_num + function.max_stack_cell_num;
#if WASM_ENABLE_FAST_INTERP != 0
      cells += function.const_cell_num;
#else
      cells = (cells + sizeof(void*) / 4 - 1) / (sizeof(void*) / 4) * (sizeof(void*) / 4);
      cells += std::uint64_t(function.max_block_num) * sizeof(WASMBranchBlock) / 4;
#endif
      if (cells > UINT32_MAX / 8)
         throw std::length_error("WASM frame size is not representable");
      _frame_bytes = std::max<std::uint64_t>(_frame_bytes, wasm_interp_interp_frame_size(static_cast<unsigned>(cells)));
      for (unsigned flags = 0; flags < _function_bytes.size(); ++flags) {
         const auto cells = std::uint64_t(flags & 1 ? function.param_cell_num : 0) +
                            (flags & 2 ? function.local_cell_num : 0) + (flags & 4 ? function.forge_operand_cells : 0);
         _function_bytes[flags] = std::max(_function_bytes[flags], cells * 4);
      }
   }
   set(metric::function_bytes, function_bytes(6));
   for (std::uint32_t i = 0; i < module.import_function_count; ++i) {
      const auto& function = module.import_functions[i].u.function;
      maximum(metric::symbol_bytes, std::strlen(function.module_name));
      maximum(metric::symbol_bytes, std::strlen(function.field_name));
   }
   for (std::uint32_t i = 0; i < module.export_count; ++i) {
      const auto& item = module.exports[i];
      maximum(metric::symbol_bytes, item.name_len);
      if (item.kind == EXPORT_KIND_GLOBAL && module.globals[item.index].type.is_mutable)
         _exports_mutable_global = true;
   }
   for (std::uint32_t i = 0; i < module.global_count; ++i) {
      const auto& item = module.globals[i];
      if (!item.type.is_mutable)
         continue;
      std::uint64_t bytes;
      switch (item.type.val_type) {
      case VALUE_TYPE_I32:
      case VALUE_TYPE_F32:
         bytes = 4;
         break;
      case VALUE_TYPE_I64:
      case VALUE_TYPE_F64:
         bytes = 8;
         break;
      default:
         throw std::invalid_argument("Unsupported WASM global value kind");
      }
      set(metric::mutable_global_bytes, get(metric::mutable_global_bytes) + bytes);
   }
   for (std::uint32_t i = 0; i < module.table_count; ++i)
      maximum(metric::table_elements, module.tables[i].table_type.init_size);
   for (std::uint32_t i = 0; i < module.table_seg_count; ++i)
      maximum(metric::element_segment_elements, module.table_segments[i].value_count);
   for (std::uint32_t i = 0; i < module.data_seg_count; ++i) {
      const auto& segment = *module.data_segments[i];
      if (segment.base_offset.init_expr_type != INIT_EXPR_TYPE_I32_CONST)
         throw std::invalid_argument("Unsupported WASM data-segment initializer");
      maximum(metric::data_segment_bytes, segment.data_length);
      maximum(metric::linear_memory_init,
              static_cast<std::uint64_t>(static_cast<std::uint32_t>(segment.base_offset.u.unary.v.i32)) +
                  segment.data_length);
      if (segment.memory_index >= module.memory_count)
         _valid_memory_segments = false;
      else {
         const auto& memory = module.memories[segment.memory_index];
         const auto end = static_cast<std::uint64_t>(static_cast<std::uint32_t>(segment.base_offset.u.unary.v.i32)) +
                          segment.data_length;
         if (end > static_cast<std::uint64_t>(memory.init_page_count) * memory.num_bytes_per_page)
            _valid_memory_segments = false;
      }
   }
   for (std::uint32_t i = 0; i < module.memory_count; ++i) {
      const auto& memory = module.memories[i];
      // WAMR may coalesce non-growing memory into a single internal page.
      // Public policy still counts WebAssembly's 64 KiB pages.
      const auto bytes = static_cast<std::uint64_t>(memory.init_page_count) * memory.num_bytes_per_page;
      maximum(metric::pages, (bytes + 65535) / 65536);
   }
   for (std::uint32_t i = 0; i < module.table_seg_count; ++i) {
      const auto& segment = module.table_segments[i];
      if (segment.table_index >= module.table_count || segment.base_offset.init_expr_type != INIT_EXPR_TYPE_I32_CONST)
         _valid_table_segments = false;
      else if (static_cast<std::uint64_t>(static_cast<std::uint32_t>(segment.base_offset.u.unary.v.i32)) +
                   segment.value_count >
               module.tables[segment.table_index].table_type.init_size)
         _valid_table_segments = false;
   }
}

std::uint32_t engine_limits::interpreter_stack_bytes(std::uint32_t depth) const {
   // Include the entry/host/outgoing frames and alignment slack. The actual
   // upstream frame layout, including fast-interpreter constants, is measured.
   const auto size = (_frame_bytes + 256) * (static_cast<std::uint64_t>(depth) + 2) + 64 * 1024;
   if (size > UINT32_MAX)
      throw std::length_error("WASM stack reservation exceeds the runtime capacity");
   return static_cast<std::uint32_t>(size);
}

void engine_limits::validate_instantiation() const {
   if (!_valid_memory_segments)
      throw invalid_instantiation{true};
   if (!_valid_table_segments)
      throw invalid_instantiation{false};
}

std::uint64_t engine_limits::get(metric field) const {
   const auto index = static_cast<std::size_t>(field);
   if (index >= _values.size())
      throw std::invalid_argument("Invalid WASM limit metric");
   return _values[index];
}

bool engine_limits::exports_mutable_global() const noexcept {
   return _exports_mutable_global;
}

std::uint64_t engine_limits::function_bytes(unsigned flags) const {
   if (flags >= _function_bytes.size())
      throw std::invalid_argument("Invalid function-byte accounting flags");
   return _function_bytes[flags];
}

void engine_limits::check(metric field, std::uint64_t allowed) const {
   const auto actual = get(field);
   if (actual > allowed)
      throw exceeded(field, actual, allowed);
}

} // namespace forge::vm::wasm::interpret::detail
