#include <cstddef>
#include <cstdint>
#include <cstring>

import forge.vm.wasm.interpret.backend;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
   using namespace forge::vm::wasm::interpret;

   auto code = wasm_code{};
   code.resize(size);
   std::memcpy(code.data(), data, size);

   try {
      auto validator = backend<std::nullptr_t, null_backend>{code, nullptr};
      (void)validator;
   } catch (const forge::exceptions::base&) {
      // Invalid bytecode is an expected fuzz input, not a harness failure.
   }
   return 0;
}
