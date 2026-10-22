#pragma once

// The donor's pointer-identity fixture has no WASM memory. Its custom
// converter needs real host objects for C++ pointers and references.
char* host_function_test_memory(char* memory) {
   static char storage[65536]{};
   return memory ? memory : storage;
}

forge::vm::wasm::interpret::execution_interface
host_function_test_interface(forge::vm::wasm::interpret::execution_interface interface) {
   if (!interface.get_memory()) {
      interface.memory = host_function_test_memory(nullptr);
      interface.memory_size = 65536;
   }
   return interface;
}
