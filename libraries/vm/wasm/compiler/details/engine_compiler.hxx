#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

struct WASMModuleCommon;

namespace forge::vm::wasm::compiler::detail {

// Runs only in the isolated compiler process, not alongside an execution runtime.
class engine_compiler {
 public:
   std::vector<std::uint8_t> compile(std::span<const std::uint8_t> wasm, std::uint32_t max_input_bytes,
                                     std::uint32_t max_output_bytes) const;
   // The callback observes the compiler-profile loader, only in this process.
   // Its upstream types never cross a public module or the worker protocol.
   std::vector<std::uint8_t> compile(std::span<const std::uint8_t> wasm, std::uint32_t max_input_bytes,
                                     std::uint32_t max_output_bytes,
                                     const std::function<void(WASMModuleCommon*)>& admit,
                                     bool allow_zero_blocktype = false) const;
};

} // namespace forge::vm::wasm::compiler::detail
