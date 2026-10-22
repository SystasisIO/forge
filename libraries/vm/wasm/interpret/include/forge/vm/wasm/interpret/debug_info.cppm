module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

export module forge.vm.wasm.interpret.debug_info;

export namespace forge::vm::wasm::interpret {

struct null_debug_info {
   using builder = null_debug_info;
   void on_code_start(const void* compiled_base, const void* wasm_code_start) {}
   void on_function_start(const void* code_addr, const void* wasm_addr) {}
   void on_instr_start(const void* code_addr, const void* wasm_addr) {}
   void on_code_end(const void* code_addr, const void* wasm_addr) {}
   void set(const null_debug_info&) {}
   void relocate(const void*) {}
};

// Native observers pin the immutable full-PC map and executable owner. Stop and
// join sampling before moving, replacing or destroying the observer.
class profile_instr_map {
   struct addr_entry {
      uint32_t offset;
      uint32_t wasm_addr;
   };

 public:
   struct builder {
      void on_code_start(const void* compiled_base, const void* wasm_code_start);
      void on_function_start(const void* code_addr, const void* wasm_addr);
      void on_instr_start(const void* code_addr, const void* wasm_addr);
      void on_code_end(const void* code_addr, const void* wasm_addr);

      const void* code_base = nullptr;
      const void* wasm_base = nullptr;
      const void* code_end = nullptr;
      std::vector<addr_entry> data;
   };

   void set(builder&& value);

   profile_instr_map() = default;
   profile_instr_map(const profile_instr_map&) = delete;
   profile_instr_map& operator=(const profile_instr_map&) = delete;
   profile_instr_map(profile_instr_map&&) noexcept;
   profile_instr_map& operator=(profile_instr_map&&) noexcept;
   // Engine-neutral immutable observer. The lookup must be allocation-free and
   // signal-safe; its context is owner.get(), retained for this object's lifetime.
   profile_instr_map(std::shared_ptr<const void>, std::uint32_t (*)(const void*, std::uintptr_t) noexcept) noexcept;

   // Indicate that the executable code was moved/copied/mmapped/etc to another location
   void relocate(const void* new_base);

   // Cannot use most of the standard library as the STL is not async-signal-safe
   std::uint32_t translate(const void* pc) const noexcept;

 private:
   using native_translate = std::uint32_t (*)(const void*, std::uintptr_t) noexcept;
   std::shared_ptr<const void> _native_owner;
   const void* _native_context = nullptr;
   native_translate _native_translate = nullptr;
   const void* base_address = nullptr;
   std::size_t code_size = 0;

   addr_entry* offset_to_addr = nullptr;
   std::size_t offset_to_addr_len = 0;

   std::vector<addr_entry> data;
};

} // namespace forge::vm::wasm::interpret
