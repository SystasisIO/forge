module;

#include <cstddef>
#include <span>
#include <type_traits>

export module forge.vm.wasm.interpret.backend;

export import forge.vm.wasm.interpret.allocator;
export import forge.vm.wasm.interpret.debug_info;
export import forge.vm.wasm.interpret.exceptions;
export import forge.vm.wasm.interpret.host_function;
export import forge.vm.wasm.interpret.options;
export import forge.vm.wasm.interpret.types;
export import forge.vm.wasm.interpret.watchdog;
export import forge.vm.wasm.interpret.prepared_module;
import :runtime_backend;

export namespace forge::vm::wasm::interpret {

// One of the existing 62 FP/comparison/conversion operations on raw slots.
// Bad arity/domain use numeric_arity/unsupported_numeric; numeric guest traps
// retain the interpreter category. Reinterpret preserves payload bits, whereas
// arithmetic NaNs follow the deterministic WAMR profile. Caller stays on the
// host stack. One fixed module is prepared lazily; each call owns a new bounded
// interpreter context. This is not a zero-allocation SoftFloat primitive.
expr_value execute_numeric(opcodes operation, std::span<const expr_value> arguments);

struct jit {
   template <typename Host> using context = runtime_context;
   static constexpr bool is_jit = true;
};

struct jit_profile : jit {};

struct interpreter {
   template <typename Host> using context = runtime_context;
   static constexpr bool is_jit = false;
};

struct null_backend {
   static constexpr bool is_jit = false;
};

template <typename T> struct maybe_unique_ptr {
   maybe_unique_ptr(T* ptr = nullptr, bool owns = true) : ptr(ptr), owns(owns) {}
   maybe_unique_ptr(const maybe_unique_ptr&) = delete;
   maybe_unique_ptr& operator=(const maybe_unique_ptr&) = delete;
   ~maybe_unique_ptr() {
      if (ptr && owns)
         delete ptr;
   }
   T& operator*() const {
      return *ptr;
   }
   T* operator->() const {
      return ptr;
   }
   T* get() const {
      return ptr;
   }
   void reset(T* value, bool ownership = true) {
      if (ptr && owns)
         delete ptr;
      ptr = value;
      owns = ownership;
   }

 private:
   T* ptr;

 public:
   bool owns;
};

template <typename HostFunctions = std::nullptr_t, typename Impl = interpreter, typename Options = default_options,
          typename DebugInfo = null_debug_info>
class backend : public runtime_backend<HostFunctions, Options, DebugInfo, std::is_same_v<Impl, null_backend>,
                                       Impl::is_jit, std::is_same_v<Impl, jit_profile>> {
   static_assert(std::is_base_of_v<interpreter, Impl> || std::is_same_v<Impl, null_backend> ||
                     std::is_same_v<Impl, jit> || std::is_same_v<Impl, jit_profile>,
                 "Custom machine-code writers are not WAMR execution backends");
   using implementation = runtime_backend<HostFunctions, Options, DebugInfo, std::is_same_v<Impl, null_backend>,
                                          Impl::is_jit, std::is_same_v<Impl, jit_profile>>;

 public:
   using implementation::implementation;
};

} // namespace forge::vm::wasm::interpret
