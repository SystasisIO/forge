#include "details/engine_exception.hxx"
#include <wasm_runtime.h>

namespace forge::vm::wasm::interpret::detail {

engine_exception::reason engine_exception::inspect(wasm_module_inst_t instance) noexcept {
   using reason = engine_exception::reason;
   const auto info = wasm_runtime_get_exception_info(instance);
   if (info.status == WASM_EXCEPTION_NONE)
      return reason::none;
   if (info.status != WASM_EXCEPTION_KNOWN)
      return reason::unknown;
   switch (info.id) {
   case EXCE_UNREACHABLE:
      return reason::unreachable;
   case EXCE_OUT_OF_MEMORY:
      return reason::allocation;
   case EXCE_OUT_OF_BOUNDS_MEMORY_ACCESS:
      return reason::memory;
   case EXCE_INTEGER_OVERFLOW:
      return reason::integer_overflow;
   case EXCE_INTEGER_DIVIDE_BY_ZERO:
      return reason::integer_divide_by_zero;
   case EXCE_INVALID_CONVERSION_TO_INTEGER:
      return reason::invalid_conversion;
   case EXCE_INVALID_FUNCTION_TYPE_INDEX:
      return reason::indirect_type;
   case EXCE_INVALID_FUNCTION_INDEX:
   case EXCE_UNDEFINED_ELEMENT:
   case EXCE_UNINITIALIZED_ELEMENT:
   case EXCE_OUT_OF_BOUNDS_TABLE_ACCESS:
      return reason::table_bounds;
   case EXCE_NATIVE_STACK_OVERFLOW:
   case EXCE_OPERAND_STACK_OVERFLOW:
      return reason::physical_stack;
   case EXCE_AUX_STACK_OVERFLOW:
   case EXCE_AUX_STACK_UNDERFLOW:
      return reason::aux_stack;
   default:
      return reason::unknown;
   }
}

} // namespace forge::vm::wasm::interpret::detail
