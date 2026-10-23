#include "details/memory.hxx"

#include <boost/interprocess/managed_external_buffer.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__APPLE__) || !defined(__aarch64__)
#error The expression worker allocation domain is qualified on macOS ARM64 only.
#endif

namespace forge::expression::detail::memory {
namespace {

using heap_type = boost::interprocess::managed_external_buffer;

constexpr std::size_t maximum_bytes = 64 * 1024 * 1024;
constexpr std::size_t page_bytes = 16384;
constexpr int failure_endpoint = 3;

struct mapping {
   void* address;
   std::size_t bytes;
   mapping* next;
};

struct domain {
   domain() noexcept;

   pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
   std::size_t capacity = 0;
   std::size_t peak = 0;
   std::size_t failures = 0;
   heap_type* heap = nullptr;
   mapping* mappings = nullptr;
   alignas(heap_type) std::byte heap_object[sizeof(heap_type)];
};

// Both the domain header and Boost's segment manager live inside this store.
// No destructor is registered: dependency globals may release memory until exit.
alignas(page_bytes) std::byte storage[maximum_bytes];
constexpr std::size_t prefix_bytes =
    (sizeof(domain) + alignof(std::max_align_t) - 1) & ~(alignof(std::max_align_t) - 1);

bool read_capacity(std::size_t& result) noexcept {
   const char* value = std::getenv("FORGE_EXPRESSION_HEAP_BYTES");
   if (value == nullptr) {
      result = maximum_bytes;
      return true;
   }
   if (*value == '\0') {
      return false;
   }
   std::size_t bytes = 0;
   for (; *value != '\0'; ++value) {
      if (*value < '0' || *value > '9') {
         return false;
      }
      const auto digit = static_cast<std::size_t>(*value - '0');
      if (bytes > (maximum_bytes - digit) / 10) {
         return false;
      }
      bytes = bytes * 10 + digit;
   }
   if (bytes < prefix_bytes + heap_type::segment_manager::get_min_size() || bytes > maximum_bytes) {
      return false;
   }
   result = bytes;
   return true;
}

domain::domain() noexcept {
   if (!read_capacity(capacity)) {
      capacity = 0;
      return;
   }
   // The selected null-mutex/iset-index Boost manager initializes in place
   // without allocation. Size/alignment are checked before its constructor.
   // Allocation operations below exclusively use its nonthrowing API.
   heap =
       ::new (heap_object) heap_type(boost::interprocess::create_only, storage + prefix_bytes, capacity - prefix_bytes);
   peak = capacity - heap->get_free_memory();
}

domain& state() noexcept {
   static domain* value = ::new (storage) domain;
   return *value;
}

class lock {
 public:
   explicit lock(domain& value) noexcept : _value(value) {
      if (::pthread_mutex_lock(&_value.mutex) != 0) {
         __builtin_trap();
      }
   }
   ~lock() {
      if (::pthread_mutex_unlock(&_value.mutex) != 0) {
         __builtin_trap();
      }
   }

