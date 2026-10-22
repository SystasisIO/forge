#include "details/engine_memory.hxx"
#include "details/engine_load_budget.hxx"

#include <cstdlib>
#include <cstring>

namespace forge::vm::wasm::interpret::detail {

thread_local engine_memory* engine_memory::_current = nullptr;

engine_memory::engine_memory(void* data, allocate_function allocate, reallocate_function reallocate,
                             release_function release) noexcept
    : _data(data), _allocate(allocate), _reallocate(reallocate), _release(release) {}

engine_memory engine_memory::system() noexcept {
   return {nullptr, nullptr, nullptr, nullptr};
}

bool engine_memory::failed() const noexcept {
   return _failed;
}
bool engine_memory::current_failed() noexcept {
   return _current && _current->_failed;
}
void* engine_memory::current_base() noexcept {
   return _current ? _current->_base : nullptr;
}
void engine_memory::set_base(void* base) noexcept {
   _base = base;
}
void engine_memory::clear_failure() noexcept {
   _failed = false;
}
void engine_memory::bind_instance(wasm_module_inst_t instance) noexcept {
   _instance = instance;
}
void engine_memory::fail() noexcept {
   _failed = true;
   if (_instance)
      wasm_runtime_terminate(_instance);
}

engine_memory::scope::scope(engine_memory& owner) noexcept : _previous(_current) {
   _current = &owner;
}
engine_memory::scope::~scope() {
   _current = _previous;
}

void* engine_memory::allocate(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
    mem_alloc_usage_t usage,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void*,
#endif
    unsigned int size) noexcept {
#if WASM_MEM_ALLOC_WITH_USAGE != 0
   if (usage == Alloc_For_LinearMemory) {
      if (!_current)
         return nullptr;
      if (_current->_failed)
         return nullptr;
      if (!size) {
         _current->fail();
         return nullptr;
      }
      try {
         auto* result = _current->_allocate ? _current->_allocate(_current->_data, size) : std::calloc(1, size);
         if (!result)
            _current->fail();
         else if (!_current->_allocate) {
            _current->_base = result;
            _current->_system_bytes = size;
         }
         return result;
      } catch (...) {
         _current->fail();
         return nullptr;
      }
   }
#endif
   return engine_load_budget::allocate(size);
}

void* engine_memory::reallocate(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
    mem_alloc_usage_t usage, bool full_size_mapped,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void*,
#endif
    void* pointer, unsigned int size) noexcept {
#if WASM_MEM_ALLOC_WITH_USAGE != 0
   if (usage == Alloc_For_LinearMemory) {
      if (!_current)
         return nullptr;
      if (_current->_failed)
         return nullptr;
      if (!size) {
         _current->fail();
         return nullptr;
      }
      if (full_size_mapped) {
         _current->fail();
         return nullptr;
      }
      try {
         auto* result = _current->_reallocate ? _current->_reallocate(_current->_data, pointer, size)
                                              : std::realloc(pointer, size);
         if (!result)
            _current->fail();
         else if (!_current->_reallocate) {
            if (size > _current->_system_bytes)
               std::memset(static_cast<unsigned char*>(result) + _current->_system_bytes, 0,
                           size - _current->_system_bytes);
            _current->_base = result;
            _current->_system_bytes = size;
         }
         return result;
      } catch (...) {
         _current->fail();
         return nullptr;
      }
   }
#endif
   return engine_load_budget::reallocate(pointer, size);
}

void engine_memory::release(
#if WASM_MEM_ALLOC_WITH_USAGE != 0
    mem_alloc_usage_t usage,
#endif
#if WASM_MEM_ALLOC_WITH_USER_DATA != 0
    void*,
#endif
    void* pointer) noexcept {
#if WASM_MEM_ALLOC_WITH_USAGE != 0
   if (usage == Alloc_For_LinearMemory) {
      if (!_current)
         return;
      try {
         if (_current->_release)
            _current->_release(_current->_data, pointer);
         else {
            std::free(pointer);
            _current->_base = nullptr;
            _current->_system_bytes = 0;
         }
      } catch (...) {
         _current->fail();
      }
      return;
   }
#endif
   engine_load_budget::free(pointer);
}

} // namespace forge::vm::wasm::interpret::detail
