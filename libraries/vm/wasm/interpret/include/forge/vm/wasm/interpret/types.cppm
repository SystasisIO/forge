module;

#include <cstddef>
#include <cstdint>
#include <vector>

export module forge.vm.wasm.interpret.types;

export import forge.vm.wasm.interpret.allocator;
export import forge.vm.wasm.interpret.exceptions;
export import forge.vm.wasm.interpret.guarded_ptr;
export import forge.vm.wasm.interpret.opcodes;
export import forge.vm.wasm.interpret.stack_elem;
export import forge.vm.wasm.interpret.utils;
export import forge.vm.wasm.interpret.variant;
export import forge.vm.wasm.interpret.vector;

export namespace forge::vm::wasm::interpret {
enum types { i32 = 0x7f, i64 = 0x7e, f32 = 0x7d, f64 = 0x7c, anyfunc = 0x70, func = 0x60, pseudo = 0x40, ret_void };

enum external_kind { Function = 0, Table = 1, Memory = 2, Global = 3 };

typedef uint8_t value_type;
typedef uint8_t block_type;
typedef uint8_t elem_type;

struct activation_frame {
   opcode* pc;
   uint32_t last_op_index;
};

struct func_type {
   value_type form; // value for the func type constructor
   std::vector<value_type> param_types;
   uint8_t return_count;
   value_type return_type;
};

inline bool operator==(const func_type& lhs, const func_type& rhs) {
   return lhs.form == rhs.form && lhs.param_types == rhs.param_types && lhs.return_count == rhs.return_count &&
          (lhs.return_count == 0 || lhs.return_type == rhs.return_type);
}

union expr_value {
   int32_t i32;
   int64_t i64;
   uint32_t f32;
   uint64_t f64;
};

union native_value {
   native_value() = default;
   constexpr native_value(uint32_t arg) : i32(arg) {}
   constexpr native_value(uint64_t arg) : i64(arg) {}
   constexpr native_value(float arg) : f32(arg) {}
   constexpr native_value(double arg) : f64(arg) {}
   uint32_t i32;
   uint64_t i64;
   float f32;
   double f64;
};

using wasm_code = std::vector<uint8_t>;
using wasm_code_ptr = guarded_ptr<uint8_t>;
typedef std::uint32_t wasm_ptr_t;
typedef std::uint32_t wasm_size_t;

// Owned metadata shared by the public prepared-module and host-binding contracts.
struct module {
   struct jit_mod_t {
      struct jit_func_type {
         value_type form;
         std::vector<value_type> param_types;
         uint8_t return_count;
         value_type return_type;
      };
      struct jit_import_type {
         uint32_t func_t;
      };
      struct jit_import_entry {
         std::vector<uint8_t> module_str;
         std::vector<uint8_t> field_str;
         external_kind kind;
         jit_import_type type;
      };
   };
};
} // namespace forge::vm::wasm::interpret
