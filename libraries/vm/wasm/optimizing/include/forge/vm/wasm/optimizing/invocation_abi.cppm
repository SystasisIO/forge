module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module forge.vm.wasm.optimizing.invocation_abi;

import forge.vm.wasm.interpret.types;

export namespace forge::vm::wasm::optimizing {

struct invocation_abi;

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

using entry_function = void (*)(invocation_abi*, const interpret::expr_value*, interpret::expr_value*) noexcept;
// Binding ordinal is the module/name-sorted configuration.imports index,
// carried by compiled_function.import_index, never a module function index
// or a process registry ordinal.
using host_call_callback = trap_reason (*)(invocation_abi* context, std::uint32_t canonical_import_index,
                                           const interpret::expr_value* arguments,
                                           interpret::expr_value* result) noexcept;
using grow_memory_callback = std::int32_t (*)(invocation_abi*, std::uint32_t) noexcept;
// existing_opcode is the value of interpret::opcodes, not a separate OC
// operation numbering scheme. argument_count counts raw numeric operands.
// Unknown/prefixed values stay unsupported by the canonical parser/owner;
// this ABI does not introduce saturating-conversion acceptance.
using numeric_callback = trap_reason (*)(invocation_abi* context, std::uint32_t existing_opcode,
                                         std::uint32_t argument_count, const interpret::expr_value* arguments,
                                         interpret::expr_value* result) noexcept;
using poll_callback = trap_reason (*)(invocation_abi*) noexcept;
// Nonreturning contract. LLVM emits unreachable after this call. Callback
// implementation transports a trap only after all callback C++ frames unwind.
using trap_callback = void (*)(invocation_abi*, trap_reason) noexcept;

struct invocation_callbacks {
   host_call_callback host_call = nullptr;
   grow_memory_callback grow_memory = nullptr;
   numeric_callback numeric = nullptr;
   poll_callback poll = nullptr;
   trap_callback trap = nullptr;
};

struct native_function {
   entry_function entry = nullptr;
   std::uint64_t stack_bytes = 0;
   std::uint32_t type_index = 0;
   std::uint32_t reserved = 0;
};

struct table_entry {
   std::uint32_t function_index = UINT32_MAX;
   std::uint32_t type_index = UINT32_MAX;
};

// The sole native context layout. The compiler accesses byte offsets obtained
// with offsetof on this declaration; it does not maintain an LLVM struct copy.
// No STL, process-global state, exception or stop-token layout crosses this ABI.
struct invocation_abi {
   static constexpr std::uint32_t current_version = 2;

   std::uint32_t version = current_version;
   std::uint32_t size = sizeof(invocation_abi);
   std::uint8_t* memory_base = nullptr;
   std::uint64_t memory_bytes = 0;
   std::uint32_t current_pages = 0;
   std::uint32_t max_pages = 0;
   interpret::expr_value* globals = nullptr;
   std::uint32_t global_count = 0;
   std::uint32_t reserved_globals = 0;
   const table_entry* table = nullptr;
   std::uint32_t table_count = 0;
   std::uint32_t reserved_table = 0;
   const native_function* functions = nullptr;
   std::uint32_t function_count = 0;
   std::uint32_t call_depth = 0;
   std::uint32_t max_call_depth = 0;
   std::uint32_t reserved_depth = 0;
   std::uint64_t stack_remaining_bytes = 0;
   std::uintptr_t stack_low = 0;
   std::uintptr_t stack_high = 0;
   std::uintptr_t stack_guard_low = 0;
   std::uintptr_t stack_guard_high = 0;
   void* owner = nullptr;
   const invocation_callbacks* callbacks = nullptr;
   // Caller pins aligned lock-free atomic_ref backing words for the entire
   // activation. ALL host accesses use atomic_ref, never mixed plain accesses.
   // const pointers do not authorize LLVM readonly/invariant metadata.
   const std::uint64_t* epoch = nullptr;
   // The logical invocation thread writes snapshot+1 while guest is suspended.
   // Callback returns may rearm it; generated code reloads after every bridge.
   std::uint64_t epoch_deadline = 0;
   // Existing trap_reason values: none/cancelled/deadline. Sticky first-wins;
   // cancellation publishes reason then bumps shared epoch, even without tick.
   const std::uint32_t* sticky_reason = nullptr;
};

static_assert(std::is_standard_layout_v<invocation_abi> && std::is_trivially_copyable_v<invocation_abi>);
static_assert(std::is_standard_layout_v<invocation_callbacks> && std::is_trivially_copyable_v<invocation_callbacks>);
static_assert(std::is_standard_layout_v<native_function> && std::is_trivially_copyable_v<native_function>);
static_assert(std::is_standard_layout_v<table_entry> && std::is_trivially_copyable_v<table_entry>);
static_assert(sizeof(interpret::expr_value) == 8 && alignof(interpret::expr_value) == 8);
static_assert(sizeof(void*) == 8 && alignof(invocation_abi) == 8 && sizeof(invocation_abi) == 168);
static_assert(offsetof(invocation_abi, owner) == 128 && offsetof(invocation_abi, callbacks) == 136);
static_assert(offsetof(invocation_abi, epoch) == 144 && offsetof(invocation_abi, epoch_deadline) == 152 &&
              offsetof(invocation_abi, sticky_reason) == 160);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free &&
              std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::required_alignment <= 8 &&
              std::atomic_ref<std::uint32_t>::required_alignment <= 8);

} // namespace forge::vm::wasm::optimizing
