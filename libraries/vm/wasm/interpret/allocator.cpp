module;

#include <cstddef>
#include <cstring>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

module forge.vm.wasm.interpret.allocator;

namespace forge::vm::wasm::interpret {

struct wasm_allocator::borrow::registration {
   std::weak_ptr<void> owner;
   void (*retire)(void*, const void*) noexcept;
};

struct wasm_allocator::state {
   class mutation;
   explicit state(wasm_allocator* value) : allocator(value) {}
   std::mutex mutex;
   std::condition_variable retired;
   wasm_allocator* allocator;
   std::shared_ptr<borrow::registration> owner;
   std::size_t active = 0;
   std::thread::id thread;
   bool mutating = false;
};

class wasm_allocator::state::mutation {
 public:
   explicit mutation(state& state) : _state(state), _lock(state.mutex) {
      detail::check<exceptions::busy>(!state.mutating && !state.active,
                                      "Allocator has an active execution or memory transition");
      detail::check<exceptions::allocation>(state.allocator != nullptr, "Allocator mapping has been released");
      auto previous = state.owner;
      detail::check<exceptions::busy>(!previous || previous->retire, "Allocator has an exclusive manual borrow");
      state.mutating = true;
      try {
         if (previous) {
            auto owner = previous->owner.lock();
            _lock.unlock();
            if (owner) {
               previous->retire(owner.get(), previous.get());
               // A last owner reference can run its destructor and unregister.
               owner.reset();
               _lock.lock();
               detail::check<exceptions::busy>(state.owner != previous, "Idle owner did not retire its instance");
            } else {
               _lock.lock();
               // The last context reference may already be in destruction.
               state.retired.wait(_lock, [&] { return state.owner != previous; });
            }
         }
      } catch (...) {
         if (!_lock.owns_lock())
            _lock.lock();
         state.mutating = false;
         state.retired.notify_all();
         throw;
      }
   }
   ~mutation() {
      _state.mutating = false;
      _state.retired.notify_all();
   }

