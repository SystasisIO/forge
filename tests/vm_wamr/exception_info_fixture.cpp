#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using bytes = std::vector<std::uint8_t>;
void uleb(bytes& out, std::uint32_t value) {
   do {
      const auto byte = static_cast<std::uint8_t>(value & 0x7f);
      value >>= 7;
      out.push_back(byte | (value ? 0x80 : 0));
   } while (value);
}
void sleb(bytes& out, std::int32_t value) {
   bool more;
   do {
      const auto byte = static_cast<std::uint8_t>(value & 0x7f);
      value >>= 7;
      more = !((value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40)));
      out.push_back(byte | (more ? 0x80 : 0));
   } while (more);
}
void name(bytes& out, std::string_view value) {
   uleb(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}
void section(bytes& out, unsigned id, const bytes& payload) {
   out.push_back(id);
   uleb(out, payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
}
struct function { std::string_view name; unsigned type; bytes code; };
bytes fixture(int start) {
   const std::vector<function> functions{
      {"answer", 0, {0x41, 42}},
      {"unreachable", 2, {0}},
      {"divide", 1, {0x41, 7, 0x20, 0, 0x6d}},
      {"overflow", 0, {0x41, 0x80, 0x80, 0x80, 0x80, 0x78, 0x41, 0x7f, 0x6d}},
      {"conversion", 0, {0x44, 0, 0, 0, 0, 0, 0, 0xf8, 0x7f, 0xaa}},
      {"bounds", 1, {0x20, 0, 0x28, 2, 0}},
      {"recurse", 0, {0x10, 7}},
      {"host", 2, {0x10, 0}},
      {"indirect", 1, {0x41, 7, 0x20, 0, 0x11, 1, 0}},
      {"leaf", 1, {0x20, 0}}
   };
   bytes out{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   section(out, 1, {3, 0x60, 0, 1, 0x7f, 0x60, 1, 0x7f, 1, 0x7f, 0x60, 0, 0});
   bytes imports{1};
   name(imports, "info"); name(imports, "host");
   imports.insert(imports.end(), {0, 2});
   section(out, 2, imports);
   bytes types;
   uleb(types, functions.size());
   for (const auto& item : functions) uleb(types, item.type);
   section(out, 3, types);
   section(out, 4, {1, 0x70, 0, 3});
   section(out, 5, {1, 1, 1, 2});
   bytes globals{3};
   // Only __stack_pointer is mutable.
   for (unsigned i = 0; i < 3; ++i) {
      globals.insert(globals.end(), {0x7f, static_cast<std::uint8_t>(i == 2), 0x41});
      sleb(globals, i == 1 ? 1024 : 65536);
      globals.push_back(0x0b);
   }
   section(out, 6, globals);
   bytes exports;
   uleb(exports, functions.size() + 4);
   for (unsigned i = 0; i < functions.size(); ++i) {
      name(exports, functions[i].name); exports.push_back(0); uleb(exports, i + 1);
   }
   name(exports, "memory"); exports.insert(exports.end(), {2, 0});
   unsigned index = 0;
   for (const auto value : {"__heap_base", "__data_end", "__stack_pointer"}) {
      name(exports, value); exports.push_back(3); uleb(exports, index++);
   }
   section(out, 7, exports);
   if (start >= 0) { bytes value; uleb(value, start); section(out, 8, value); }
   section(out, 9, {1, 0, 0x41, 0, 0x0b, 2, 10, 1});
   bytes code;
   uleb(code, functions.size());
   for (const auto& item : functions) {
      uleb(code, item.code.size() + 2);
      code.push_back(0);
      code.insert(code.end(), item.code.begin(), item.code.end());
      code.push_back(0x0b);
   }
   section(out, 10, code);
   return out;
}
void write(const std::filesystem::path& path, const bytes& module) {
   std::ofstream output(path, std::ios::binary);
   output.write(reinterpret_cast<const char*>(module.data()), module.size());
   if (!output) throw std::runtime_error("Cannot write exception fixture");
}
}
int main(int argc, char** argv) {
   if (argc != 2) return 1;
   const std::filesystem::path directory(argv[1]);
   std::filesystem::create_directories(directory);
   write(directory / "exception_info.wasm", fixture(-1));
   write(directory / "exception_info_start_trap.wasm", fixture(2));
   write(directory / "exception_info_start_host.wasm", fixture(8));
}
