#include <cstdint>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
   if (argc != 2) return 1;
   // A real WASM start section, not an exported _start or __post_instantiate.
   // start: mode = probe.start(); mode == 2 traps; other nonzero modes loop.
   const std::vector<std::uint8_t> module{
      0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
      1, 8, 2, 0x60, 0, 1, 0x7f, 0x60, 0, 0,
      2, 15, 1, 5, 'p', 'r', 'o', 'b', 'e', 5, 's', 't', 'a', 'r', 't', 0, 0,
      3, 3, 2, 1, 0,
      7, 10, 1, 6, 'a', 'n', 's', 'w', 'e', 'r', 0, 2,
      8, 1, 1,
      10, 32, 2,
      25, 1, 1, 0x7f, 0x10, 0, 0x22, 0, 0x41, 2, 0x46, 0x04, 0x40, 0, 0x0b,
      0x20, 0, 0x04, 0x40, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b, 0x0b,
      4, 0, 0x41, 42, 0x0b
   };
   std::ofstream output(argv[1], std::ios::binary);
   output.write(reinterpret_cast<const char*>(module.data()), module.size());
   return output ? 0 : 1;
}