 private:
   state& _state;
   std::unique_lock<std::mutex> _lock;
};

wasm_allocator::wasm_allocator() : _state(std::make_shared<state>(this)) {
   const auto system_page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
   const auto mapping_size = max_memory + 2 * system_page_size;
   auto* mapping = static_cast<char*>(::mmap(nullptr, mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
   detail::check<exceptions::allocation>((mapping != MAP_FAILED), "mmap failed to allocate pages");

   if (::mprotect(mapping, system_page_size, PROT_READ) != 0) {
      ::munmap(mapping, mapping_size);
      detail::fail<exceptions::allocation>("mprotect failed");
   }

   raw = mapping + system_page_size;
   page = 0;
}

wasm_allocator::~wasm_allocator() {
   free();
}

wasm_allocator::borrow::borrow(wasm_allocator& allocator) : borrow(allocator, {}, nullptr) {}

wasm_allocator::borrow::borrow(wasm_allocator& allocator, std::weak_ptr<void> owner,
                               void (*retire)(void*, const void*) noexcept)
    : _state(allocator._state), _registration(std::make_shared<registration>(std::move(owner), retire)) {
   const state::mutation transition(*_state);
   _state->owner = _registration;
}

wasm_allocator::borrow::~borrow() {
   const auto lock = std::lock_guard{_state->mutex};
   if (_state->owner == _registration)
      _state->owner.reset();
   _state->retired.notify_all();
}

const void* wasm_allocator::borrow::identity() const noexcept {
   return _registration.get();
}

wasm_allocator::borrow::activity::activity(const borrow& borrow) : _state(borrow._state) {
   const auto lock = std::lock_guard{_state->mutex};
   detail::check<exceptions::busy>(!_state->mutating && _state->owner == borrow._registration &&
                                       (!_state->active || _state->thread == std::this_thread::get_id()),
                                   "Allocator execution belongs to another owner");
   ++_state->active;
   _state->thread = std::this_thread::get_id();
}
wasm_allocator::borrow::activity::~activity() {
   const auto lock = std::lock_guard{_state->mutex};
   if (!--_state->active)
      _state->thread = {};
   _state->retired.notify_all();
}

void wasm_allocator::borrow::reset(std::uint32_t pages) {
   const auto lock = std::lock_guard{_state->mutex};
   detail::check<exceptions::busy>(!_state->mutating && _state->owner == _registration && _state->active &&
                                       _state->thread == std::this_thread::get_id(),
                                   "Initial reset requires the active memory owner");
   _state->allocator->reset_pages(pages);
}
void wasm_allocator::borrow::alloc(std::size_t pages) {
   const auto lock = std::lock_guard{_state->mutex};
   detail::check<exceptions::busy>(!_state->mutating && _state->owner == _registration && _state->active &&
                                       _state->thread == std::this_thread::get_id(),
                                   "Growth requires the active memory owner");
   _state->allocator->allocate_pages(pages);
}

void wasm_allocator::allocate_pages(std::size_t size) {
   if (!size)
      return;
   detail::check<exceptions::allocation>(page >= 0 && static_cast<std::size_t>(page) <= max_pages &&
                                             size <= max_pages - static_cast<std::size_t>(page),
                                         "Exceeded available memory pages");
   detail::check<exceptions::allocation>(
       ::mprotect(raw + page_size * page, page_size * size, PROT_READ | PROT_WRITE) == 0, "mprotect failed");
   std::memset(raw + page_size * page, 0, page_size * size);
   page += size;
}
void wasm_allocator::release_pages(std::size_t size) {
   if (!size)
      return;
   detail::check<exceptions::allocation>(page >= 0 && size <= static_cast<std::uint32_t>(page), "Freed too many pages");
   const auto next = page - size;
   detail::check<exceptions::allocation>(::mprotect(raw + page_size * next, page_size * size, PROT_NONE) == 0,
                                         "mprotect failed");
   page = next;
}
void wasm_allocator::reset_pages(std::uint32_t pages) {
   detail::check<exceptions::allocation>(raw != nullptr, "Require memory to reset");
   if (page >= 0)
      std::memset(raw, 0, page_size * page);
   else {
      const auto system_page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
      detail::check<exceptions::allocation>(::mprotect(raw - system_page, system_page, PROT_READ) == 0,
                                            "mprotect failed");
      page = 0;
   }
   if (pages > static_cast<std::uint32_t>(page))
      allocate_pages(pages - page);
   else if (pages < static_cast<std::uint32_t>(page))
      release_pages(page - pages);
}
void wasm_allocator::allocate(std::size_t size) {
   if (!size)
      return;
   const state::mutation transition(*_state);
   allocate_pages(size);
}
void wasm_allocator::release(std::size_t size) {
   if (!size)
      return;
   const state::mutation transition(*_state);
   release_pages(size);
}
void wasm_allocator::reset(std::uint32_t pages) {
   const state::mutation transition(*_state);
   reset_pages(pages);
}
void wasm_allocator::reset() {
   const state::mutation transition(*_state);
   if (page >= 0) {
      const auto system_page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
      std::memset(raw, 0, page_size * page);
      detail::check<exceptions::allocation>(
          ::mprotect(raw - system_page, page_size * page + system_page, PROT_NONE) == 0, "mprotect failed");
   }
   page = -1;
}
std::int32_t wasm_allocator::get_current_page() const {
   const auto lock = std::lock_guard{_state->mutex};
   return page;
}

void wasm_allocator::free() noexcept {
   auto lock = std::unique_lock{_state->mutex};
   if (raw == nullptr)
      return;
   lock.unlock();
   const state::mutation transition(*_state);
   const auto system_page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
   auto* mapping = raw - system_page_size;
   raw = nullptr;
   page = -1;
   _state->allocator = nullptr;
   ::munmap(mapping, max_memory + 2 * system_page_size);
}

stack_allocator::stack_allocator(std::size_t min_size) {
   if (min_size > 4 * 1024 * 1024) {
      const auto page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
      _size = ((min_size + page_size - 1) & ~(page_size - 1)) + 4 * 1024 * 1024;
      int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_STACK
      flags |= MAP_STACK;
#endif
      auto* ptr = ::mmap(nullptr, _size, PROT_READ | PROT_WRITE, flags, -1, 0);
      detail::check<exceptions::allocation>((ptr != MAP_FAILED), "failed to allocate alternate stack");
      _ptr = ptr;
   }
}

stack_allocator::stack_allocator(stack_allocator&& other) noexcept
    : _ptr(std::exchange(other._ptr, nullptr)), _size(std::exchange(other._size, 0)) {}

stack_allocator& stack_allocator::operator=(stack_allocator&& other) noexcept {
   if (this != &other) {
      release();
      _ptr = std::exchange(other._ptr, nullptr);
      _size = std::exchange(other._size, 0);
   }
   return *this;
}

stack_allocator::~stack_allocator() {
   release();
}

void stack_allocator::release() noexcept {
   if (_ptr) {
      ::munmap(_ptr, _size);
      _ptr = nullptr;
      _size = 0;
   }
}

void* stack_allocator::top() const {
   if (_ptr) {
      return static_cast<char*>(_ptr) + _size;
   }
   return nullptr;
}

contiguous_allocator::contiguous_allocator(std::size_t size) : _size(align_to_page(size)) {
   _base = static_cast<char*>(::mmap(nullptr, _size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
   detail::check<exceptions::allocation>((_base != MAP_FAILED), "mmap failed.");
}

contiguous_allocator::contiguous_allocator(contiguous_allocator&& other) noexcept
    : _offset(std::exchange(other._offset, 0)), _size(std::exchange(other._size, 0)),
      _base(std::exchange(other._base, nullptr)) {}

contiguous_allocator& contiguous_allocator::operator=(contiguous_allocator&& other) noexcept {
   if (this != &other) {
      release();
      _offset = std::exchange(other._offset, 0);
      _size = std::exchange(other._size, 0);
      _base = std::exchange(other._base, nullptr);
   }
   return *this;
}

contiguous_allocator::~contiguous_allocator() {
   release();
}

void contiguous_allocator::release() noexcept {
   if (_base) {
      ::munmap(_base, _size);
      _offset = 0;
      _size = 0;
      _base = nullptr;
   }
}

} // namespace forge::vm::wasm::interpret
