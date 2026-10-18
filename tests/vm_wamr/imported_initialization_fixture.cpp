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
      auto byte = static_cast<std::uint8_t>(value & 0x7f);
      value >>= 7;
      out.push_back(byte | (value ? 0x80 : 0));
   } while (value);
}
void name(bytes& out, std::string_view value) {
   uleb(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}
void section(bytes& out, std::uint8_t id, const bytes& payload) {
   out.push_back(id);
   uleb(out, payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
}
void export_function(bytes& out, std::string_view value, unsigned index) {
   name(out, value);
   out.push_back(0);
   uleb(out, index);
}

bytes fixture(bool start, bool post, unsigned post_type = 0) {
   bytes out{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   // Types: ()->(), ()->i32, (i32)->(). Imports are deliberately not
   // exported except for __post_instantiate: start is independent of lookup.
   section(out, 1, {3, 0x60, 0, 0, 0x60, 0, 1, 0x7f, 0x60, 1, 0x7f, 0});
   const unsigned imports = unsigned(start) + unsigned(post);
   bytes imported;
   uleb(imported, imports);
   for (unsigned i = 0; i < imports; ++i) {
      name(imported, "init");
      const bool is_start = start && i == 0;
      name(imported, is_start ? "start" : "post");
      imported.push_back(0);
      uleb(imported, is_start ? 0 : post_type);
   }
   section(out, 2, imported);
   section(out, 3, {1, 1});
   bytes exported;
   uleb(exported, post ? 2 : 1);
   export_function(exported, "answer", imports);
   if (post) export_function(exported, "__post_instantiate", start ? 1 : 0);
   section(out, 7, exported);
   if (start) section(out, 8, {0});
   section(out, 10, {1, 4, 0, 0x41, 42, 0x0b});
   return out;
}

void write(const std::filesystem::path& path, const bytes& module) {
   std::ofstream output(path, std::ios::binary);
   output.write(reinterpret_cast<const char*>(module.data()), module.size());
   if (!output) throw std::runtime_error("Cannot write initialization fixture");
}
}

int main(int argc, char** argv) {
   if (argc != 2) return 1;
   const std::filesystem::path directory(argv[1]);
   std::filesystem::create_directories(directory);
   write(directory / "imported_start.wasm", fixture(true, false));
   write(directory / "imported_post.wasm", fixture(false, true));
   write(directory / "imported_order.wasm", fixture(true, true));
   write(directory / "imported_post_result.wasm", fixture(false, true, 1));
   write(directory / "imported_post_parameter.wasm", fixture(false, true, 2));
}
