#pragma once

#include "fixtures.hxx"
#include <array>
#include <cstdint>
#include <vector>

namespace fixtures {
struct profile_fixture {
   fixtures::bytes wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   std::array<std::vector<std::uint32_t>, 2> opcodes;
   std::uint32_t code_section = 0;

   explicit profile_fixture(bool with_cancel_hook = false) {
      fixtures::bytes padding;
      fixtures::text(padding, "padding");
      padding.resize(300, 0);
      fixtures::section(wasm, 0, padding);
      fixtures::section(wasm, 1,
                        with_cancel_hook ? fixtures::bytes{2, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 0, 0}
                                         : fixtures::bytes{1, 0x60, 1, 0x7f, 1, 0x7f});
      if (with_cancel_hook)
         fixtures::section(wasm, 2, {1, 3, 'e', 'n', 'v', 7, 'e', 'n', 't', 'e', 'r', 'e', 'd', 0, 1});
      fixtures::section(wasm, 3, with_cancel_hook ? fixtures::bytes{3, 0, 0, 1} : fixtures::bytes{2, 0, 0});
      const auto first_function = static_cast<std::uint8_t>(with_cancel_hook);
      fixtures::bytes exports{static_cast<std::uint8_t>(with_cancel_hook ? 3 : 2)};
      fixtures::text(exports, "mix");
      exports.insert(exports.end(), {0, first_function});
      fixtures::text(exports, "other");
      exports.insert(exports.end(), {0, static_cast<std::uint8_t>(first_function + 1)});
      if (with_cancel_hook) {
         fixtures::text(exports, "cancel");
         exports.insert(exports.end(), {0, 3});
      }
      fixtures::section(wasm, 7, exports);
      fixtures::bytes code{static_cast<std::uint8_t>(with_cancel_hook ? 3 : 2)};
      for (std::uint32_t function = 0; function != 2; ++function) {
         fixtures::bytes body{1, 1, 0x7f};
         const auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
            opcodes[function].push_back(static_cast<std::uint32_t>(body.size()));
            body.insert(body.end(), bytes.begin(), bytes.end());
         };
         emit({0x41, 1});
         emit({0x21, 1});
         emit({0x03, 0x40});
         emit({0x20, 1});
         emit({0x41, static_cast<std::uint8_t>(function ? 5 : 3)});
         emit({0x6c});
         emit({0x41, static_cast<std::uint8_t>(function ? 11 : 7)});
         emit({0x73});
         emit({0x20, 0});
         emit({0x6e});
         emit({0x21, 1});
         // The second quotient depends on the first and feeds the next iteration.
         emit({0x20, 0});
         emit({0x20, 1});
         emit({0x41, 1});
         emit({0x72});
         emit({0x6e});
         emit({0x21, 1});
         emit({0x20, 0});
         emit({0x41, 1});
         emit({0x6b});
         emit({0x22, 0});
         emit({0x0d, 0});
         emit({0x0b});
         emit({0x20, 1});
         emit({0x0b});
         fixtures::size(code, body.size());
         for (auto& offset : opcodes[function])
            offset += static_cast<std::uint32_t>(code.size());
         code.insert(code.end(), body.begin(), body.end());
      }
      if (with_cancel_hook) {
         // Guest entry is observable before cancellation; the loop cannot win
         // a race by completing before the installed stop callback runs.
         const fixtures::bytes cancel{0, 0x10, 0, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b};
         fixtures::size(code, cancel.size());
         code.insert(code.end(), cancel.begin(), cancel.end());
      }
      wasm.push_back(10);
      fixtures::size(wasm, code.size());
      code_section = static_cast<std::uint32_t>(wasm.size());
      for (auto& function : opcodes)
         for (auto& offset : function)
            offset += code_section;
      wasm.insert(wasm.end(), code.begin(), code.end());
   }

   static std::uint32_t expected(std::uint32_t count, bool other = false) {
      std::uint32_t result = 1;
      do {
         result = ((result * (other ? 5U : 3U)) ^ (other ? 11U : 7U)) / count;
         result = count / (result | 1U);
      } while (--count);
      return result;
   }
};
} // namespace fixtures
