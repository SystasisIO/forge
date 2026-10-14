module;

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

class jit_allocator {
   static constexpr std::size_t segment_size = std::size_t{1024u} * 1024u * 1024u;

 public:
   // allocates page aligned memory with executable permission
   void* alloc(std::size_t size) {
      std::lock_guard l{_mutex};
      size = round_to_page(size);
      auto best = free_blocks_by_size.lower_bound(size);
      if (best == free_blocks_by_size.end()) {
         best = allocate_segment(size);
      }
      if (best->first > size) {
         best = split_block(best, size);
      }
      transfer_node(free_blocks, allocated_blocks, best->second);
      best = transfer_node(free_blocks_by_size, allocated_blocks_by_size, *best);
      return best->second;
   }
   // ptr must be previously allocated by a call to alloc
   void free(void* ptr) noexcept {
      std::lock_guard l{_mutex};
      auto pos = transfer_node(allocated_blocks, free_blocks, ptr);
      transfer_node(allocated_blocks_by_size, free_blocks_by_size, {pos->second, pos->first});

      // merge the freed block with adjacent free blocks
      if (pos != free_blocks.begin()) {
         auto prev = pos;
         --prev;
         pos = maybe_consolidate_blocks(prev, pos);
      }
      auto next = pos;
      ++next;
      if (next != free_blocks.end()) {
         maybe_consolidate_blocks(pos, next);
      }
   }
   static jit_allocator& instance() {
      static jit_allocator the_jit_allocator;
      return the_jit_allocator;
   }

 private:
   struct segment {
      segment(void* base, std::size_t size) : base(base), size(size) {}
      segment(segment&& other) : base(other.base), size(other.size) {
         other.base = nullptr;
         other.size = 0;
      }
      segment& operator=(const segment& other) = delete;
      ~segment() {
         if (base) {
            ::munmap(base, size);
         }
      }
      void* base;
      std::size_t size;
   };
   using block = std::pair<std::size_t, void*>;
   struct by_size {
      using is_transparent = void;
      bool operator()(const block& lhs, const block& rhs) const {
         return lhs.first < rhs.first || (lhs.first == rhs.first && std::less<void*>{}(lhs.second, rhs.second));
      }
      bool operator()(const block& lhs, std::size_t rhs) const {
         return lhs.first < rhs;
      }
      bool operator()(std::size_t lhs, const block& rhs) const {
         return lhs < rhs.first;
      }
   };
   std::vector<segment> _segments;
   std::set<block, by_size> free_blocks_by_size;
   std::set<block, by_size> allocated_blocks_by_size;
   std::map<void*, std::size_t> free_blocks;
   std::map<void*, std::size_t> allocated_blocks;
   std::mutex _mutex;
   using blocks_by_size_t = std::set<block, by_size>;
   using blocks_t = std::map<void*, size_t>;

   // moves an element from one associative container to another
   // @pre key must be present in from, but not in to
   template <typename C> static typename C::iterator transfer_node(C& from, C& to, typename C::key_type key) noexcept {
      auto node = from.extract(key);
      assert(node);
      auto [pos, inserted, _] = to.insert(std::move(node));
      assert(inserted);
      return pos;
   }

   blocks_by_size_t::iterator allocate_segment(std::size_t min_size) {
      std::size_t size = std::max(min_size, segment_size);
      // To avoid additional memory mappings being created during permission changes of
      // from PROT_EXEC to PROT_READ | PROT_WRITE, and back to PROT_EXEC,
      // set permisions to PROT_READ | PROT_WRITE initially.
      // The permission will be changed to PROT_EXEC after executible code is copied.
      void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      detail::check<exceptions::allocation>((base != MAP_FAILED), "failed to allocate jit segment");
      segment s{base, size};
      _segments.emplace_back(std::move(s));
      bool success = false;
      auto guard_1 = scope_guard{[&] {
         if (!success) {
            _segments.pop_back();
         }
      }};
      auto pos2 = free_blocks_by_size.insert({size, base}).first;
      auto guard_2 = scope_guard{[&] {
         if (!success) {
            free_blocks_by_size.erase(pos2);
         }
      }};
      free_blocks.insert({base, size});
      success = true;
      return pos2;
   }

