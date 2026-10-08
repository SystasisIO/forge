#include "pubsub_claim_allocation.hxx"

#include <cstdlib>
#include <exception>
#include <new>

namespace {
thread_local bool armed = false;
thread_local std::size_t fail_at = 0;
thread_local std::size_t calls = 0;

void* allocate(std::size_t size, std::size_t alignment = 0) {
   forge::tests::p2p::pubsub_claim_allocation::record();
   void* result = nullptr;
   if (alignment == 0) {
      result = std::malloc(size == 0 ? 1 : size);
   } else if (posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0) {
      result = nullptr;
   }
   if (!result) { throw std::bad_alloc{}; }
   return result;
}
} // namespace

namespace forge::tests::p2p {

pubsub_claim_allocation::pubsub_claim_allocation(std::size_t ordinal) noexcept {
   if (armed) { std::terminate(); }
   ::calls = 0;
   fail_at = ordinal;
   armed = true;
}

pubsub_claim_allocation::~pubsub_claim_allocation() { armed = false; }

std::size_t pubsub_claim_allocation::calls() const noexcept { return ::calls; }

void pubsub_claim_allocation::record() {
   if (armed && ++::calls == fail_at) { throw std::bad_alloc{}; }
}

} // namespace forge::tests::p2p

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
   return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
   return allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
