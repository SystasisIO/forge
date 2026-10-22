#include "details/engine_compiler.hxx"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import forge.vm.wasm.interpret.capacity_profile;

namespace {
struct control_opcode {
   const char* name;
   std::uint8_t value;
};
constexpr std::array controls{control_opcode{"block", 0x02}, control_opcode{"loop", 0x03},
   control_opcode{"if", 0x04}};

std::vector<std::uint8_t> blocktype_fixture(std::uint8_t opcode,
   std::initializer_list<std::uint8_t> block_type, bool result = false) {
   // Type 0 is () -> (); type 1 is () -> i32. Exported run uses type 0.
   std::vector<std::uint8_t> wasm{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
      1, 8, 2, 0x60, 0, 0, 0x60, 0, 1, 0x7f,
      3, 2, 1, 0, 7, 7, 1, 3, 'r', 'u', 'n', 0, 0};
   std::vector<std::uint8_t> body{0};
   if (opcode == 0x04) body.insert(body.end(), {0x41, 1});
   body.push_back(opcode);
   body.insert(body.end(), block_type);
   if (result) {
      body.insert(body.end(), {0x41, 7});
      if (opcode == 0x04) body.insert(body.end(), {0x05, 0x41, 9});
   }
   body.push_back(0x0b);
   if (result) body.push_back(0x1a);
   body.push_back(0x0b);
   wasm.insert(wasm.end(), {10, static_cast<std::uint8_t>(body.size() + 2),
      1, static_cast<std::uint8_t>(body.size())});
   wasm.insert(wasm.end(), body.begin(), body.end());
   return wasm;
}

enum class blocktype_policy { strict_default, strict_callback_default, strict_explicit, legacy };

std::vector<std::uint8_t> check_blocktype_compile(
   const forge::vm::wasm::compiler::detail::engine_compiler& compiler,
   const std::vector<std::uint8_t>& wasm, blocktype_policy policy, bool accepted) {
   const auto original = wasm;
   bool rejected = false;
   std::vector<std::uint8_t> output;
   try {
      switch (policy) {
         case blocktype_policy::strict_default:
            output = compiler.compile(wasm, 1024, 65536);
            break;
         case blocktype_policy::strict_callback_default:
            output = compiler.compile(wasm, 1024, 65536, {});
            break;
         case blocktype_policy::strict_explicit:
            output = compiler.compile(wasm, 1024, 65536, {}, false);
            break;
         case blocktype_policy::legacy:
            output = compiler.compile(wasm, 1024, 65536, {}, true);
            break;
      }
   } catch (const std::runtime_error&) {
      rejected = true;
   }
   if (wasm != original) throw std::runtime_error("Blocktype compilation mutated caller input");
   if (rejected == accepted) throw std::runtime_error("Unexpected blocktype policy admission");
   constexpr std::array<std::uint8_t, 8> aot_header{0, 0x61, 0x6f, 0x74, 6, 0, 0, 0};
   if (accepted && (output.size() < aot_header.size() ||
       !std::equal(aot_header.begin(), aot_header.end(), output.begin())))
      throw std::runtime_error("Blocktype compilation did not emit AOT v6");
   return output;
}

void check_blocktype_policy() {
   const forge::vm::wasm::compiler::detail::engine_compiler compiler;
   for (const auto& control : controls) {
      const auto canonical = blocktype_fixture(control.value, {0x40});
      const auto zero = blocktype_fixture(control.value, {0});
      const auto overlong_zero = blocktype_fixture(control.value, {0x80, 0});
      const auto nonvoid_index = blocktype_fixture(control.value, {1}, true);
      const auto numeric = blocktype_fixture(control.value, {0x7f}, true);
      const auto reference = check_blocktype_compile(compiler, canonical, blocktype_policy::strict_default, true);
      for (const auto policy : {blocktype_policy::strict_default, blocktype_policy::strict_callback_default,
              blocktype_policy::strict_explicit, blocktype_policy::legacy}) {
         const auto compiled = check_blocktype_compile(compiler, zero, policy, policy == blocktype_policy::legacy);
         if (policy == blocktype_policy::legacy && compiled != reference)
            throw std::runtime_error("Legacy zero does not compile as canonical void");
         check_blocktype_compile(compiler, overlong_zero, policy, false);
         check_blocktype_compile(compiler, nonvoid_index, policy, false);
         check_blocktype_compile(compiler, numeric, policy, true);
      }
      // An earlier legacy compilation must not change a subsequent strict call.
      check_blocktype_compile(compiler, zero, blocktype_policy::strict_default, false);
      std::cout << "PASS " << control.name << " strict defaults, legacy void, index rejection, immutable input and reuse\n";
   }
}

void write_blocktype_fixtures(const std::filesystem::path& directory) {
   std::filesystem::create_directories(directory);
   for (const auto& control : controls) {
      const auto write = [&](const char* kind, const std::vector<std::uint8_t>& bytes) {
         const auto path = directory / (std::string(control.name) + "-" + kind + ".wasm");
         std::ofstream output(path, std::ios::binary);
         output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
         if (!output) throw std::runtime_error("Cannot write blocktype fixture");
      };
      write("void", blocktype_fixture(control.value, {0x40}));
      write("zero", blocktype_fixture(control.value, {0}));
      write("overlong-zero", blocktype_fixture(control.value, {0x80, 0}));
      write("nonvoid-index", blocktype_fixture(control.value, {1}, true));
      write("numeric", blocktype_fixture(control.value, {0x7f}, true));
   }
}

void check_capacity_profile() {
   using forge::vm::wasm::interpret::capacity_profile;
   constexpr auto representable_pages = std::numeric_limits<std::uint32_t>::max() / 65536u;
   if (capacity_profile::max_pages != representable_pages)
      throw std::runtime_error("Compiler profile exceeds its allocation callback extent");
   // A valid empty function and a memory declaration: compilation never
   // instantiates or allocates the declared guest linear memory.
   auto wasm = std::vector<std::uint8_t>{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
      1, 4, 1, 0x60, 0, 0, 3, 2, 1, 0,
      5, 8, 1, 1, 0xff, 0xff, 3, 0xff, 0xff, 3,
      10, 4, 1, 2, 0, 0x0b};
   const forge::vm::wasm::compiler::detail::engine_compiler compiler;
   bool admitted = false;
   const auto output = compiler.compile(wasm, 1024, 65536, [&](auto*) { admitted = true; });
   if (!admitted || output.empty()) throw std::runtime_error("Representable memory declaration rejected");
   auto implicit_max = wasm;
   implicit_max[19] = 5;
   implicit_max[21] = 0;
   implicit_max.erase(implicit_max.begin() + 25, implicit_max.begin() + 28);
   admitted = false;
   static_cast<void>(compiler.compile(implicit_max, 1024, 65536, [&](auto*) { admitted = true; }));
   if (!admitted) throw std::runtime_error("Implicit memory maximum was mistaken for initial allocation");
   for (const auto offset : {22u, 25u}) {
      wasm[offset] = 0x80; wasm[offset + 1] = 0x80; wasm[offset + 2] = 4;
   }
   admitted = false;
   bool rejected = false;
   try { static_cast<void>(compiler.compile(wasm, 1024, 65536, [&](auto*) { admitted = true; })); }
   catch (const std::length_error&) { rejected = true; }
   if (!rejected || admitted) throw std::runtime_error("Unrepresentable memory reached compiler code generation");
   std::cout << "PASS compiler shared page admission, without guest allocation\n";
}
}

