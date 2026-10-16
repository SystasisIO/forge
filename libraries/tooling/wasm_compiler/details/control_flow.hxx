#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace forge::tooling::wasm_compiler {

// Traverses already validated Forge opcodes, not WASM bytes. Heights describe
// temporary operands only: parameters/locals have separate fixed slots.
class control_flow {
 public:
   static constexpr std::uint32_t unreachable = std::numeric_limits<std::uint32_t>::max();
   struct node {
      std::uint32_t next = 0;
      std::uint32_t height = unreachable;
      bool instruction = false;
   };

   control_flow(const vm::wasm::interpret::module& parsed, std::uint32_t index, std::uint32_t module_base);
   std::uint32_t target(std::uint32_t module_pc) const;
   static std::uint32_t branch_height(std::uint32_t height, std::uint32_t pop_info);
   // Shape of existing FP/conversion operations; zero means outside this
   // callback domain. Shared by the height analysis and raw ABI lowering.
   static std::uint32_t numeric_arity(vm::wasm::interpret::opcodes operation);

   std::vector<node> nodes;
   std::uint32_t max_stack = 0;

 private:
   std::uint32_t _base = 0;
};

} // namespace forge::tooling::wasm_compiler
