#include "../vm_wasm_execution/fixtures.hxx"
#include <fstream>
#include <stdexcept>

int main(int argc, char** argv) {
   if (argc != 3) return 1;
   const auto write = [](const char* path, const fixtures::bytes& bytes) {
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      if (!out) throw std::runtime_error("Cannot write binding fixture");
   };
   write(argv[1], fixtures::module());
   fixtures::bytes start{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(start, 1, {2, 0x60, 0, 1, 0x7f, 0x60, 0, 0});
   fixtures::bytes imports{1};
   fixtures::text(imports, "env");
   fixtures::text(imports, "action_data_size");
   imports.insert(imports.end(), {0, 0});
   fixtures::section(start, 2, imports);
   fixtures::section(start, 3, {1, 1});
   fixtures::section(start, 8, {1});
   fixtures::section(start, 10, {1, 5, 0, 0x10, 0, 0x1a, 0x0b});
   write(argv[2], start);
}
