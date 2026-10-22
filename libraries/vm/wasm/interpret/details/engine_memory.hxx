#pragma once

#include <cstdint>
#include <wasm_export.h>

namespace forge::vm::wasm::interpret::detail {

class engine_memory {
 public:
   using allocate_function = void* (*)(void*, std::uint32_t);
   using reallocate_function = void* (*)(void*, void*, std::uint32_t);
   using release_function = void (*)(void*, void*);
   class scope;

   engine_memory(void*, allocate_function, reallocate_function, release_function) noexcept;
   static engine_memory system() noexcept;
   engine_memory(const engine_memory&) = delete;
   engine_memory& operator=(const engine_memory&) = delete;
   bool failed() const noexcept;
   static bool current_failed() noexcept;
   static void* current_base() noexcept;
   void set_base(void*) noexcept;
   void clear_failure() noexcept;
   void bind_instance(wasm_module_inst_t) noexcept;

   static void* allocate(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
       mem_alloc_usage_t,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
       void*,
#endif
       unsigned int) noexcept;
   static void* reallocate(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
       mem_alloc_usage_t, bool,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
       void*,
#endif
       void*, unsigned int) noexcept;
   static void release(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
       mem_alloc_usage_t,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
       void*,
#endif
       void*) noexcept;

 private:
   static thread_local engine_memory* _current;
   void fail() noexcept;
   void* _data;
   allocate_function _allocate;
   reallocate_function _reallocate;
   release_function _release;
   bool _failed = false;
   wasm_module_inst_t _instance = nullptr;
   void* _base = nullptr;
   std::uint32_t _system_bytes = 0;
};

class engine_memory::scope {
 public:
   explicit scope(engine_memory&) noexcept;
   ~scope();
   scope(const scope&) = delete;
   scope& operator=(const scope&) = delete;

 private:
   engine_memory* _previous;
};

} // namespace forge::vm::wasm::interpret::detail
