#include <__config>
#include <forge/contract/intrinsics.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
[[noreturn]] void contract_abort(const char* message);
}

_LIBCPP_BEGIN_NAMESPACE_STD
void __libcpp_verbose_abort(const char* message, ...) noexcept {
   contract_abort(message == nullptr ? "libc++ contract runtime failure" : message);
}
_LIBCPP_END_NAMESPACE_STD

namespace {
[[noreturn]] void contract_abort(const char* message) {
   auto size = std::uint32_t{0};
   while (message[size] != '\0') {
      ++size;
   }
   eosio_assert_message(0U, message, size);
   __builtin_unreachable();
}

void* allocate(std::size_t size) noexcept {
   return std::malloc(size == 0 ? 1 : size);
}

void* allocate_aligned(std::size_t size, std::align_val_t requested_alignment) noexcept {
   auto alignment = static_cast<std::size_t>(requested_alignment);
   if (alignment < alignof(void*)) {
      alignment = alignof(void*);
   }
   if ((alignment & (alignment - 1)) != 0 ||
       size > std::numeric_limits<std::size_t>::max() - alignment - sizeof(void*)) {
      return nullptr;
   }

   auto* allocation = std::malloc((size == 0 ? 1 : size) + alignment - 1 + sizeof(void*));
   if (allocation == nullptr) {
      return nullptr;
   }

   const auto base = reinterpret_cast<std::uintptr_t>(allocation) + sizeof(void*);
   const auto aligned = (base + alignment - 1) & ~(alignment - 1);
   auto* result = reinterpret_cast<void*>(aligned);
   reinterpret_cast<void**>(result)[-1] = allocation;
   return result;
}

[[noreturn]] void allocation_failed() {
   contract_abort("contract allocation failed");
}

void deallocate_aligned(void* value) noexcept {
   if (value != nullptr) {
      std::free(reinterpret_cast<void**>(value)[-1]);
   }
}
} // namespace

void* operator new(std::size_t size) {
   if (auto* result = allocate(size)) {
      return result;
   }
   allocation_failed();
}

void* operator new[](std::size_t size) {
   return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
   return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
   return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
   if (auto* result = allocate_aligned(size, alignment)) {
      return result;
   }
   allocation_failed();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
   return ::operator new(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
   return allocate_aligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
   return allocate_aligned(size, alignment);
}

void operator delete(void* value) noexcept {
   std::free(value);
}

void operator delete[](void* value) noexcept {
   std::free(value);
}

void operator delete(void* value, std::size_t) noexcept {
   std::free(value);
}

void operator delete[](void* value, std::size_t) noexcept {
   std::free(value);
}

void operator delete(void* value, const std::nothrow_t&) noexcept {
   std::free(value);
}

void operator delete[](void* value, const std::nothrow_t&) noexcept {
   std::free(value);
}

void operator delete(void* value, std::align_val_t) noexcept {
   deallocate_aligned(value);
}

void operator delete[](void* value, std::align_val_t) noexcept {
   deallocate_aligned(value);
}

void operator delete(void* value, std::size_t, std::align_val_t) noexcept {
   deallocate_aligned(value);
}

void operator delete[](void* value, std::size_t, std::align_val_t) noexcept {
   deallocate_aligned(value);
}

void operator delete(void* value, std::align_val_t, const std::nothrow_t&) noexcept {
   deallocate_aligned(value);
}

void operator delete[](void* value, std::align_val_t, const std::nothrow_t&) noexcept {
   deallocate_aligned(value);
}

extern "C" [[noreturn]] void abort() {
   contract_abort("contract aborted");
}

extern "C" [[noreturn]] void __assert_fail(const char* expression, const char*, int, const char*) {
   contract_abort(expression == nullptr ? "contract assertion failed" : expression);
}
