#pragma once

#include <wasm_export.h>

namespace forge::vm::wasm::interpret::detail {

class engine_exception {
 public:
   enum class reason {
      none,
      unknown,
      unreachable,
      memory,
      table_bounds,
      indirect_type,
      integer_overflow,
      integer_divide_by_zero,
      invalid_conversion,
      physical_stack,
      allocation,
      aux_stack
   };
   static reason inspect(wasm_module_inst_t) noexcept;
};

} // namespace forge::vm::wasm::interpret::detail
