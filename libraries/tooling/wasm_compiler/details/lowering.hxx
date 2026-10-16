#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace forge::tooling::wasm_compiler {

class lowering {
 public:
   static std::vector<std::uint8_t> emit(const vm::wasm::interpret::module& parsed,
                                       const vm::wasm::optimizing::artifact_payload& metadata,
                                       std::span<const std::uint32_t> canonical_types,
                                       std::string* optimized_ir = nullptr);
};

} // namespace forge::tooling::wasm_compiler
