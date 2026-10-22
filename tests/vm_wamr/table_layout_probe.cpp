#include <aot_emit_table.h>
#include <aot_runtime.h>
#include <wasm_runtime.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::uint64_t aligned(std::uint64_t value) { return (value + 7) & ~std::uint64_t{7}; }
void offsets(unsigned width, bool jit) {
   std::array<AOTImportTable, 2> imports{};
   std::array<AOTTable, 2> tables{};
   imports[0].table_type.init_size = 1;
   imports[0].table_type.max_size = 7;
   tables[0].table_type.init_size = 3;
   tables[0].table_type.max_size = 9;
   tables[0].table_type.possible_grow = true;
   tables[1].table_type.init_size = 1;
   tables[1].table_type.max_size = 8;
   AOTCompData data{};
   data.memory_count = 1;
   data.global_data_size_32bit = 12;
   data.global_data_size_64bit = 20;
   data.import_table_count = imports.size();
   data.import_tables = imports.data();
   data.table_count = tables.size();
   data.tables = tables.data();
   AOTCompContext context{};
   context.comp_data = &data;
   context.pointer_size = width;
   context.is_jit_mode = jit;
   const auto base = std::uint64_t{offsetof(AOTModuleInstance, global_table_data.bytes)}
      + sizeof(AOTMemoryInstance) + (width == 8 ? 20 : 12);
   auto expected = aligned(base);
   const std::array<unsigned, 4> slots{1, 0, 9, 1};
   for (unsigned index = 0; index <= slots.size(); ++index) {
      require(get_tbl_inst_offset(&context, nullptr, index) == expected, "AOT table offset mismatch");
      require(expected % 8 == 0, "AOT table start is not aligned");
      if (index < slots.size()) {
         const auto stride = aligned(24 + std::uint64_t{width} * slots[index]);
         require(wasm_table_instance_size(slots[index], width) == stride, "Table stride mismatch");
         expected += stride;
      }
   }
   require(get_module_inst_extra_offset(&context) == expected, "AOT extra offset mismatch");
   data.import_table_count = data.table_count = 0;
   require(get_module_inst_extra_offset(&context) == aligned(base), "Zero-table layout mismatch");
   require(wasm_table_instance_size(0, width) == 24, "Zero-slot table lost its aligned header");
   require(wasm_table_instance_size(UINT32_MAX, width) > UINT32_MAX, "Table arithmetic narrowed early");

   data.memory_count = UINT32_MAX;
   require(get_module_inst_extra_offset(&context) == UINT32_MAX, "Oversized memory prefix was truncated");
   data.memory_count = 0;
   const auto header = offsetof(AOTModuleInstance, global_table_data.bytes);
   data.global_data_size_32bit = data.global_data_size_64bit = UINT32_MAX - header;
   require(get_module_inst_extra_offset(&context) == UINT32_MAX, "Aligned prefix overflow was truncated");
   const auto extra = jit ? sizeof(WASMModuleInstanceExtra) : sizeof(AOTModuleInstanceExtra);
   const auto signed_limit = std::uint64_t{INT32_MAX} + 1;
   data.global_data_size_32bit = data.global_data_size_64bit = signed_limit - header - extra;
   require(get_module_inst_extra_offset(&context) == UINT32_MAX, "Signed offset boundary was accepted");
   data.global_data_size_32bit = data.global_data_size_64bit = signed_limit - header - extra - 8;
   require(get_module_inst_extra_offset(&context) == signed_limit - extra - 8,
      "Last aligned positive allocation extent was rejected");
   data.global_data_size_32bit = data.global_data_size_64bit = UINT32_MAX - header - extra;
   require(get_module_inst_extra_offset(&context) == UINT32_MAX, "Extra allocation extent overflow was accepted");
   data.global_data_size_32bit = data.global_data_size_64bit = 0;
   data.import_table_count = imports.size();
   imports[0].table_type.init_size = imports[0].table_type.max_size = UINT32_MAX / 4;
   require(get_module_inst_extra_offset(&context) == UINT32_MAX, "Oversized table stride was truncated");
   data.import_table_count = UINT32_MAX;
   data.table_count = 1;
   require(get_module_inst_extra_offset(&context) == UINT32_MAX, "Table-count addition overflow was accepted");
}
}
int main() {
   try {
      static_assert(offsetof(WASMTableInstance, elems) == 24);
      for (unsigned width : {4, 8}) for (bool jit : {false, true}) offsets(width, jit);
      std::cout << "PASS target32/64 table start/stride/zero/overflow (release checks)\n";
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
