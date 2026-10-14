#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

import forge.execution.wasm.compiled_module;

int main(int argc, char** argv) {
   if (argc != 2) return 2;
   auto input = std::ifstream{argv[1], std::ios::binary};
   if (!input) return 3;
   const auto code = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input}, {}};
   auto module = forge::execution::wasm::compiled_module{code};
   const auto bytes = std::vector<std::uint8_t>{0x90, 1, 0, 0, 0, 0, 0, 0};
   const auto result = module.invoke({.receiver = forge::chain::protocol::name{"wirevalues"},
      .action = forge::chain::protocol::name{"checkstatus"}, .data = bytes});
   if (result.data != bytes || !result.console.empty()) return 4;
   try {
      (void)module.invoke({.receiver = forge::chain::protocol::name{"wirevalues"},
         .action = forge::chain::protocol::name{"checkstatus"}, .data = {}});
      return 5;
   } catch (const forge::execution::wasm::exceptions::assertion_failed&) {
      return 0;
   }
}
