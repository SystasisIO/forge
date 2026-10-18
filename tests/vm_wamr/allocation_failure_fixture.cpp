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
void name(bytes& out, std::string_view value) {
   uleb(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}
void section(bytes& out, unsigned id, const bytes& payload) {
   out.push_back(id);
   uleb(out, payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
}
bytes fixture() {
   bytes out{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   bytes types{3};
   for (unsigned count : {16, 17, 0}) {
      types.push_back(0x60);
      uleb(types, count);
      types.insert(types.end(), count, 0x7f);
      types.insert(types.end(), {1, 0x7f});
   }
   section(out, 1, types);
   bytes imports{2};
   for (unsigned i = 0; i < 2; ++i) {
      name(imports, "oom"); name(imports, i ? "raw17" : "raw16");
      imports.push_back(0); uleb(imports, i);
   }
   section(out, 2, imports);
   section(out, 3, {2, 2, 2});
   bytes exports{4};
   unsigned index = 0;
   for (const auto text : {"raw16", "raw17", "guest16", "guest17"}) {
      name(exports, text); exports.push_back(0); uleb(exports, index++);
   }
   section(out, 7, exports);
   bytes code{2};
   for (unsigned i = 0; i < 2; ++i) {
      bytes body{0};
      for (unsigned argument = 1; argument <= 16 + i; ++argument) {
         body.push_back(0x41); uleb(body, argument);
      }
      body.push_back(0x10); uleb(body, i); body.push_back(0x0b);
      uleb(code, body.size()); code.insert(code.end(), body.begin(), body.end());
   }
   section(out, 10, code);
   return out;
}
}
int main(int argc, char** argv) {
   if (argc != 2) return 1;
   std::filesystem::create_directories(argv[1]);
   std::ofstream output(std::filesystem::path(argv[1]) / "allocation_failure.wasm", std::ios::binary);
   const auto module = fixture();
   output.write(reinterpret_cast<const char*>(module.data()), module.size());
   if (!output) throw std::runtime_error("Cannot write allocation failure fixture");
}
