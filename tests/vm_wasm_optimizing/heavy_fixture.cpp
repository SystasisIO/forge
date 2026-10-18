#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using bytes = std::vector<std::uint8_t>;
void u32(bytes& out, std::uint32_t value) {
   do {
      const auto next = value >> 7;
      out.push_back((value & 127) | (next ? 128 : 0));
      value = next;
   } while (value);
}
void name(bytes& out, std::string_view value) {
   u32(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}
void section(bytes& out, std::uint8_t id, const bytes& value) {
   out.push_back(id);
   u32(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}
void call(bytes& out, std::uint32_t index) {
   out.push_back(0x10);
   u32(out, index);
}
void local(bytes& out, std::uint8_t opcode, std::uint32_t index) {
   out.push_back(opcode);
   u32(out, index);
}
bytes fixture(std::uint32_t budget, bool changed_body = false, bool changed_initializer = false) {
   bytes out{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   section(out, 1, {4, 0x60, 0, 1, 0x7e, 0x60, 1, 0x7e, 0, 0x60, 1, 0x7f, 0, 0x60, 0, 0});
   bytes imports;
   u32(imports, 3);
   for (const auto& [field, type] :
        std::vector<std::pair<std::string_view, unsigned>>{{"next", 0}, {"consume", 1}, {"mark", 2}}) {
      name(imports, "env");
      name(imports, field);
      imports.push_back(0);
      u32(imports, type);
   }
   section(out, 2, imports);
   section(out, 3, {2, 2, 3});
   section(out, 6, {1, 0x7f, 1, 0x41, static_cast<std::uint8_t>(changed_initializer ? 1 : 0), 0x0b});
   bytes exports;
   u32(exports, 3);
   name(exports, "heavy");
   exports.push_back(0);
   u32(exports, 3);
   name(exports, "spin");
   exports.push_back(0);
   u32(exports, 4);
   name(exports, "imported_next");
   exports.push_back(0);
   u32(exports, 0);
   section(out, 7, exports);
   const auto count = (budget - 16) / 8;
   bytes heavy;
   u32(heavy, 1);
   u32(heavy, count);
   heavy.push_back(0x7e);
   // Each result is unpredictable and every local remains observable AFTER
   // recursion. LLVM cannot remove padding or rematerialize these host calls.
   for (std::uint32_t i = 1; i <= count; ++i) {
      call(heavy, 0);
      local(heavy, 0x21, i);
   }
   local(heavy, 0x20, 0);
   if (changed_body)
      heavy.insert(heavy.end(), {0x41, 1, 0x6a});
   call(heavy, 2);
   local(heavy, 0x20, 0);
   heavy.insert(heavy.end(), {0x04, 0x40});
   local(heavy, 0x20, 0);
   heavy.insert(heavy.end(), {0x41, 1, 0x6b});
   call(heavy, 3);
   heavy.push_back(0x0b);
   for (std::uint32_t i = 1; i <= count; ++i) {
      local(heavy, 0x20, i);
      call(heavy, 1);
   }
   heavy.push_back(0x0b);
   bytes spin{0, 0x41, 0};
   call(spin, 2);
   spin.insert(spin.end(), {0x03, 0x40, 0x0c, 0, 0x0b, 0x0b});
   bytes code;
   u32(code, 2);
   u32(code, heavy.size());
   code.insert(code.end(), heavy.begin(), heavy.end());
   u32(code, spin.size());
   code.insert(code.end(), spin.begin(), spin.end());
   section(out, 10, code);
   return out;
}

bytes traps() {
   bytes out{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   section(out, 1, {5, 0x60, 0, 1, 0x7e, 0x60, 1, 0x7e, 0, 0x60, 1, 0x7f, 0, 0x60, 0, 0, 0x60, 0, 1, 0x7f});
   bytes imports;
   u32(imports, 3);
   for (const auto& [field, type] :
        std::vector<std::pair<std::string_view, unsigned>>{{"next", 0}, {"consume", 1}, {"mark", 2}}) {
      name(imports, "env");
      name(imports, field);
      imports.push_back(0);
      u32(imports, type);
   }
   section(out, 2, imports);
   section(out, 3, {12, 3, 3, 3, 3, 3, 3, 2, 4, 4, 0, 3, 3});
   section(out, 4, {1, 0x70, 0, 3});
   section(out, 5, {1, 1, 1, 1});
   const auto exported = std::vector<std::pair<std::string_view, unsigned>>{
       {"unreachable", 3},      {"divide_zero", 4},   {"overflow", 5},      {"conversion", 6},
       {"table_bounds", 7},     {"indirect_type", 8}, {"depth", 9},         {"private_call", 11},
       {"indirect_import", 12}, {"table_null", 13},   {"memory_bounds", 14}};
   bytes exports;
   u32(exports, exported.size());
   for (const auto& [field, index] : exported) {
      name(exports, field);
      exports.push_back(0);
      u32(exports, index);
   }
   section(out, 7, exports);
   section(out, 9, {1, 0, 0x41, 0, 0x0b, 2, 10, 0});
   const auto functions =
       std::vector<bytes>{{0, 0x00, 0x0b},
                          {0, 0x41, 1, 0x41, 0, 0x6d, 0x1a, 0x0b},
                          {0, 0x41, 0x80, 0x80, 0x80, 0x80, 0x78, 0x41, 0x7f, 0x6d, 0x1a, 0x0b},
                          {0, 0x44, 0, 0, 0, 0, 0, 0, 0xf8, 0x7f, 0xaa, 0x1a, 0x0b},
                          {0, 0x41, 3, 0x11, 4, 0, 0x1a, 0x0b},
                          {0, 0x41, 0, 0x11, 3, 0, 0x0b},
                          // The observable host call after recursion prevents tail-call collapse.
                          {0, 0x20, 0, 0x04, 0x40, 0x20, 0, 0x41, 1, 0x6b, 0x10, 9, 0x0b, 0x20, 0, 0x10, 2, 0x0b},
                          {0, 0x41, 7, 0x0b},
                          {0, 0x10, 10, 0x0b},
                          {0, 0x41, 1, 0x11, 0, 0, 0x0b},
                          {0, 0x41, 2, 0x11, 4, 0, 0x1a, 0x0b},
                          {0, 0x41, 0x80, 0x80, 0x04, 0x28, 2, 0, 0x1a, 0x0b}};
   bytes code;
   u32(code, functions.size());
   for (const auto& function : functions) {
      u32(code, function.size());
      code.insert(code.end(), function.begin(), function.end());
   }
   section(out, 10, code);
   return out;
}

void signed_constant(bytes& out, std::uint8_t opcode, std::uint64_t bits) {
   out.push_back(opcode);
   auto value = std::bit_cast<std::int64_t>(bits);
   for (;;) {
      const auto part = static_cast<std::uint8_t>(value) & 127U;
      value >>= 7;
      if ((value == 0 && !(part & 64U)) || (value == -1 && (part & 64U))) {
         out.push_back(part);
         return;
      }
      out.push_back(part | 128U);
   }
}

// The archived benchmark bodies, not a host implementation of their work.
bytes benchmark_body(bool memory) {
   auto body = bytes{2, 2, 0x7f, 2, 0x7e, 0x20, 0, 0x20, 1};
   const auto i32 = [&](std::uint32_t value) { signed_constant(body, 0x41, value); };
   const auto i64 = [&](std::uint64_t value) { signed_constant(body, 0x42, value); };
   const auto begin_loop = [&](std::uint8_t index, std::uint32_t bound) {
      body.insert(body.end(), {0x02, 0x40, 0x03, 0x40, 0x20, index});
      i32(bound);
      body.insert(body.end(), {0x4f, 0x0d, 1});
   };
   const auto end_loop = [&](std::uint8_t index) {
      body.insert(body.end(), {0x20, index, 0x41, 1, 0x6a, 0x21, index, 0x0c, 0, 0x0b, 0x0b});
   };
   i64(17);
   body.insert(body.end(), {0x89, 0x85, 0x20, 2, 0x85, 0x21, 5});
   if (!memory) {
      begin_loop(3, 4096);
      body.insert(body.end(), {0x20, 5, 0x20, 3, 0xad});
      i64(1);
      body.insert(body.end(), {0x7c, 0x7c});
      i64(13);
      body.push_back(0x89);
      i64(0x9e3779b185ebca87ULL);
      body.insert(body.end(), {0x7e, 0x22, 5, 0x20, 5});
      i64(7);
      body.insert(body.end(), {0x88, 0x85, 0x21, 5});
      end_loop(3);
   } else {
      begin_loop(3, 8192);
      body.insert(body.end(), {0x20, 3, 0x41, 3, 0x74, 0x20, 5, 0x20, 3, 0xad, 0x85, 0x37, 3, 0});
      end_loop(3);
      begin_loop(4, 8);
      body.insert(body.end(), {0x41, 0, 0x21, 3});
      begin_loop(3, 8192);
      body.insert(body.end(),
                  {0x20, 3, 0x41, 3, 0x74, 0x20, 3, 0x41, 3, 0x74, 0x29, 3, 0, 0x20, 5, 0x7c, 0x20, 3, 0xad, 0x7c});
      i64(13);
      body.insert(body.end(), {0x89, 0x20, 4, 0xad});
      i64(1);
      body.insert(body.end(), {0x7c, 0x85, 0x22, 6, 0x37, 3, 0, 0x20, 5, 0x20, 6, 0x85, 0x21, 5});
      end_loop(3);
      end_loop(4);
   }
   body.insert(body.end(), {0x20, 5, 0x10, 0, 0x0b});
   return body;
}

bytes operational_fixture() {
   auto types = std::vector<bytes>{{0x60, 1, 0x7e, 0}};
   auto signatures = std::vector<std::uint32_t>{};
   auto bodies = std::vector<bytes>{};
   auto exported = std::vector<std::pair<std::string, std::uint32_t>>{};
   const auto add = [&](std::string field, bytes parameters, std::uint8_t result, bytes body) {
      auto type = bytes{0x60};
      u32(type, parameters.size());
      type.insert(type.end(), parameters.begin(), parameters.end());
      type.push_back(result == 0x40 ? 0 : 1);
      if (result != 0x40)
         type.push_back(result);
      signatures.push_back(types.size());
      types.push_back(std::move(type));
      bodies.push_back(std::move(body));
      const auto index = static_cast<std::uint32_t>(bodies.size()); // import0
      exported.emplace_back(std::move(field), index);
      return index;
   };
   const auto operation = [&](std::string field, std::uint8_t op, std::uint8_t input,
                              std::uint8_t output, unsigned arity, bool relay) {
      auto parameters = bytes(arity, input);
      auto body = bytes{1, 1, input, 0x20, 0, 0x22, static_cast<std::uint8_t>(arity)};
      // FP wrappers read the typed local, not tee's stack copy; no physical spill claim.
      if (relay)
         body.insert(body.end(), {0x1a, 0x20, static_cast<std::uint8_t>(arity)});
      auto forwarded = bytes{0, 0x20, 0};
      if (arity == 2) {
         body.insert(body.end(), {0x20, 1});
         forwarded.insert(forwarded.end(), {0x20, 1});
      }
      body.insert(body.end(), {op, 0x0b});
      const auto index = add(field, parameters, output, body);
      if (relay) {
         call(forwarded, index);
         forwarded.push_back(0x0b);
         add("relay_" + std::to_string(op), parameters, output, forwarded);
      }
   };
   for (const bool wide : {false, true}) {
      const auto type = static_cast<std::uint8_t>(wide ? 0x7e : 0x7f);
      for (unsigned op = wide ? 0x51 : 0x46; op <= (wide ? 0x5a : 0x4f); ++op)
         operation("int_" + std::to_string(op), op, type, 0x7f, 2, false);
      for (unsigned op = wide ? 0x79 : 0x67; op <= (wide ? 0x8a : 0x78); ++op)
         operation("int_" + std::to_string(op), op, type, type, op <= (wide ? 0x7b : 0x69) ? 1 : 2, false);
      operation("int_" + std::to_string(wide ? 0x50 : 0x45), wide ? 0x50 : 0x45, type, 0x7f, 1, false);
      const auto fp = static_cast<std::uint8_t>(wide ? 0x7c : 0x7d);
      for (unsigned op = wide ? 0x61 : 0x5b; op <= (wide ? 0x66 : 0x60); ++op)
         operation("fp_" + std::to_string(op), op, fp, 0x7f, 2, true);
      for (unsigned op = wide ? 0x99 : 0x8b; op <= (wide ? 0xa6 : 0x98); ++op)
         operation("fp_" + std::to_string(op), op, fp, fp, op >= (wide ? 0xa0 : 0x92) ? 2 : 1, true);
   }
   operation("int_167", 0xa7, 0x7e, 0x7f, 1, false);
   operation("int_172", 0xac, 0x7f, 0x7e, 1, false);
   operation("int_173", 0xad, 0x7f, 0x7e, 1, false);
   for (const auto& entry : std::vector<bytes>{
            {0xa8, 0x7d, 0x7f}, {0xa9, 0x7d, 0x7f}, {0xaa, 0x7c, 0x7f}, {0xab, 0x7c, 0x7f},
            {0xae, 0x7d, 0x7e}, {0xaf, 0x7d, 0x7e}, {0xb0, 0x7c, 0x7e}, {0xb1, 0x7c, 0x7e},
            {0xb2, 0x7f, 0x7d}, {0xb3, 0x7f, 0x7d}, {0xb4, 0x7e, 0x7d}, {0xb5, 0x7e, 0x7d},
            {0xb7, 0x7f, 0x7c}, {0xb8, 0x7f, 0x7c}, {0xb9, 0x7e, 0x7c}, {0xba, 0x7e, 0x7c},
            {0xb6, 0x7c, 0x7d}, {0xbb, 0x7d, 0x7c}, {0xbc, 0x7d, 0x7f}, {0xbd, 0x7c, 0x7e},
            {0xbe, 0x7f, 0x7d}, {0xbf, 0x7e, 0x7c}})
      operation("fp_" + std::to_string(entry[0]), entry[0], entry[1], entry[2], 1, true);

   const auto cfg = std::vector<bytes>{
       {0, 0x20, 0, 0x04, 0x7f, 0x20, 1, 0x05, 0x41, 9, 0x0b, 0x0b},
       {0, 0x02, 0x7f, 0x41, 0x63, 0x41, 12, 0x20, 0, 0x0d, 0, 0x1a, 0x0b, 0x0b},
       {0, 0x02, 0x7f, 0x02, 0x7f, 0x41, 0x63, 0x41, 12, 0x20, 0, 0x0e, 2, 0, 1, 1,
        0x0b, 0x41, 1, 0x6a, 0x0b, 0x0b},
       {1, 1, 0x7f, 0x02, 0x40, 0x03, 0x40, 0x20, 0, 0x45, 0x0d, 1, 0x20, 2, 0x20, 0, 0x6a,
        0x21, 2, 0x20, 0, 0x41, 1, 0x6b, 0x21, 0, 0x0c, 0, 0x0b, 0x0b, 0x20, 2, 0x0b},
       {0, 0x41, 0x63, 0x20, 1, 0x0f, 0x0b},
       {0, 0x20, 0, 0x20, 1, 0x20, 0, 0x1b, 0x0b},
       {0, 0x02, 0x7f, 0x41, 12, 0x20, 0, 0x0e, 0, 0, 0x0b, 0x0b}};
   for (unsigned i = 0; i < cfg.size(); ++i)
      add("cfg_" + std::to_string(i), {0x7f, 0x7f}, 0x7f, cfg[i]);
   auto dense = bytes{0};
   for (unsigned i = 0; i < 16; ++i)
      dense.insert(dense.end(), {0x02, 0x40});
   dense.insert(dense.end(), {0x20, 0, 0x0e, 16});
   for (unsigned i = 0; i < 16; ++i)
      dense.push_back(i);
   dense.push_back(15);
   const auto ops = std::array<std::uint8_t, 8>{0x6a, 0x73, 0x6c, 0x70, 0x74, 0x78, 0x72, 0x71};
   for (unsigned i = 0; i < 16; ++i)
      dense.insert(dense.end(), {0x0b, 0x20, 1, 0x41, static_cast<std::uint8_t>(i + 3), ops[i % 8], 0x0f});
   dense.push_back(0x0b);
   add("dense", {0x7f, 0x7f}, 0x7f, dense);

   const auto load_types = std::array<std::uint8_t, 14>{
       0x7f, 0x7e, 0x7d, 0x7c, 0x7f, 0x7f, 0x7f, 0x7f, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e};
   for (unsigned i = 0; i < load_types.size(); ++i) {
      auto body = bytes{0, 0x20, 0, static_cast<std::uint8_t>(0x28 + i), 0, 3};
      signed_constant(body, 0x42, 42);
      call(body, 0);
      body.push_back(0x0b);
      add("load_" + std::to_string(i), {0x7f}, load_types[i], body);
   }
   const auto store_load = std::array<std::uint8_t, 9>{0x28, 0x29, 0x2a, 0x2b, 0x2d, 0x2f, 0x31, 0x33, 0x35};
   const auto store_type = std::array<std::uint8_t, 9>{0x7f, 0x7e, 0x7d, 0x7c, 0x7f, 0x7f, 0x7e, 0x7e, 0x7e};
   for (unsigned i = 0; i < store_load.size(); ++i) {
      auto body = bytes{0, 0x20, 0, 0x20, 1, static_cast<std::uint8_t>(0x36 + i), 0, 3,
                        0x20, 0, store_load[i], 0, 3};
      signed_constant(body, 0x42, 42);
      call(body, 0);
      body.push_back(0x0b);
      add("store_" + std::to_string(i), {0x7f, store_type[i]}, store_type[i], body);
   }
   for (const bool wide : {false, true}) {
      const auto type = static_cast<std::uint8_t>(wide ? 0x7e : 0x7f);
      const auto global = static_cast<std::uint8_t>(wide ? 1 : 0);
      add(wide ? "global64" : "global32", {type}, type,
          {0, 0x20, 0, 0x24, global, 0x23, global, 0x0b});
      const auto fp = static_cast<std::uint8_t>(wide ? 0x7c : 0x7d);
      const auto fp_global = static_cast<std::uint8_t>(wide ? 3 : 2);
      add(wide ? "float_global64" : "float_global32", {fp}, fp,
          {0, 0x20, 0, 0x24, fp_global, 0x23, fp_global, 0x0b});
      add(wide ? "float_initial64" : "float_initial32", {}, fp, {0, 0x23, fp_global, 0x0b});
      auto branch = bytes{0, 0x20, 0, 0x04, fp, 0x20, 1, 0x05, static_cast<std::uint8_t>(wide ? 0x44 : 0x43)};
      for (unsigned i = 0; i < (wide ? 8U : 4U); ++i)
         branch.push_back(i == (wide ? 7U : 3U) ? 0x80 : 0);
      branch.insert(branch.end(), {0x0b, 0x0b});
      add(wide ? "float_branch64" : "float_branch32", {0x7f, fp}, fp, branch);
      add(wide ? "zero64" : "zero32", {}, fp, {1, 1, fp, 0x20, 0, 0x0b});
   }
   const auto grow = add("grow", {0x7f}, 0x7f, {0, 0x20, 0, 0x40, 0, 0x0b});
   auto reload = bytes{0, 0x41, 0, 0x28, 2, 3, 0x1a, 0x20, 0};
   call(reload, grow);
   reload.insert(reload.end(), {0x1a, 0x41, 0, 0x28, 2, 3, 0x0b});
   add("grow_reload", {0x7f}, 0x7f, reload);
   auto new_page = bytes{0, 0x41, 0, 0x28, 2, 3, 0x1a, 0x20, 0};
   call(new_page, grow);
   new_page.push_back(0x1a);
   signed_constant(new_page, 0x41, 65536);
   new_page.insert(new_page.end(), {0x28, 2, 0, 0x0b});
   add("grow_new_page", {0x7f}, 0x7f, new_page);
   add("grow_size", {0x7f}, 0x7f, {0, 0x20, 0, 0x40, 0, 0x1a, 0x3f, 0, 0x0b});
   add("global_initial", {}, 0x7f, {0, 0x23, 0, 0x0b});
   add("global_initial64", {}, 0x7e, {0, 0x23, 1, 0x0b});
   add("benchmark_integer", {0x7e, 0x7e, 0x7e}, 0x40, benchmark_body(false));
   add("benchmark_memory", {0x7e, 0x7e, 0x7e}, 0x40, benchmark_body(true));

   auto out = bytes{0, 97, 115, 109, 1, 0, 0, 0};
   auto packed = bytes{};
   u32(packed, types.size());
   for (const auto& type : types)
      packed.insert(packed.end(), type.begin(), type.end());
   section(out, 1, packed);
   section(out, 2, {1, 3, 'e', 'n', 'v', 7, 'p', 'r', 'i', 'n', 't', 'u', 'i', 0, 0});
   packed.clear();
   u32(packed, signatures.size());
   for (const auto type : signatures)
      u32(packed, type);
   section(out, 3, packed);
   section(out, 5, {1, 1, 1, 3});
   section(out, 6, {4, 0x7f, 1, 0x41, 7, 0x0b, 0x7e, 1, 0x42, 7, 0x0b,
                    0x7d, 1, 0x43, 0x45, 0x23, 0xc1, 0x7f, 0x0b,
                    0x7c, 1, 0x44, 0x45, 0x23, 1, 0, 0, 0, 0xf8, 0x7f, 0x0b});
   packed.clear();
   u32(packed, exported.size());
   for (const auto& [field, index] : exported) {
      name(packed, field);
      packed.push_back(0);
      u32(packed, index);
   }
   section(out, 7, packed);
   packed.clear();
   u32(packed, bodies.size());
   for (const auto& body : bodies) {
      u32(packed, body.size());
      packed.insert(packed.end(), body.begin(), body.end());
   }
   section(out, 10, packed);
   section(out, 11, {1, 0, 0x41, 0, 0x0b, 12, 0xa5, 0x39, 0x12, 0x80, 0xff, 0x45, 0x23, 0x81, 0xfe, 0xdc, 0xba, 0x98});
   return out;
}
} // namespace

int main(int argc, char** argv) {
   if (argc != 2)
      return 2;
   for (const unsigned budget : {8192, 16384, 32768}) {
      const auto bytes = fixture(budget);
      auto output =
          std::ofstream(std::string(argv[1]) + "/heavy-" + std::to_string(budget) + ".wasm", std::ios::binary);
      output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      if (!output)
         throw std::runtime_error("Cannot write heavy-frame fixture");
   }
   for (const auto& [suffix, body] :
        std::vector<std::pair<std::string, bool>>{{"body", true}, {"initializer", false}}) {
      const auto bytes = fixture(8192, body, !body);
      auto output = std::ofstream(std::string(argv[1]) + "/heavy-8192-" + suffix + ".wasm", std::ios::binary);
      output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      if (!output)
         throw std::runtime_error("Cannot write AOT substitution fixture");
   }
   const auto bytes = traps();
   auto output = std::ofstream(std::string(argv[1]) + "/traps.wasm", std::ios::binary);
   output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
   if (!output)
      throw std::runtime_error("Cannot write native guest-trap fixture");
   const auto multi = std::vector<std::uint8_t>{0,    0x61, 0x73, 0x6d, 1, 0,  0, 0, 1, 6, 1,    0x60, 0,    2, 0x7f,
                                                0x7f, 3,    2,    1,    0, 10, 8, 1, 6, 0, 0x41, 1,    0x41, 2, 0x0b};
   auto rejected = std::ofstream(std::string(argv[1]) + "/multi-result.wasm", std::ios::binary);
   rejected.write(reinterpret_cast<const char*>(multi.data()), multi.size());
   if (!rejected)
      throw std::runtime_error("Cannot write multi-result rejection fixture");
   const auto oracle = operational_fixture();
   auto additional = std::ofstream(std::string(argv[1]) + "/operational-oracles.wasm", std::ios::binary);
   additional.write(reinterpret_cast<const char*>(oracle.data()), oracle.size());
   if (!additional)
      throw std::runtime_error("Cannot write operational oracle fixture");
}
