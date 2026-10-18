#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.backend;

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
   using namespace std::chrono_literals;
   auto wasm = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   auto backend = vm::backend<std::nullptr_t, vm::interpreter>{wasm, nullptr};
   unsigned calls = 0;
   for (unsigned iteration = 0; iteration < 32; ++iteration) {
      bool rejected = false;
      fail_next_allocation = true;
      try {
         backend.timed_run(std::chrono::steady_clock::now() + 1s, {}, [&] { ++calls; });
      } catch (const std::bad_alloc&) {
         rejected = true;
      }
      if (fail_next_allocation || !rejected || calls != iteration)
         return 1;
      backend.timed_run(std::chrono::steady_clock::now() + 1s, {}, [&] { ++calls; });
   }
   if (calls != 32)
      return 2;
   std::cout << "PASS 32 stop-state allocation failures before Work, each followed by reusable timed scope\n";
}
