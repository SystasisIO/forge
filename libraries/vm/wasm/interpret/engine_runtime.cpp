#include "details/engine_runtime.hxx"
#include "details/engine_memory.hxx"
#include "details/engine_load_budget.hxx"

#include <algorithm>
#include <stdexcept>
#include <wasm_export.h>

namespace forge::vm::wasm::interpret::detail {

std::mutex engine_runtime::_initialization_mutex;
std::weak_ptr<engine_runtime> engine_runtime::_initialized;

engine_runtime::engine_runtime() {
   RuntimeInitArgs args{};
   args.mem_alloc_type = Alloc_With_Allocator;
   args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(&engine_memory::allocate);
   args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(&engine_memory::reallocate);
   args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(&engine_memory::release);
   args.running_mode = Mode_Interp;
#if WASM_ENABLE_JIT != 0
   args.llvm_jit_opt_level = 3;
   args.llvm_jit_size_level = 3;
#endif
   if (!wasm_runtime_full_init(&args)) {
      throw std::runtime_error("WASM engine initialization failed");
   }
}

engine_runtime::~engine_runtime() {
   {
      const auto lock = std::lock_guard{_initialization_mutex};
      _initialized.reset();
   }
   retry_cleanup(pending_cleanup());
   wasm_runtime_destroy();
}

engine_runtime::cleanup_report engine_runtime::retry_pending_cleanup(std::size_t max_owners) noexcept {
   std::shared_ptr<engine_runtime> runtime;
   {
      const auto lock = std::lock_guard{_initialization_mutex};
      runtime = _initialized.lock();
   }
   if (runtime)
      return runtime->retry_cleanup(max_owners);
   return {0, 0};
}

void engine_runtime::retain_cleanup(const std::shared_ptr<engine_load_budget>& owner) noexcept {
   const auto maps = std::lock_guard{owner->_mapping_mutex};
   if (!owner->cleanup_pending_locked())
      return;
   const auto lock = std::lock_guard{_cleanup_mutex};
   if (owner->_cleanup_registered) {
      owner->_cleanup_again = true;
      return;
   }
   owner->_cleanup_registered = true;
   owner->_cleanup_runtime = weak_from_this();
   owner->_cleanup_owner = owner;
   owner->_cleanup_next = nullptr;
   if (_cleanup_tail)
      _cleanup_tail->_cleanup_next = owner.get();
   else
      _cleanup_head = owner.get();
   _cleanup_tail = owner.get();
   ++_cleanup_count;
}

void engine_runtime::retire_cleanup(engine_load_budget* owner) noexcept {
   std::shared_ptr<engine_load_budget> retired;
   {
      // Revalidate while preventing a new failed unmap from racing retirement.
      const auto maps = std::lock_guard{owner->_mapping_mutex};
      if (owner->cleanup_pending_locked())
         return;
      const auto lock = std::lock_guard{_cleanup_mutex};
      if (!owner->_cleanup_registered || owner->_cleanup_running)
         return;
      auto** link = &_cleanup_head;
      auto* previous = static_cast<engine_load_budget*>(nullptr);
      while (*link && *link != owner) {
         previous = *link;
         link = &(*link)->_cleanup_next;
      }
      if (!*link)
         return;
      *link = owner->_cleanup_next;
      if (_cleanup_tail == owner)
         _cleanup_tail = previous;
      owner->_cleanup_next = nullptr;
      owner->_cleanup_registered = false;
      owner->_cleanup_runtime.reset();
      retired = std::move(owner->_cleanup_owner);
      --_cleanup_count;
   }
}

engine_runtime::cleanup_report engine_runtime::retry_cleanup(std::size_t max_owners) noexcept {
   auto report = cleanup_report{0, 0};
   std::size_t attempts;
   {
      const auto lock = std::lock_guard{_cleanup_mutex};
      attempts = std::min(max_owners, _cleanup_count);
   }
   while (report.attempted < attempts) {
      std::shared_ptr<engine_load_budget> owner;
      {
         const auto lock = std::lock_guard{_cleanup_mutex};
         if (!_cleanup_head)
            break;
         owner = _cleanup_head->_cleanup_owner;
         _cleanup_head = owner->_cleanup_next;
         if (!_cleanup_head)
            _cleanup_tail = nullptr;
         owner->_cleanup_next = nullptr;
         owner->_cleanup_again = false;
         owner->_cleanup_running = true;
      }
      // Native syscalls and physical frees never hold the runtime list lock.
      const auto complete = owner->retry_cleanup();
      std::shared_ptr<engine_load_budget> retired;
      {
         const auto maps = std::lock_guard{owner->_mapping_mutex};
         const auto lock = std::lock_guard{_cleanup_mutex};
         owner->_cleanup_running = false;
         if (complete && !owner->_cleanup_again && !owner->cleanup_pending_locked()) {
            owner->_cleanup_registered = false;
            owner->_cleanup_runtime.reset();
            retired = std::move(owner->_cleanup_owner);
            --_cleanup_count;
         } else {
            if (_cleanup_tail)
               _cleanup_tail->_cleanup_next = owner.get();
            else
               _cleanup_head = owner.get();
            _cleanup_tail = owner.get();
         }
      }
      ++report.attempted;
   }
   report.remaining = pending_cleanup();
   return report;
}

std::size_t engine_runtime::pending_cleanup() const noexcept {
   const auto lock = std::lock_guard{_cleanup_mutex};
   return _cleanup_count;
}

std::shared_ptr<engine_runtime> engine_runtime::acquire() {
   // WAMR has process-global registries. Instances do not initialize or destroy them.
   static auto runtime = [] {
      auto result = std::shared_ptr<engine_runtime>(new engine_runtime);
      const auto lock = std::lock_guard{_initialization_mutex};
      _initialized = result;
      return result;
   }();
   return runtime;
}

} // namespace forge::vm::wasm::interpret::detail
