module;

#include <cstdint>

export module forge.vm.wasm.optimizing.invocation_abi;

export namespace forge::vm::wasm::optimizing {

// Version of the shared WAMR host binding contract, not a raw native entry ABI.
inline constexpr std::uint32_t current_host_abi_version = 3;

enum class trap_reason : std::uint32_t {
   none,
   unreachable,
   memory_bounds,
   table_bounds,
   indirect_type,
   integer_divide_by_zero,
   integer_overflow,
   invalid_conversion,
   stack_overflow,
   deadline,
   cancelled,
   host_exception,
   // Local native capacity is operational, not a canonical WASM depth trap.
   native_stack_exhausted
};

static_assert(static_cast<std::uint32_t>(trap_reason::stack_overflow) == 8 &&
              static_cast<std::uint32_t>(trap_reason::host_exception) == 11 &&
              static_cast<std::uint32_t>(trap_reason::native_stack_exhausted) == 12);

// Inspection metadata only. Invoke through executor and the shared runtime.
struct native_function {
   std::uint64_t stack_bytes = 0;
   std::uint32_t type_index = 0;
};

} // namespace forge::vm::wasm::optimizing
