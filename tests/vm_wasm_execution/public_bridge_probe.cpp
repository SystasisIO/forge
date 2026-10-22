#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <sys/mman.h>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.prepared_module;

namespace {
std::vector<std::uint8_t> read(const char* path) {
   auto input = std::ifstream{path, std::ios::binary};
   if (!input)
      throw std::runtime_error("Fixture could not be opened");
   return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void allocator_probe(const std::vector<std::uint8_t>& source, const std::vector<std::uint8_t>& native, bool zero) {
   namespace vm = forge::vm::wasm::interpret;
   std::uint32_t calls = 0;
   auto binding = vm::host_binding{
       "env", "effect", {{}, {}}, [](void* host, vm::execution_interface) { ++*static_cast<std::uint32_t*>(host); }};
   auto module = vm::prepared_module{
       source, native, vm::options{}, {binding}, vm::prepared_module::native_origin::trusted_compiler};
   auto memory = vm::wasm_allocator{};
   auto context = vm::runtime_context{module, 251};
   context.set_wasm_allocator(&memory);
   context.set_max_pages(zero ? 1 : 2);
   auto* base = memory.get_base_ptr<char>();
   const auto grow = [&] { return context.call(&calls, "grow", {})->to_ui32(); };
   for (unsigned repeat = 0; repeat < 3; ++repeat) {
      context.initialize(&calls);
      if (context.linear_memory() != base || memory.get_current_page() != (zero ? 0 : 1))
         throw std::runtime_error("Native allocator ownership mismatch");
      if (!zero)
         base[0] = 42;
      if (grow() != (zero ? 0 : 1) || context.linear_memory() != base || memory.get_current_page() != (zero ? 1 : 2))
         throw std::runtime_error("Native allocator growth mismatch");
      if ((!zero && base[0] != 42) || !std::all_of(base + (zero ? 0 : 65536), base + (zero ? 65536 : 131072),
                                                   [](auto value) { return value == 0; }))
         throw std::runtime_error("Native allocator growth did not zero new bytes");
      if (grow() != UINT32_MAX)
         throw std::runtime_error("Native logical growth cap ignored");
      context.initialize(&calls);
      if (!zero && base[0] != 0)
         throw std::runtime_error("Native fresh state was not zeroed");
   }
   const auto before_failure = calls;
   if (::munmap(base + (zero ? 0 : 65536), 65536) != 0)
      throw std::runtime_error("Could not inject local protection failure");
   bool failed = false;
   try {
      static_cast<void>(grow());
   } catch (const vm::exceptions::execution_unavailable&) {
      failed = true;
   }
   if (!failed || calls != before_failure)
      throw std::runtime_error("Native local failure reached a guest host effect");
   context.set_max_pages(zero ? 0 : 1);
   context.initialize(&calls);
   if (grow() != UINT32_MAX || calls != before_failure + 1)
      throw std::runtime_error("Native canonical growth failure was not preserved");
   std::cout
       << "PASS native allocator alias, zero/growth/reset, logical limit and operational failure without host effect\n";
}
} // namespace

int main(int argc, char** argv) {
   if (argc == 3 && (std::string_view{argv[1]} == "--emit-indexed" || std::string_view{argv[1]} == "--emit-allocator" ||
                     std::string_view{argv[1]} == "--emit-allocator-zero")) {
      const auto mode = std::string_view{argv[1]};
      const auto source = mode == "--emit-indexed" ? fixtures::private_table()
                                                   : fixtures::allocator_memory(mode == "--emit-allocator-zero" ? 0 : 1,
                                                                                3, false, false, true);
      auto output = std::ofstream{argv[2], std::ios::binary};
      output.write(reinterpret_cast<const char*>(source.data()), source.size());
      return output ? 0 : 1;
   }
   const bool indexed = argc == 4 && std::string_view{argv[1]} == "--indexed";
   const bool allocator =
       argc == 4 && (std::string_view{argv[1]} == "--allocator" || std::string_view{argv[1]} == "--allocator-zero");
   if (argc != 3 && !indexed && !allocator)
      return 2;
   namespace vm = forge::vm::wasm::interpret;
   try {
      if (allocator) {
         allocator_probe(read(argv[2]), read(argv[3]), std::string_view{argv[1]} == "--allocator-zero");
         return 0;
      }
      const auto source = read(argv[indexed ? 2 : 1]);
      const auto native = read(argv[indexed ? 3 : 2]);
      auto policy = vm::resolve_options(vm::default_options{});
      policy.max_call_depth = 1024;
      bool refused = false;
      try {
         vm::prepared_module untrusted{source, native, policy};
      } catch (const vm::exceptions::profile&) {
         refused = true;
      }
      if (!refused)
         throw std::runtime_error("Untrusted native artifact was accepted");
      std::vector<vm::host_binding> bindings;
      std::uint32_t calls = 0;
      if (indexed)
         bindings.push_back(
             {"env", "bump", {{vm::types::i32}, {vm::types::i32}}, [](void* host, vm::execution_interface interface) {
                 ++*static_cast<std::uint32_t*>(host);
                 const auto value = interface.pop_operand().to_ui32();
                 interface.push_operand(vm::i32_const_t{value + 1});
              }});
      auto module = vm::prepared_module{source, native, policy, std::move(bindings),
                                        vm::prepared_module::native_origin::trusted_compiler};
      module.validate();
      const auto frames = module.native_function_stack_bytes();
      const auto memory = module.accounted_module_memory();
      if (frames.empty() || module.native_code().empty() || !memory.upstream_source_bytes ||
          !memory.upstream_native_bytes)
         throw std::runtime_error("Native metadata is incomplete");
      auto context = vm::runtime_context{module, policy.max_call_depth};
      const std::array<vm::operand_stack_elem, 3> args{vm::i64_const_t{UINT64_C(0)}, vm::i64_const_t{UINT64_C(0)},
                                                       vm::i64_const_t{UINT64_C(0)}};
      for (unsigned i = 0; i < 3; ++i) {
         context.initialize(&calls);
         if (indexed) {
            if (!module.function_exports().empty())
               throw std::runtime_error("Indexed fixture unexpectedly exports functions");
            const std::array<vm::operand_stack_elem, 1> argument{vm::i32_const_t{10}};
            if (context.call_indirect(&calls, 0, argument)->to_ui32() != 52 ||
                context.call_indirect(&calls, 1, argument)->to_ui32() != 11 ||
                context.call(&calls, 1, argument)->to_ui32() != 52 ||
                context.call(&calls, 0, argument)->to_ui32() != 11)
               throw std::runtime_error("Unexported table function/import dispatch failed");
         } else if (context.call(nullptr, module.get_exported_function("apply"), args).has_value())
            throw std::runtime_error("Unexpected native return value");
         context.reset();
      }
      if (indexed && calls != 6)
         throw std::runtime_error("Unexported import invocation count mismatch");
      std::cout << "{\"native_frames\":" << frames.size() << ",\"source_buffer_bytes\":" << memory.source_buffer_bytes
                << ",\"native_buffer_bytes\":" << memory.native_buffer_bytes
                << ",\"upstream_source_bytes\":" << memory.upstream_source_bytes
                << ",\"upstream_native_bytes\":" << memory.upstream_native_bytes << "}\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
