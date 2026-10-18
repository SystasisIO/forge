#include <cstdlib>
#include <iostream>
#include <new>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.prepared_module;

namespace {
thread_local bool fail_next_allocation = false;
}
void* operator new(std::size_t size) {
   if (fail_next_allocation) {
      fail_next_allocation = false;
      throw std::bad_alloc{};
   }
   if (auto* pointer = std::malloc(size ? size : 1))
      return pointer;
   throw std::bad_alloc{};
}
void operator delete(void* pointer) noexcept {
   std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
   std::free(pointer);
}

int main() {
   namespace vm = forge::vm::wasm::interpret;
   auto binding = vm::host_binding{"env", "effect", {{}, {}}, [](void*, vm::execution_interface) {}};
   auto module = vm::prepared_module{fixtures::allocator_memory(1, 2), vm::options{}, {binding}};
   auto memory = vm::wasm_allocator{};
   auto first = vm::runtime_context{module, 251};
   auto second = vm::runtime_context{module, 251};
   first.set_wasm_allocator(&memory);
   second.set_wasm_allocator(&memory);
   for (unsigned iteration = 0; iteration < 32; ++iteration) {
      first.initialize(nullptr);
      first.linear_memory()[0] = 42;
      bool rejected = false;
      fail_next_allocation = true;
      try {
         second.initialize(nullptr);
      } catch (const vm::exceptions::execution_unavailable&) {
         rejected = true;
      }
      if (fail_next_allocation || !rejected || !first.initialized() || second.initialized() ||
          first.linear_memory()[0] != 42 || memory.get_current_page() != 1)
         return 1;
      second.initialize(nullptr);
      if (first.initialized() || !second.initialized() || second.linear_memory()[0] != 0)
         return 2;
   }
   std::cout << "PASS 32 registration allocation failures preserve the prior owner and permit later handoff\n";
}
