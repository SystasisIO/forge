module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

export module forge.vm.wasm.interpret.allocator;

export import forge.vm.wasm.interpret.constants;
export import forge.vm.wasm.interpret.exceptions;
export import forge.vm.wasm.interpret.scope_guard;
export import forge.vm.wasm.interpret.span;

export namespace forge::vm::wasm::interpret {
class bounded_allocator {
 public:
   bounded_allocator(size_t size) {
      mem_size = size;
      raw = std::unique_ptr<uint8_t[]>(new uint8_t[mem_size]);
   }
   template <typename T> T* alloc(size_t size = 1) {
      detail::check<exceptions::allocation>(((sizeof(T) * size) + index <= mem_size), "wasm failed to allocate native");
      T* ret = (T*)(raw.get() + index);
      index += sizeof(T) * size;
      return ret;
   }

   template <typename T> void reclaim(const T* ptr, size_t size = 0) { /* noop for now */ }

   void free() {
      detail::check<exceptions::double_free>((index > 0), "double free");
      index = 0;
   }
   void reset() {
      index = 0;
   }
   size_t mem_size;
   std::unique_ptr<uint8_t[]> raw;
   size_t index = 0;
};

// Conditionally allocates a new stack leaving enough room
// for host function and signal handler execution.  If
// the required stack size is small enough to fit in the
// regular program stack, does nothing and returns nullptr.
class stack_allocator {
 public:
   explicit stack_allocator(std::size_t min_size);
   stack_allocator(const stack_allocator&) = delete;
   stack_allocator& operator=(const stack_allocator&) = delete;
   stack_allocator(stack_allocator&& other) noexcept;
   stack_allocator& operator=(stack_allocator&& other) noexcept;
   ~stack_allocator();

   void* top() const;

 private:
   void release() noexcept;

   void* _ptr = nullptr;
   std::size_t _size = 0;
};

class contiguous_allocator {
 public:
   template <std::size_t align_amt> static constexpr size_t align_offset(size_t offset) {
      return (offset + align_amt - 1) & ~(align_amt - 1);
   }

   static std::size_t align_to_page(std::size_t offset) {
      std::size_t pagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
      return (offset + pagesize - 1) & ~(pagesize - 1);
   }

   contiguous_allocator(size_t size);
   contiguous_allocator(const contiguous_allocator&) = delete;
   contiguous_allocator& operator=(const contiguous_allocator&) = delete;
   contiguous_allocator(contiguous_allocator&& other) noexcept;
   contiguous_allocator& operator=(contiguous_allocator&& other) noexcept;
   ~contiguous_allocator();

   template <typename T> T* alloc(size_t size = 0) {
      _offset = align_offset<alignof(T)>(_offset);
      size_t aligned = (sizeof(T) * size) + _offset;
      if (aligned > _size) {
         size_t new_size = align_to_page(aligned);
         char* new_base = (char*)mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         detail::check<exceptions::allocation>((new_base != MAP_FAILED), "mmap failed.");
         std::memcpy(new_base, _base, _size);
         munmap(_base, _size);
         _size = new_size;
         _base = new_base;
      }
      T* ptr = (T*)(_base + _offset);
      _offset = aligned;
      return ptr;
   }
   template <typename T> void reclaim(const T* ptr, size_t size = 0) { /* noop for now */ }
   void free() { /* noop for now */ }

 private:
   void release() noexcept;

   size_t _offset = 0;
   size_t _size = 0;
   char* _base = nullptr;
};

template <typename T> class fixed_stack_allocator {
 private:
   T* raw = nullptr;
   size_t max_size = 0;

 public:
   template <typename U> void free() {
      munmap(raw, max_memory);
   }
   fixed_stack_allocator(size_t max_size) : max_size(max_size) {
      raw = (T*)mmap(NULL, max_memory, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      detail::check<exceptions::allocation>((raw != MAP_FAILED), "mmap failed to alloca pages");
      mprotect(raw, max_size * sizeof(T), PROT_READ | PROT_WRITE);
   }
   inline T* get_base_ptr() const {
      return raw;
   }
};

class wasm_allocator {
 private:
   char* raw = nullptr;
   int32_t page = 0;
   struct state;
   std::shared_ptr<state> _state;
   void allocate_pages(std::size_t);
   void release_pages(std::size_t);
   void reset_pages(std::uint32_t);
   void allocate(std::size_t);
   void release(std::size_t);

 public:
   class borrow;
   wasm_allocator();
   ~wasm_allocator();

   wasm_allocator(const wasm_allocator&) = delete;
   wasm_allocator& operator=(const wasm_allocator&) = delete;
   wasm_allocator(wasm_allocator&&) = delete;
   wasm_allocator& operator=(wasm_allocator&&) = delete;

   template <typename T> void alloc(size_t size = 1 /*in pages*/) {
      allocate(size);
   }
   template <typename T> void free(std::size_t size) {
      release(size);
   }
   void free() noexcept;

   // Initializes the memory controlled by the allocator.
   //
   // \post get_current_page() == new_pages
   // \post all allocated pages are zero-filled.
   void reset(uint32_t new_pages);

   // Signal no memory defined
   void reset();

   template <typename T> inline T* get_base_ptr() const {
      return reinterpret_cast<T*>(raw);
   }
   template <typename T> inline T* create_pointer(uint32_t offset) {
      detail::check<exceptions::allocation>((raw != nullptr), "require memory to create pointer");
      return reinterpret_cast<T*>(raw + offset);
   }
   int32_t get_current_page() const;
   bool is_in_region(char* p) {
      if (raw == nullptr)
         return false;
      return p >= raw && p < raw + max_memory;
   }

   span<std::byte> get_span() const {
      detail::check<exceptions::allocation>((raw != nullptr), "require memory to create span");
      const std::size_t syspagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
      return {(std::byte*)raw - syspagesize, max_memory + 2 * syspagesize};
   }
};

class wasm_allocator::borrow {
 public:
   class activity;
   explicit borrow(wasm_allocator&);
   // The retirement owner contains this borrow; its destructor unregisters it.
   borrow(wasm_allocator&, std::weak_ptr<void>, void (*retire)(void*, const void*) noexcept);
   ~borrow();
   borrow(const borrow&) = delete;
   borrow& operator=(const borrow&) = delete;
   const void* identity() const noexcept;
   void reset(std::uint32_t);
   void alloc(std::size_t);

 private:
   struct registration;
   std::shared_ptr<state> _state;
   std::shared_ptr<registration> _registration;
   friend struct wasm_allocator::state;
};

class wasm_allocator::borrow::activity {
 public:
   explicit activity(const borrow&);
   ~activity();
   activity(const activity&) = delete;
   activity& operator=(const activity&) = delete;

 private:
   std::shared_ptr<state> _state;
};
} // namespace forge::vm::wasm::interpret
