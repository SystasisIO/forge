module;

#include <array>
#include <cstdint>
#include <forge/exceptions/macros.hpp>
#include <forge/vm/wasm/interpret/opcode_macros.hpp>
#include <span>

module forge.vm.wasm.interpret.backend;

import forge.vm.wasm.interpret.floating_point_environment;
import forge.vm.wasm.interpret.stack_elem;
import :interpret_visitor;

namespace forge::vm::wasm::interpret {
namespace {

// Fixed scratch operands adapt the existing visitor; they own no VM, parser,
// allocator, signal checkpoint, policy or numeric implementation.
struct numeric_operands {
   std::array<operand_stack_elem, 2> slots;
   std::uint32_t count = 0;

   void inc_pc() noexcept {}
   operand_stack_elem pop_operand() {
      return slots[--count];
   }
   operand_stack_elem& peek_operand() {
      return slots[count - 1];
   }
};

template <typename Operation> constexpr bool supported_fp() {
   constexpr auto code = Operation::opcode;
   return (code >= f32_eq_t::opcode && code <= f64_ge_t::opcode) ||
          (code >= f32_abs_t::opcode && code <= f64_copysign_t::opcode) ||
          (code >= i32_trunc_s_f32_t::opcode && code <= f64_reinterpret_i64_t::opcode &&
           code != i64_extend_s_i32_t::opcode && code != i64_extend_u_i32_t::opcode);
}

template <typename Operation> constexpr value_type input_type() {
   constexpr auto code = Operation::opcode;
   if constexpr ((code >= f32_eq_t::opcode && code <= f32_ge_t::opcode) ||
                 (code >= f32_abs_t::opcode && code <= f32_copysign_t::opcode) ||
                 code == i32_trunc_s_f32_t::opcode || code == i32_trunc_u_f32_t::opcode ||
                 code == i64_trunc_s_f32_t::opcode || code == i64_trunc_u_f32_t::opcode ||
                 code == f64_promote_f32_t::opcode || code == i32_reinterpret_f32_t::opcode)
      return f32;
   else if constexpr ((code >= f64_eq_t::opcode && code <= f64_ge_t::opcode) ||
                      (code >= f64_abs_t::opcode && code <= f64_copysign_t::opcode) ||
                      code == i32_trunc_s_f64_t::opcode || code == i32_trunc_u_f64_t::opcode ||
                      code == i64_trunc_s_f64_t::opcode || code == i64_trunc_u_f64_t::opcode ||
                      code == f32_demote_f64_t::opcode || code == i64_reinterpret_f64_t::opcode)
      return f64;
   else if constexpr (code == f32_convert_s_i32_t::opcode || code == f32_convert_u_i32_t::opcode ||
                      code == f64_convert_s_i32_t::opcode || code == f64_convert_u_i32_t::opcode ||
                      code == f32_reinterpret_i32_t::opcode)
      return i32;
   else
      return i64;
}

template <typename Operation> expr_value execute(std::span<const expr_value> arguments) {
   static_assert(supported_fp<Operation>());
   constexpr auto code = Operation::opcode;
   constexpr auto binary = (code >= f32_eq_t::opcode && code <= f64_ge_t::opcode) ||
                           (code >= f32_add_t::opcode && code <= f32_copysign_t::opcode) ||
                           (code >= f64_add_t::opcode && code <= f64_copysign_t::opcode);
   constexpr auto count = binary ? 2U : 1U;
   if (arguments.size() != count)
      FORGE_THROW_EXCEPTION(exceptions::numeric_arity, "incorrect raw numeric operand count");
   // The caller has established NULL recovery before any C++/FP RAII. Do not
   // create another checkpoint or change the existing signal owner here.
   const auto environment = floating_point_environment{};
   auto operands = numeric_operands{};
   operands.count = count;
   for (unsigned i = 0; i < count; ++i) {
      if constexpr (input_type<Operation>() == f32)
         operands.slots[i] = f32_const_t{arguments[i].f32};
      else if constexpr (input_type<Operation>() == f64)
         operands.slots[i] = f64_const_t{arguments[i].f64};
      else if constexpr (input_type<Operation>() == i32)
         operands.slots[i] = i32_const_t{arguments[i].f32};
      else
         operands.slots[i] = i64_const_t{arguments[i].f64};
   }
   // This is the SAME overload used by ordinary interpreter execution, with
   // its existing private SoftFloat helpers and conversion checks unchanged.
   interpret_visitor{operands}(Operation{});
   const auto& value = operands.peek_operand();
   auto result = expr_value{};
   if (value.is_a<i32_const_t>())
      result.f32 = value.to_ui32();
   else if (value.is_a<i64_const_t>())
      result.f64 = value.to_ui64();
   else if (value.is_a<f32_const_t>())
      result.f32 = value.to_fui32();
   else
      result.f64 = value.to_fui64();
   return result;
}
} // namespace

expr_value execute_numeric(opcodes operation, std::span<const expr_value> arguments) {
   // Enumerate the existing canonical catalog, not an independent OC list.
#define FORGE_DISPATCH_NUMERIC_OPERATION(name, code) \
   case name: \
      if constexpr (supported_fp<name##_t>()) \
         return execute<name##_t>(arguments); \
      else \
         break;
   switch (operation) {
      FORGE_VM_WASM_INTERPRET_COMPARISON_OPS(FORGE_DISPATCH_NUMERIC_OPERATION)
      FORGE_VM_WASM_INTERPRET_NUMERIC_OPS(FORGE_DISPATCH_NUMERIC_OPERATION)
      FORGE_VM_WASM_INTERPRET_CONVERSION_OPS(FORGE_DISPATCH_NUMERIC_OPERATION)
   default:
      break;
   }
#undef FORGE_DISPATCH_NUMERIC_OPERATION
   FORGE_THROW_EXCEPTION(exceptions::unsupported_numeric, "operation is outside the shared FP numeric domain");
}

} // namespace forge::vm::wasm::interpret