   blocks_by_size_t::iterator split_block(blocks_by_size_t::iterator pos, std::size_t size) {
      bool success = false;
      auto new1 = free_blocks_by_size.insert({size, pos->second}).first;
      auto guard1 = scope_guard{[&] {
         if (!success) {
            free_blocks_by_size.erase(new1);
         }
      }};
      auto new2 = free_blocks_by_size.insert({pos->first - size, static_cast<char*>(pos->second) + size}).first;
      auto guard2 = scope_guard{[&] {
         if (!success) {
            free_blocks_by_size.erase(new2);
         }
      }};
      free_blocks.insert({new2->second, new2->first});
      // the rest is nothrow
      free_blocks_by_size.erase(pos);
      free_blocks[new1->second] = new1->first;
      success = true;
      return new1;
   }

   blocks_t::iterator maybe_consolidate_blocks(blocks_t::iterator lhs, blocks_t::iterator rhs) noexcept {
      if (static_cast<char*>(lhs->first) + lhs->second == rhs->first) {
         // merge blocks in free_blocks_by_size
         auto node = free_blocks_by_size.extract({lhs->second, lhs->first});
         assert(node);
         node.value().first += rhs->second;
         free_blocks_by_size.insert(std::move(node));
         free_blocks_by_size.erase({rhs->second, rhs->first});
         // merge the blocks in free_blocks
         lhs->second += rhs->second;
         free_blocks.erase(rhs);
         return lhs;
      } else {
         return rhs;
      }
   }

   static std::size_t round_to_page(std::size_t offset) {
      std::size_t pagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
      return (offset + pagesize - 1) & ~(pagesize - 1);
   }
};

class growable_allocator {
 public:
   growable_allocator(const growable_allocator&) = delete;
   growable_allocator& operator=(const growable_allocator&) = delete;
   growable_allocator(growable_allocator&&) = delete;
   growable_allocator& operator=(growable_allocator&&) = delete;

   static constexpr size_t max_memory_size = 1024 * 1024 * 1024; // 1GB
   template <std::size_t align_amt> static constexpr size_t align_offset(size_t offset) {
      return (offset + align_amt - 1) & ~(align_amt - 1);
   }

   static std::size_t align_to_page(std::size_t offset) {
      std::size_t pagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
      assert(max_memory_size % page_size == 0);
      return (offset + pagesize - 1) & ~(pagesize - 1);
   }

   growable_allocator() {}

   // size in bytes
   explicit growable_allocator(size_t size) {
      detail::check<exceptions::allocation>((size <= max_memory_size), "Too large initial memory size");
      use_default_memory();
   }