 private:
   domain& _value;
};

void failed(domain& value) noexcept {
   ++value.failures;
   // One byte is sufficient; the parent owns a nonblocking descriptor and
   // treats any byte as exhaustion. Full/closed endpoints require no retry.
   constexpr char marker = 1;
   const auto ignored = ::write(failure_endpoint, &marker, sizeof(marker));
   (void)ignored;
   errno = ENOMEM;
}

bool aligned(std::size_t alignment) noexcept {
   return alignment >= sizeof(void*) && (alignment & (alignment - 1)) == 0;
}

void update_peak(domain& value) noexcept {
   const auto used = value.capacity - value.heap->get_free_memory();
   if (used > value.peak) {
      value.peak = used;
   }
}

void* allocate_locked(domain& value, std::size_t bytes, std::size_t alignment) noexcept {
   if (value.heap == nullptr || bytes > value.capacity || alignment > value.capacity) {
      failed(value);
      return nullptr;
   }
   auto* result = value.heap->allocate_aligned(bytes == 0 ? 1 : bytes, alignment, std::nothrow);
   if (result == nullptr) {
      failed(value);
      return nullptr;
   }
   update_peak(value);
   return result;
}

void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept {
   auto& value = state();
   lock guard(value);
   return allocate_locked(value, bytes, alignment);
}

bool owned(const domain& value, const void* pointer) noexcept {
   const auto address = reinterpret_cast<std::uintptr_t>(pointer);
   const auto begin = reinterpret_cast<std::uintptr_t>(storage + prefix_bytes);
   return value.heap != nullptr && address >= begin && address < begin + value.capacity - prefix_bytes;
}

void release(void* pointer) noexcept {
   if (pointer == nullptr) {
      return;
   }
   auto& value = state();
   lock guard(value);
   if (!owned(value, pointer)) {
      // A foreign allocation violates the private worker boundary. Never
      // forward it to a system allocator or misreport it as heap exhaustion.
      __builtin_trap();
   }
   value.heap->deallocate(pointer);
}

std::size_t allocation_size(const void* pointer) noexcept {
   if (pointer == nullptr) {
      return 0;
   }
   auto& value = state();
   lock guard(value);
   return owned(value, pointer) ? value.heap->get_segment_manager()->size(pointer) : 0;
}

void* resize(void* pointer, std::size_t bytes) noexcept {
   if (pointer == nullptr) {
      return allocate(bytes);
   }
   if (bytes == 0) {
      release(pointer);
      return nullptr;
   }
   auto& value = state();
   lock guard(value);
   if (!owned(value, pointer)) {
      __builtin_trap();
   }
   auto* result = allocate_locked(value, bytes, alignof(std::max_align_t));
   if (result == nullptr) {
      return nullptr;
   }
   const auto previous = value.heap->get_segment_manager()->size(pointer);
   std::memcpy(result, pointer, bytes < previous ? bytes : previous);
   value.heap->deallocate(pointer);
   return result;
}

bool round_mapping(std::size_t bytes, std::size_t& result) noexcept {
   if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() - (page_bytes - 1)) {
      return false;
   }
   result = (bytes + page_bytes - 1) & ~(page_bytes - 1);
   return true;
}

void* map(void* address, std::size_t bytes, int protection, int flags, int fd, off_t offset) noexcept {
   // This is exactly Abseil LowLevelAlloc's ordinary Darwin mapping contract.
   // It is deliberately not a replacement for a general OS virtual-memory API.
   if (address != nullptr || protection != (PROT_READ | PROT_WRITE) || flags != (MAP_PRIVATE | MAP_ANONYMOUS) ||
       fd != -1 || offset != 0) {
      errno = ENOTSUP;
      return MAP_FAILED;
   }
   std::size_t rounded = 0;
   if (!round_mapping(bytes, rounded)) {
      errno = EINVAL;
      return MAP_FAILED;
   }
   auto& value = state();
   lock guard(value);
   auto* result = allocate_locked(value, rounded, page_bytes);
   if (result == nullptr) {
      return MAP_FAILED;
   }
   auto* record = static_cast<mapping*>(allocate_locked(value, sizeof(mapping), alignof(std::max_align_t)));
   if (record == nullptr) {
      value.heap->deallocate(result);
      return MAP_FAILED;
   }
   *record = {result, rounded, value.mappings};
   value.mappings = record;
   // MAP_ANONYMOUS memory is zero-filled even when this pool reuses a block.
   std::memset(result, 0, rounded);
   return result;
}

int unmap(void* address, std::size_t bytes) noexcept {
   std::size_t rounded = 0;
   if (!round_mapping(bytes, rounded)) {
      errno = EINVAL;
      return -1;
   }
   auto& value = state();
   lock guard(value);
   for (auto** cursor = &value.mappings; *cursor != nullptr; cursor = &(*cursor)->next) {
      auto* record = *cursor;
      if (record->address == address && record->bytes == rounded) {
         *cursor = record->next;
         value.heap->deallocate(record->address);
         value.heap->deallocate(record);
         return 0;
      }
   }
   errno = EINVAL;
   return -1;
}

} // namespace

snapshot stats() noexcept {
   auto& value = state();
   lock guard(value);
   return {value.capacity, value.heap == nullptr ? 0 : value.capacity - value.heap->get_free_memory(), value.peak,
           value.failures, value.heap != nullptr};
}

} // namespace forge::expression::detail::memory

