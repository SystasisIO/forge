#include "../vm_wasm_execution/fixtures.hxx"
#include <array>
#include <fstream>
#include <stdexcept>

using fixtures::bytes;

int main(int argc, char** argv) {
   if (argc != 3) return 1;
   for (unsigned start = 0; start != 2; ++start) {
      bytes module{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
      fixtures::section(module, 1, {3, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 0, 0, 0x60, 0, 1, 0x7f});
      bytes imports{1};
      fixtures::text(imports, "depth");
      fixtures::text(imports, "visit");
      imports.insert(imports.end(), {0, 2});
      fixtures::section(module, 2, imports);
      fixtures::section(module, 3, {7, 0, 0, 0, 2, 2, 1, 1});
      fixtures::section(module, 4, {1, 0x70, 0, 1});
      bytes exports{8};
      const std::array names{"direct", "indirect", "host", "leaf", "trap", "loop", "start"};
      for (unsigned i = 0; i < names.size(); ++i) {
         fixtures::text(exports, names[i]);
         exports.push_back(0);
         fixtures::size(exports, i + 1);
      }
      fixtures::text(exports, "native");
      exports.insert(exports.end(), {0, 0});
      fixtures::section(module, 7, exports);
      if (start) fixtures::section(module, 8, {7});
      fixtures::section(module, 9, {1, 0, 0x41, 0, 0x0b, 1, 2});
      bytes code{7};
      const auto body = [&](bytes value) {
         fixtures::size(code, value.size());
         code.insert(code.end(), value.begin(), value.end());
      };
      // Direct and indirect calls deliberately have the same logical depth.
      body({0, 0x20, 0, 0x45, 0x04, 0x7f, 0x41, 42, 0x05,
         0x20, 0, 0x41, 1, 0x6b, 0x10, 1, 0x0b, 0x0b});
      body({0, 0x20, 0, 0x45, 0x04, 0x7f, 0x41, 42, 0x05,
         0x20, 0, 0x41, 1, 0x6b, 0x41, 0, 0x11, 0, 0, 0x0b, 0x0b});
      body({0, 0x20, 0, 0x45, 0x04, 0x7f, 0x10, 0, 0x05,
         0x20, 0, 0x41, 1, 0x6b, 0x10, 3, 0x0b, 0x0b});
      body({0, 0x41, 42, 0x0b});
      body({0, 0x00, 0x0b});
      body({0, 0x10, 0, 0x1a, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b});
      body({0, 0x41, 3, 0x10, 1, 0x1a, 0x0b});
      fixtures::section(module, 10, code);
      std::ofstream out(argv[start + 1], std::ios::binary);
      out.write(reinterpret_cast<const char*>(module.data()), module.size());
      if (!out) throw std::runtime_error("Cannot write depth fixture");
   }
}