   void use_default_memory() {
      detail::check<exceptions::allocation>((_base == nullptr), "default memory already allocated");

      // uses mmap for big memory allocation
      _base = (char*)mmap(NULL, max_memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      detail::check<exceptions::allocation>((_base != MAP_FAILED), "failed to mmap for default memory.");
      _capacity = max_memory_size;
   }

   // size in bytes
   void use_fixed_memory(size_t size) {
      detail::check<exceptions::allocation>((0 < size && size <= max_memory_size), "Too large or 0 fixed memory size");
      detail::check<exceptions::allocation>((_base == nullptr), "Fixed memory already allocated");

      _base = (char*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      detail::check<exceptions::allocation>((_base != MAP_FAILED), "mmap in use_fixed_memory failed.");
      _capacity = size;
   }

   ~growable_allocator() {
      if (_base != nullptr) {
         munmap(_base, _capacity);
      }
      if (_is_jit && _code_base) {
         jit_allocator::instance().free(_code_base);
      }
   }

   // TODO use Outcome library
   template <typename T> T* alloc(size_t size = 0) {
      static_assert(max_memory_size % alignof(T) == 0, "alignment must divide max_memory_size.");
      if (size == 0)
         return nullptr;

      detail::check<exceptions::allocation>((_capacity % alignof(T) == 0), "alignment must divide _capacity.");
      _offset = align_offset<alignof(T)>(_offset);
      // Evaluating the inequality in this form cannot cause integer overflow.
      // Once this assertion passes, the rest of the function is safe.
      detail::check<exceptions::allocation>(((_capacity - _offset) / sizeof(T) >= size), "Allocated too much memory");
      size_t aligned = (sizeof(T) * size) + _offset;
      detail::check<exceptions::allocation>((aligned <= _capacity), "Allocated too much memory after aligned");

      T* ptr = (T*)(_base + _offset);
      _offset = aligned;
      if (_offset > _largest_offset) {
         _largest_offset = _offset;
      }
      return ptr;
   }

   void* start_code() {
      _offset = align_to_page(_offset);
      return _base + _offset;
   }
   template <bool IsJit> void end_code(void* code_base) {
      assert((char*)code_base >= _base);
      assert((char*)code_base <= (_base + _offset));
      _offset = align_to_page(_offset);
      _code_base = (char*)code_base;
      _code_size = _offset - ((char*)code_base - _base);
      if constexpr (IsJit) {
         auto& jit_alloc = jit_allocator::instance();
         _code_base = static_cast<char*>(jit_alloc.alloc(_code_size));
         // Own the JIT block before either protection transition can fail.
         _is_jit = true;
         int err = mprotect(_code_base, _code_size, PROT_READ | PROT_WRITE);
         detail::check<exceptions::allocation>((err == 0), "mprotect failed");
         std::memcpy(_code_base, code_base, _code_size);
         enable_code(IsJit);
         _offset = (char*)code_base - _base;
      }
   }

   // Sets protection on code pages to allow them to be executed.
   void enable_code(bool is_jit) {
      const auto error = mprotect(_code_base, _code_size, is_jit ? PROT_EXEC : (PROT_READ | PROT_WRITE));
      detail::check<exceptions::allocation>((error == 0), "failed to enable code page access");
   }
   // Make code pages unexecutable so deadline timer can kill an
   // execution (in both JIT and Interpreter)
   void disable_code() {
      const auto error = mprotect(_code_base, _code_size, PROT_NONE);
      detail::check<exceptions::allocation>((error == 0), "failed to disable code page access");
   }

   const void* get_code_start() const {
      return _code_base;
   }

   span<std::byte> get_code_span() const {
      return {(std::byte*)_code_base, _code_size};
   }

   /* different semantics than free,
    * the memory must be at the end of the most recently allocated block.
    */
   template <typename T> void reclaim(const T* ptr, size_t size = 0) {
      detail::check<exceptions::allocation>((_offset / sizeof(T) >= size), "reclaimed too much memory");
      detail::check<exceptions::allocation>((size == 0 || (char*)(ptr + size) == (_base + _offset)),
                                            "reclaiming memory must be strictly LIFO");
      if (size != 0)
         _offset = ((char*)ptr - _base);
   }

   size_t largest_used_size() {
      return align_to_page(_largest_offset);
   }

   /*
    * Finalize the memory by unmapping any excess pages, this means that the allocator will no longer grow
    */
   void finalize() {
      if (_capacity != _offset) {
         std::size_t final_size = align_to_page(_offset);
         if (final_size < _capacity) { // final_size can grow to _capacity after align_to_page.
                                       // make sure no 0 size passed to munmap
            detail::check<exceptions::allocation>((munmap(_base + final_size, _capacity - final_size) == 0),
                                                  "failed to finalize growable_allocator");
            _capacity = _offset = final_size;
            if (final_size == 0) {
               // _base became invalid after munmap if final_size is 0 so
               // set it to nullptr to avoid being used
               _base = nullptr;
            }
         }
      }
   }

   void free() {
      detail::fail<exceptions::allocation>("unimplemented");
   }

   void release_base_memory() {
      if (_base != nullptr) {
         detail::check<exceptions::allocation>((munmap(_base, _capacity) == 0), "failed to release base memory");
         _base = nullptr;
      }
   }

   void reset() {
      _offset = 0;
   }

   size_t _offset = 0;
   size_t _largest_offset = 0;
   size_t _capacity = 0;
   char* _base = nullptr;
   char* _code_base = nullptr;
   size_t _code_size = 0;
   bool _is_jit = false;
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

 public:
   wasm_allocator();
   ~wasm_allocator();

   wasm_allocator(const wasm_allocator&) = delete;
   wasm_allocator& operator=(const wasm_allocator&) = delete;
   wasm_allocator(wasm_allocator&&) = delete;
   wasm_allocator& operator=(wasm_allocator&&) = delete;

   template <typename T> void alloc(size_t size = 1 /*in pages*/) {
      if (size == 0)
         return;
      detail::check<exceptions::allocation>((page >= 0), "require memory to allocate");
      detail::check<exceptions::allocation>((size + page <= max_pages), "exceeded max number of pages");
      int err = mprotect(raw + (page_size * page), (page_size * size), PROT_READ | PROT_WRITE);
      detail::check<exceptions::allocation>((err == 0), "mprotect failed");
      T* ptr = (T*)(raw + (page_size * page));
      std::memset(ptr, 0, page_size * size);
      page += size;
   }
   template <typename T> void free(std::size_t size) {
      if (size == 0)
         return;
      detail::check<exceptions::allocation>((page >= 0), "require memory to deallocate");
      detail::check<exceptions::allocation>((size <= static_cast<uint32_t>(page)), "freed too many pages");
      page -= size;
      int err = mprotect(raw + (page_size * page), (page_size * size), PROT_NONE);
      detail::check<exceptions::allocation>((err == 0), "mprotect failed");
   }
   void free() noexcept;

   // Initializes the memory controlled by the allocator.
   //
   // \post get_current_page() == new_pages
   // \post all allocated pages are zero-filled.
   void reset(uint32_t new_pages) {
      detail::check<exceptions::allocation>((raw != nullptr), "require memory to reset");
      if (page >= 0) {
         std::memset(raw, '\0', page_size * page); // zero the memory
      } else {
         std::size_t syspagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
         int err = mprotect(raw - syspagesize, syspagesize, PROT_READ);
         detail::check<exceptions::allocation>((err == 0), "mprotect failed");
         page = 0;
      }
      if (new_pages > static_cast<uint32_t>(page)) {
         alloc<char>(new_pages - page);
      } else if (new_pages < static_cast<uint32_t>(page)) {
         free<char>(page - new_pages);
      }
   }

   // Signal no memory defined
   void reset() {
      detail::check<exceptions::allocation>((raw != nullptr), "require memory to reset");
      if (page >= 0) {
         std::size_t syspagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
         std::memset(raw, '\0', page_size * page); // zero the memory
         int err = mprotect(raw - syspagesize, page_size * page + syspagesize, PROT_NONE);
         detail::check<exceptions::allocation>((err == 0), "mprotect failed");
      }
      page = -1;
   }

   template <typename T> inline T* get_base_ptr() const {
      return reinterpret_cast<T*>(raw);
   }
   template <typename T> inline T* create_pointer(uint32_t offset) {
      detail::check<exceptions::allocation>((raw != nullptr), "require memory to create pointer");
      return reinterpret_cast<T*>(raw + offset);
   }
   inline int32_t get_current_page() const {
      return page;
   }
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
} // namespace forge::vm::wasm::interpret
