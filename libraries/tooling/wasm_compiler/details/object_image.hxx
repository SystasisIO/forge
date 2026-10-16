#pragma once

#include <cstdint>
#include <span>

namespace forge::tooling::wasm_compiler {

class object_image {
 public:
   // Parse LLVM's object output, not WASM. Resolve only checked image-local
   // ELF references and complete fixed-frame metadata before artifact creation.
   static void normalize(std::span<const std::uint8_t> object, vm::wasm::optimizing::artifact_payload& payload);
};

} // namespace forge::tooling::wasm_compiler