// Private Mach-O symbol definitions bind the worker's static CEL/libc++ closure.
// They must never be exported by the worker or linked into the embedding host.
#define FORGE_EXPRESSION_MEMORY_SYMBOL(name) __attribute__((visibility("hidden"))) __asm__("_" #name)

extern "C" void* expression_malloc(std::size_t) noexcept FORGE_EXPRESSION_MEMORY_SYMBOL(malloc);
extern "C" void expression_free(void*) noexcept FORGE_EXPRESSION_MEMORY_SYMBOL(free);
extern "C" void* expression_calloc(std::size_t, std::size_t) noexcept FORGE_EXPRESSION_MEMORY_SYMBOL(calloc);
extern "C" void* expression_realloc(void*, std::size_t) noexcept FORGE_EXPRESSION_MEMORY_SYMBOL(realloc);
extern "C" void* expression_aligned_alloc(std::size_t, std::size_t) noexcept
    FORGE_EXPRESSION_MEMORY_SYMBOL(aligned_alloc);
extern "C" int expression_posix_memalign(void**, std::size_t, std::size_t) noexcept
    FORGE_EXPRESSION_MEMORY_SYMBOL(posix_memalign);
extern "C" void* expression_valloc(std::size_t) noexcept FORGE_EXPRESSION_MEMORY_SYMBOL(valloc);
extern "C" std::size_t expression_malloc_size(const void*) noexcept FORGE_EXPRESSION_MEMORY_SYMBOL(malloc_size);
extern "C" void* expression_mmap(void*, std::size_t, int, int, int, off_t) noexcept
    FORGE_EXPRESSION_MEMORY_SYMBOL(mmap);
extern "C" int expression_munmap(void*, std::size_t) noexcept FORGE_EXPRESSION_MEMORY_SYMBOL(munmap);

#undef FORGE_EXPRESSION_MEMORY_SYMBOL

extern "C" void* expression_malloc(std::size_t bytes) noexcept {
   return forge::expression::detail::memory::allocate(bytes);
}

extern "C" void expression_free(void* pointer) noexcept {
   forge::expression::detail::memory::release(pointer);
}

extern "C" void* expression_calloc(std::size_t count, std::size_t bytes) noexcept {
   using namespace forge::expression::detail::memory;
   if (bytes != 0 && count > std::numeric_limits<std::size_t>::max() / bytes) {
      auto& value = state();
      lock guard(value);
      failed(value);
      return nullptr;
   }
   const auto total = count * bytes;
   auto* result = allocate(total);
   if (result != nullptr) {
      std::memset(result, 0, total);
   }
   return result;
}

extern "C" void* expression_realloc(void* pointer, std::size_t bytes) noexcept {
   return forge::expression::detail::memory::resize(pointer, bytes);
}

extern "C" void* expression_aligned_alloc(std::size_t alignment, std::size_t bytes) noexcept {
   using namespace forge::expression::detail::memory;
   if (!aligned(alignment) || bytes % alignment != 0) {
      errno = EINVAL;
      return nullptr;
   }
   return allocate(bytes, alignment);
}

extern "C" int expression_posix_memalign(void** output, std::size_t alignment, std::size_t bytes) noexcept {
   using namespace forge::expression::detail::memory;
   if (output == nullptr || !aligned(alignment)) {
      return EINVAL;
   }
   const auto previous_errno = errno;
   auto* result = allocate(bytes, alignment);
   errno = previous_errno;
   if (result == nullptr) {
      return ENOMEM;
   }
   *output = result;
   return 0;
}

extern "C" void* expression_valloc(std::size_t bytes) noexcept {
   return forge::expression::detail::memory::allocate(bytes, forge::expression::detail::memory::page_bytes);
}

extern "C" std::size_t expression_malloc_size(const void* pointer) noexcept {
   return forge::expression::detail::memory::allocation_size(pointer);
}

extern "C" void* expression_mmap(void* address, std::size_t bytes, int protection, int flags, int fd,
                                 off_t offset) noexcept {
   return forge::expression::detail::memory::map(address, bytes, protection, flags, fd, offset);
}

extern "C" int expression_munmap(void* address, std::size_t bytes) noexcept {
   return forge::expression::detail::memory::unmap(address, bytes);
}
