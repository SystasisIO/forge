#pragma once

#include "../../vm_wasm_execution/fixtures.hxx"

#include <cstdint>
#include <vector>

namespace instruction_profile_fixture {

struct image {
   fixtures::bytes wasm;
   std::vector<std::uint32_t> opcodes;
   std::vector<std::vector<std::uint32_t>> function_opcodes;
   std::uint32_t code_section_offset = 0;
};

inline image stripped(bool multiple_functions = false) {
   image result;
   result.wasm = {0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   // No name/source/DWARF section. Unrelated padding makes original-file and
   // Code-section-relative offsets observably different without another parser.
   fixtures::bytes padding;
   fixtures::text(padding, "padding");
   padding.resize(300, 0);
   fixtures::section(result.wasm, 0, padding);
   fixtures::section(result.wasm, 1, {1, 0x60, 1, 0x7f, 1, 0x7f});
   fixtures::section(result.wasm, 3, multiple_functions ? fixtures::bytes{2, 0, 0} : fixtures::bytes{1, 0});
   fixtures::bytes exports{static_cast<std::uint8_t>(multiple_functions ? 2 : 1)};
   fixtures::text(exports, "mix");
   exports.insert(exports.end(), {0, 0});
   if (multiple_functions) {
      fixtures::text(exports, "outer");
      exports.insert(exports.end(), {0, 1});
   }
   fixtures::section(result.wasm, 7, exports);

   fixtures::bytes body{1, 1, 0x7f};
   std::vector<std::uint32_t> relative;
   const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
      relative.push_back(static_cast<std::uint32_t>(body.size()));
      body.insert(body.end(), bytes.begin(), bytes.end());
   };
   emit({0x41, 1});    // i32.const 1
   emit({0x21, 1});    // local.set 1 (accumulator)
   emit({0x03, 0x40}); // loop void
   emit({0x20, 1});
   emit({0x41, 3});
   emit({0x6c}); // i32.mul: loop-carried, cannot constant fold
   emit({0x41, 7});
   emit({0x73}); // i32.xor: a second instruction in this same function
   emit({0x20, 0});
   emit({0x6e}); // i32.div_u: real checked division, input zero must trap
   emit({0x21, 1});
   emit({0x20, 0});
   emit({0x41, 1});
   emit({0x6b});
   emit({0x22, 0});
   emit({0x0d, 0}); // br_if 0
   emit({0x0b});
   emit({0x20, 1});
   emit({0x0b});

   fixtures::bytes code{static_cast<std::uint8_t>(multiple_functions ? 2 : 1)};
   fixtures::size(code, body.size());
   const auto body_offset = code.size();
   code.insert(code.end(), body.begin(), body.end());
   std::vector<std::uint32_t> outer_relative;
   if (multiple_functions) {
      // A real call boundary plus checked division keeps independently
      // attributable locations in the second guest function at opt/size 3.
      const fixtures::bytes outer{0, 0x20, 0, 0x10, 0, 0x20, 0, 0x6e, 0x0b};
      fixtures::size(code, outer.size());
      const auto outer_offset = static_cast<std::uint32_t>(code.size());
      for (const auto offset : {1U, 3U, 5U, 7U, 8U})
         outer_relative.push_back(outer_offset + offset);
      code.insert(code.end(), outer.begin(), outer.end());
   }
   result.wasm.push_back(10);
   fixtures::size(result.wasm, code.size());
   result.code_section_offset = static_cast<std::uint32_t>(result.wasm.size());
   for (const auto offset : relative)
      result.opcodes.push_back(result.code_section_offset + static_cast<std::uint32_t>(body_offset) + offset);
   result.function_opcodes.push_back(result.opcodes);
   if (multiple_functions) {
      auto& offsets = result.function_opcodes.emplace_back();
      for (const auto offset : outer_relative) {
         offsets.push_back(result.code_section_offset + offset);
         result.opcodes.push_back(result.code_section_offset + offset);
      }
   }
   result.wasm.insert(result.wasm.end(), code.begin(), code.end());
   return result;
}

inline std::uint32_t expected(std::uint32_t count) {
   std::uint32_t value = 1;
   do
      value = ((value * 3) ^ 7) / count;
   while (--count);
   return value;
}

} // namespace instruction_profile_fixture
