module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <forge/exceptions/macros.hpp>
#include <forge/vm/wasm/interpret/opcode_macros.hpp>
#include <optional>
#include <span>
#include <vector>

module forge.vm.wasm.interpret.backend;

namespace forge::vm::wasm::interpret {
namespace {

struct numeric_operation {
   opcodes operation;
   std::uint8_t wire;
   value_type input;
   value_type output;
   std::uint8_t arity;
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
                 (code >= f32_abs_t::opcode && code <= f32_copysign_t::opcode) || code == i32_trunc_s_f32_t::opcode ||
                 code == i32_trunc_u_f32_t::opcode || code == i64_trunc_s_f32_t::opcode ||
                 code == i64_trunc_u_f32_t::opcode || code == f64_promote_f32_t::opcode ||
                 code == i32_reinterpret_f32_t::opcode)
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

template <typename Operation> constexpr value_type output_type() {
   constexpr auto code = Operation::opcode;
   if constexpr ((code >= f32_eq_t::opcode && code <= f64_ge_t::opcode) ||
                 (code >= i32_trunc_s_f32_t::opcode && code <= i32_trunc_u_f64_t::opcode) ||
                 code == i32_reinterpret_f32_t::opcode)
      return i32;
   else if constexpr ((code >= i64_trunc_s_f32_t::opcode && code <= i64_trunc_u_f64_t::opcode) ||
                      code == i64_reinterpret_f64_t::opcode)
      return i64;
   else if constexpr ((code >= f32_abs_t::opcode && code <= f32_copysign_t::opcode) ||
                      (code >= f32_convert_s_i32_t::opcode && code <= f32_demote_f64_t::opcode) ||
                      code == f32_reinterpret_i32_t::opcode)
      return f32;
   else
      return f64;
}

template <typename Operation> constexpr std::optional<numeric_operation> describe(opcodes operation) {
   if constexpr (!supported_fp<Operation>()) {
      return {};
   } else {
      constexpr auto code = Operation::opcode;
      constexpr auto binary = (code >= f32_eq_t::opcode && code <= f64_ge_t::opcode) ||
                              (code >= f32_add_t::opcode && code <= f32_copysign_t::opcode) ||
                              (code >= f64_add_t::opcode && code <= f64_copysign_t::opcode);
      return numeric_operation{operation, code, input_type<Operation>(), output_type<Operation>(), binary ? 2U : 1U};
   }
}

constexpr auto catalog = std::array{
#define FORGE_DESCRIBE_NUMERIC(name, code) describe<name##_t>(name),
    FORGE_VM_WASM_INTERPRET_COMPARISON_OPS(FORGE_DESCRIBE_NUMERIC)
        FORGE_VM_WASM_INTERPRET_NUMERIC_OPS(FORGE_DESCRIBE_NUMERIC)
            FORGE_VM_WASM_INTERPRET_CONVERSION_OPS(FORGE_DESCRIBE_NUMERIC)
#undef FORGE_DESCRIBE_NUMERIC
};
static_assert(std::count_if(catalog.begin(), catalog.end(), [](const auto& entry) { return entry.has_value(); }) == 62);
constexpr auto operations = [] {
   auto result = std::array<numeric_operation, 62>{};
   auto index = std::size_t{0};
   for (const auto& entry : catalog)
      if (entry)
         result[index++] = *entry;
   return result;
}();

// 36 unary and 26 binary functions: type payload 337, function payload 63,
// code payload 425, plus the header and section envelopes. No runtime encoder.
constexpr auto numeric_wasm = [] {
   auto code = std::array<std::uint8_t, 841>{};
   auto position = std::size_t{0};
   const auto byte = [&](std::uint8_t value) { code[position++] = value; };
   const auto u32 = [&](std::uint32_t value) {
      do {
         const auto low = static_cast<std::uint8_t>(value & 0x7f);
         value >>= 7;
         byte(low | (value ? 0x80 : 0));
      } while (value);
   };
   for (const auto value : std::array<std::uint8_t, 8>{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0})
      byte(value);
   byte(1);
   u32(337);
   u32(operations.size());
   for (const auto& entry : operations) {
      byte(0x60);
      byte(entry.arity);
      for (unsigned argument = 0; argument < entry.arity; ++argument)
         byte(entry.input);
      byte(1);
      byte(entry.output);
   }
   byte(3);
   u32(63);
   u32(operations.size());
   for (std::uint32_t index = 0; index < operations.size(); ++index)
      u32(index);
   byte(10);
   u32(425);
   u32(operations.size());
   for (const auto& entry : operations) {
      byte(3 + 2 * entry.arity);
      byte(0);
      for (std::uint8_t argument = 0; argument < entry.arity; ++argument) {
         byte(0x20);
         byte(argument);
      }
      byte(entry.wire);
      byte(0x0b);
   }
   if (position != code.size())
      throw "fixed numeric module size mismatch";
   return code;
}();
static_assert(numeric_wasm.size() < 64 * 1024 && numeric_wasm.size() <= capacity_profile::max_module_bytes);

constexpr auto numeric_options = [] {
   auto policy = options{};
   policy.max_call_depth = 1;
   policy.max_pages = 0;
   policy.max_code_bytes = 7;
   policy.max_func_local_bytes = 32;
   policy.max_func_local_bytes_flags = max_func_local_bytes_flags_t::params | max_func_local_bytes_flags_t::locals |
                                       max_func_local_bytes_flags_t::stack;
   return policy;
}();

prepared_module& numeric_program() {
   // Exactly one fixed program, never a keyed cache. The existing context API
   // borrows a non-const module; this adapter never mutates the prepared module.
   static auto program = prepared_module{numeric_wasm, numeric_options, std::vector<host_binding>{},
                                         prepared_module::purpose::execution, execution_mode::interpreter};
   return program;
}

template <typename Unsigned> Unsigned read_slot(const expr_value& value) noexcept {
   // A valid 32-bit slot need not initialize the union's upper four bytes.
   Unsigned bits;
   std::memcpy(&bits, &value, sizeof(bits));
   return bits;
}

} // namespace

expr_value execute_numeric(opcodes operation, std::span<const expr_value> arguments) {
   const auto found = std::find_if(operations.begin(), operations.end(),
                                   [&](const auto& entry) { return entry.operation == operation; });
   if (found == operations.end())
      FORGE_THROW_EXCEPTION(exceptions::unsupported_numeric, "operation is outside the shared FP numeric domain");
   if (arguments.size() != found->arity)
      FORGE_THROW_EXCEPTION(exceptions::numeric_arity, "incorrect raw numeric operand count");

   // WAMR owns native FP canonicalization/restoration. This path does not use
   // SoftFloat or change its caller-owned state.
   auto operands = std::array<operand_stack_elem, 2>{};
   for (std::size_t index = 0; index < arguments.size(); ++index) {
      switch (found->input) {
      case i32:
         operands[index] = i32_const_t{read_slot<std::uint32_t>(arguments[index])};
         break;
      case i64:
         operands[index] = i64_const_t{read_slot<std::uint64_t>(arguments[index])};
         break;
      case f32:
         operands[index] = f32_const_t{read_slot<std::uint32_t>(arguments[index])};
         break;
      case f64:
         operands[index] = f64_const_t{read_slot<std::uint64_t>(arguments[index])};
         break;
      }
   }
   auto context = prepared_module::context{numeric_program(), 1, execution_mode::interpreter};
   context.initialize(nullptr);
   const auto value = context.call(nullptr, static_cast<std::uint32_t>(found - operations.begin()),
                                   std::span{operands}.first(found->arity));
   if (!value)
      FORGE_THROW_EXCEPTION(exceptions::interpreter, "numeric operation returned no value");
   auto result = std::uint64_t{0};
   switch (found->output) {
   case i32:
      result = value->to_ui32();
      break;
   case i64:
      result = value->to_ui64();
      break;
   case f32:
      result = value->to_fui32();
      break;
   case f64:
      result = value->to_fui64();
      break;
   }
   return std::bit_cast<expr_value>(result);
}

} // namespace forge::vm::wasm::interpret
