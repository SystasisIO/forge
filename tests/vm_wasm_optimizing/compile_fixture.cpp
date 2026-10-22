#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

import forge.vm.wasm.compiler.compiler;
import forge.vm.wasm.interpret.capacity_profile;

int main(int argc, char** argv) {
   try {
      if (argc != 4 && (argc != 5 || std::string_view(argv[4]) != "operational-oracles"))
         return 2;
      namespace oc = forge::vm::wasm::optimizing;
      namespace vm = forge::vm::wasm::interpret;
      auto input = std::ifstream(argv[1], std::ios::binary);
      if (!input)
         return 2;
      auto request = oc::compile_request{};
      request.wasm.assign(std::istreambuf_iterator<char>(input), {});
      request.configuration.target = oc::target_profile::native();
      request.configuration.limits = {8 * 1024 * 1024, 64 * 1024};
      request.configuration.execution.max_call_depth = 1024;
      request.configuration.execution.max_pages = vm::capacity_profile::max_pages;
      request.configuration.execution.max_func_local_bytes = 32768;
      request.configuration.imports = {
          {"env", "next", {{}, {vm::i64}}}, {"env", "consume", {{vm::i64}, {}}}, {"env", "mark", {{vm::i32}, {}}}};
      if (argc == 5)
         request.configuration.imports = {{"env", "printui", {{vm::i64}, {}}}};
      request.configuration = oc::canonicalize(std::move(request.configuration));
      const auto code = forge::vm::wasm::compiler::compiler{}.compile(request);
      for (const auto& [path, bytes] : std::vector<std::pair<const char*, std::vector<std::uint8_t>>>{
               {argv[2], code.encode()}, {argv[3], request.encode()}}) {
         auto output = std::ofstream(path, std::ios::binary);
         output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
         if (!output)
            return 2;
      }
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