int main(int argc, char** argv) {
   try {
      if (argc == 2 && std::string_view(argv[1]) == "--capacity-profile") {
         check_capacity_profile();
         return 0;
      }
      if (argc == 2 && std::string_view(argv[1]) == "--blocktype-policy") {
         check_blocktype_policy();
         return 0;
      }
      if (argc == 3 && std::string_view(argv[1]) == "--write-blocktype-fixtures") {
         write_blocktype_fixtures(argv[2]);
         return 0;
      }
      if (argc != 3) throw std::runtime_error("Expected input WASM and output AOT paths");
      std::ifstream input(argv[1], std::ios::binary);
      if (!input) throw std::runtime_error("Cannot read compiler fixture");
      const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
      const auto original = bytes;
      const forge::vm::wasm::compiler::detail::engine_compiler compiler;
      const auto first = compiler.compile(bytes, 4 * 1024 * 1024, 4 * 1024 * 1024);
      if (bytes != original || first.size() < 8) throw std::runtime_error("Compiler input ownership/output failed");
      bool rejected = false;
      try { compiler.compile(bytes, 1, 1024); } catch (const std::invalid_argument&) { rejected = true; }
      if (!rejected) throw std::runtime_error("Compiler ignored input limit");
      rejected = false;
      try { compiler.compile(bytes, 4 * 1024 * 1024, 1); } catch (const std::length_error&) { rejected = true; }
      if (!rejected) throw std::runtime_error("Compiler ignored output limit");
      rejected = false;
      const std::vector<std::uint8_t> malformed{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 1, 255};
      try { compiler.compile(malformed, 1024, 1024); } catch (const std::runtime_error&) { rejected = true; }
      if (!rejected) throw std::runtime_error("Compiler accepted malformed WASM");
      const auto second = compiler.compile(bytes, 4 * 1024 * 1024, 4 * 1024 * 1024);
      if (first != second) throw std::runtime_error("Compiler reuse changed emitted artifact");
      std::ofstream output(argv[2], std::ios::binary);
      output.write(reinterpret_cast<const char*>(second.data()), second.size());
      if (!output) throw std::runtime_error("Cannot write compiler artifact");
      std::cout << "PASS compiler ownership, input/output limits, rejection and reuse\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   }
}
