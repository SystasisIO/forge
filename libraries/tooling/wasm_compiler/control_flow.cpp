module;

#include <algorithm>
#include <cstdint>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <vector>

module forge.tooling.wasm_compiler.compiler;

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.optimizing.exceptions;

#include "details/control_flow.hxx"

namespace forge::tooling::wasm_compiler {
namespace {
namespace interpret = vm::wasm::interpret;
namespace exceptions = vm::wasm::optimizing::exceptions;

void require(bool condition) {
   if (!condition)
      FORGE_THROW_EXCEPTION(exceptions::invalid_module, "inconsistent canonical control flow");
}

bool integer_binary(interpret::opcodes operation) {
   return (operation >= interpret::i32_eq && operation <= interpret::i32_ge_u) ||
          (operation >= interpret::i64_eq && operation <= interpret::i64_ge_u) ||
          (operation >= interpret::i32_add && operation <= interpret::i32_rotr) ||
          (operation >= interpret::i64_add && operation <= interpret::i64_rotr);
}

bool integer_unary(interpret::opcodes operation) {
   return operation == interpret::i32_eqz || operation == interpret::i64_eqz ||
          (operation >= interpret::i32_clz && operation <= interpret::i32_popcnt) ||
          (operation >= interpret::i64_clz && operation <= interpret::i64_popcnt) ||
          operation == interpret::i32_wrap_i64 || operation == interpret::i64_extend_s_i32 ||
          operation == interpret::i64_extend_u_i32;
}
} // namespace

std::uint32_t control_flow::branch_height(std::uint32_t height, std::uint32_t pop_info) {
   const auto count = pop_info & 0x7fffffffU;
   const auto preserve = (pop_info & 0x80000000U) != 0;
   require(count <= height && (!preserve || count != 0));
   return height - count + static_cast<std::uint32_t>(preserve);
}

std::uint32_t control_flow::numeric_arity(interpret::opcodes operation) {
   if ((operation >= interpret::f32_eq && operation <= interpret::f64_ge) ||
       (operation >= interpret::f32_add && operation <= interpret::f32_copysign) ||
       (operation >= interpret::f64_add && operation <= interpret::f64_copysign))
      return 2;
   if ((operation >= interpret::f32_abs && operation <= interpret::f32_sqrt) ||
       (operation >= interpret::f64_abs && operation <= interpret::f64_sqrt) ||
       (operation >= interpret::i32_trunc_s_f32 && operation <= interpret::f64_reinterpret_i64 &&
        operation != interpret::i64_extend_s_i32 && operation != interpret::i64_extend_u_i32))
      return 1;
   return 0;
}

std::uint32_t control_flow::target(std::uint32_t module_pc) const {
   require(module_pc >= _base);
   const auto pc = module_pc - _base;
   require(pc < nodes.size() && nodes[pc].instruction);
   return pc;
}

control_flow::control_flow(const interpret::module& parsed, std::uint32_t index, std::uint32_t module_base)
    : _base(module_base) {
   require(index < parsed.code.size());
   const auto& body = parsed.code[index];
   require(body.size != 0 && body.code != nullptr);
   require(body.size <= std::numeric_limits<std::uint32_t>::max() - _base);
   nodes.resize(body.size);
   for (std::uint32_t pc = 0; pc < body.size;) {
      auto& node = nodes[pc];
      node.instruction = true;
      const auto& operation = body.code[pc];
      auto next = std::uint64_t{pc} + 1;
      if (operation.is_a<interpret::br_table_t>()) {
         const auto& table = operation.get<interpret::br_table_t>();
         // Writer-owned in-place payload slots are NOT opcode variants. Include
         // the default entry in the bound and never inspect their variant tags.
         const auto bytes = (std::uint64_t{table.size} + 1) * sizeof(interpret::br_table_t::elem_t);
         require(table.offset != 0 && bytes <= std::uint64_t{table.offset} * sizeof(interpret::opcode));
         require(table.table == reinterpret_cast<const interpret::br_table_t::elem_t*>(body.code + pc + 1));
         next += table.offset;
      }
      require(next <= body.size);
      node.next = static_cast<std::uint32_t>(next);
      pc = node.next;
   }

   // One height per instruction, rather than copying an entire type stack at
   // every PC. The canonical parser already owns operand-type validation.
   auto pending = std::vector<std::uint32_t>{};
   const auto add = [&](std::uint32_t pc, std::uint32_t height) {
      require(pc < nodes.size() && nodes[pc].instruction && height != unreachable);
      auto& node = nodes[pc];
      if (node.height == unreachable) {
         node.height = height;
         pending.push_back(pc);
      } else
         require(node.height == height);
      max_stack = std::max(max_stack, height);
   };
   add(0, 0);
   for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
      const auto pc = pending[cursor];
      const auto& node = nodes[pc];
      const auto& value = body.code[pc];
      auto height = node.height;
      const auto pop = [&](std::uint32_t count) {
         require(count <= height);
         height -= count;
      };
      const auto jump = [&](std::uint32_t destination, std::uint32_t count) {
         add(target(destination), branch_height(height, count));
      };
      const auto operation = static_cast<interpret::opcodes>(value.index());
      switch (operation) {
      case interpret::unreachable:
         continue;
      case interpret::return_:
         require(height == parsed.get_function_type(index + parsed.get_imported_functions_size()).return_count);
         continue;
      case interpret::br: {
         const auto& branch = value.get<interpret::br_t>();
         jump(branch.pc, branch.data);
         continue;
      }
      case interpret::br_if: {
         pop(1);
         const auto& branch = value.get<interpret::br_if_t>();
         jump(branch.pc, branch.data);
         break;
      }
      case interpret::if_:
         pop(1);
         add(target(value.get<interpret::if_t>().pc), height);
         break;
      case interpret::else_:
         add(target(value.get<interpret::else_t>().pc), height);
         continue;
      case interpret::br_table: {
         pop(1);
         const auto& table = value.get<interpret::br_table_t>();
         for (std::uint64_t i = 0; i <= table.size; ++i)
            jump(table.table[i].pc, table.table[i].stack_pop);
         continue;
      }
      case interpret::get_local:
      case interpret::get_global:
      case interpret::current_memory:
      case interpret::i32_const:
      case interpret::i64_const:
      case interpret::f32_const:
      case interpret::f64_const:
         require(height < unreachable - 1);
         ++height;
         break;
      case interpret::set_local:
      case interpret::set_global:
      case interpret::drop:
         pop(1);
         break;
      case interpret::tee_local:
         require(height != 0);
         break;
      case interpret::select:
         pop(3);
         ++height;
         break;
      case interpret::call: {
         const auto& type = parsed.get_function_type(value.get<interpret::call_t>().index);
         require(type.param_types.size() <= height);
         pop(static_cast<std::uint32_t>(type.param_types.size()));
         require(height < unreachable - 1);
         height += type.return_count;
         break;
      }
      case interpret::call_indirect: {
         pop(1); // Dynamic table selector is above the actual call arguments.
         const auto& type = parsed.types[value.get<interpret::call_indirect_t>().index];
         pop(static_cast<std::uint32_t>(type.param_types.size()));
         require(height < unreachable - 1);
         height += type.return_count;
         break;
      }
      case interpret::nop:
         break;
      default:
         if (operation >= interpret::i32_load && operation <= interpret::i64_load32_u)
            require(height != 0); // Address replaced by one raw loaded value.
         else if (operation >= interpret::i32_store && operation <= interpret::i64_store32)
            pop(2);
         else if (operation == interpret::grow_memory)
            require(height != 0); // Delta replaced by old pages or -1.
         else if (integer_binary(operation)) {
            pop(2);
            ++height;
         } else if (const auto arity = numeric_arity(operation)) {
            pop(arity);
            ++height;
         } else if (integer_unary(operation))
            require(height != 0);
         else
            FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable,
                                  "operation lowering is not available in this compiler slice");
      }
      add(node.next, height);
   }
}

} // namespace forge::tooling::wasm_compiler
