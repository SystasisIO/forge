module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <forge/exceptions/macros.hpp>

module forge.vm.wasm.optimizing.executor;

import forge.vm.wasm.interpret.allocator;
import forge.vm.wasm.interpret.exceptions;

#include "details/native_stack.hxx"

namespace forge::vm::wasm::optimizing {

std::size_t native_stack::mapping_bytes(std::uint64_t usable_bytes) {
   const auto page = ::sysconf(_SC_PAGESIZE);
   if (page <= 0 || (static_cast<std::uint64_t>(page) & (page - 1)) != 0)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "invalid local native stack capacity");
   const auto guard = std::max(static_cast<std::uint64_t>(page), max_frame_bytes);
   // The usable mapping never silently exceeds the caller's physical cap.
   const auto aligned = usable_bytes & ~(static_cast<std::uint64_t>(page) - 1);
   if (aligned == 0 || guard > std::numeric_limits<std::size_t>::max() / 2 ||
       aligned > std::numeric_limits<std::size_t>::max() - 2 * guard)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "invalid local native stack capacity");
   return static_cast<std::size_t>(aligned + 2 * guard);
}

native_stack::native_stack(std::uint64_t usable_bytes) : _storage(mapping_bytes(usable_bytes)) {
   _page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
   _guard_bytes = std::max(_page, static_cast<std::size_t>(max_frame_bytes));
   _bytes = mapping_bytes(usable_bytes);
   _base = _storage.alloc<std::byte>(_bytes); // Exactly one allocation: never grows/remaps.
   if (::mprotect(_base, _guard_bytes, PROT_NONE) != 0 ||
       ::mprotect(_base + _bytes - _guard_bytes, _guard_bytes, PROT_NONE) != 0)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native stack guard setup failed");
}

native_stack::bounds native_stack::usable() const noexcept {
   return {reinterpret_cast<std::uintptr_t>(_base + _guard_bytes),
           reinterpret_cast<std::uintptr_t>(_base + _bytes - _guard_bytes)};
}

std::span<const std::byte> native_stack::guard() const noexcept {
   return {_base, _guard_bytes};
}

native_stack::bounds native_stack::current_thread_bounds() {
#if defined(__APPLE__) && defined(__aarch64__)
   const auto thread = ::pthread_self();
   const auto top = reinterpret_cast<std::uintptr_t>(::pthread_get_stackaddr_np(thread));
   const auto size = ::pthread_get_stacksize_np(thread);
   if (top == 0 || size == 0 || size > top)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "host thread stack bounds unavailable");
   return {top - size, top};
#elif defined(__linux__) && defined(__x86_64__)
   auto attributes = pthread_attr_t{};
   if (::pthread_getattr_np(::pthread_self(), &attributes) != 0)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "host thread stack bounds unavailable");
   auto* base = static_cast<void*>(nullptr);
   auto size = std::size_t{0};
   const auto error = ::pthread_attr_getstack(&attributes, &base, &size);
   const auto cleanup = ::pthread_attr_destroy(&attributes);
   const auto low = reinterpret_cast<std::uintptr_t>(base);
   if (error != 0 || cleanup != 0 || low == 0 || size == 0 || size > UINTPTR_MAX - low)
      FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "host thread stack bounds unavailable");
   return {low, low + size};
#else
   FORGE_THROW_EXCEPTION(interpret::exceptions::execution_unavailable, "native stack platform unavailable");
#endif
}

std::uintptr_t native_stack::current_pointer() noexcept {
   auto result = std::uintptr_t{0};
#if defined(__APPLE__) && defined(__aarch64__)
   asm volatile("mov %0, sp" : "=r"(result));
#elif defined(__linux__) && defined(__x86_64__)
   asm volatile("movq %%rsp, %0" : "=r"(result));
#endif
   return result;
}

#if defined(__APPLE__) && defined(__aarch64__)
[[gnu::naked, gnu::noinline]] std::uint64_t native_stack::call(void*, entry, void*, std::uintptr_t*) noexcept {
   asm volatile("mov x9, sp\n"
                "str x9, [x3]\n"
                "sub x0, x0, #32\n"
                "stp x29, x30, [x0]\n"
                "str x9, [x0, #16]\n"
                "mov sp, x0\n"
                "mov x0, x2\n"
                "blr x1\n"
                "ldr x9, [sp, #16]\n"
                "ldp x29, x30, [sp]\n"
                "mov sp, x9\n"
                "ret\n");
}
#elif defined(__linux__) && defined(__x86_64__)
[[gnu::naked, gnu::noinline]] std::uint64_t native_stack::call(void*, entry, void*, std::uintptr_t*) noexcept {
   asm volatile("movq %rsp, (%rcx)\n"
                "subq $32, %rdi\n"
                "movq %rsp, (%rdi)\n"
                "movq %rdi, %rsp\n"
                "movq %rdx, %rdi\n"
                "callq *%rsi\n"
                "movq (%rsp), %rsp\n"
                "retq\n");
}
#else
std::uint64_t native_stack::call(void*, entry, void*, std::uintptr_t*) noexcept {
   std::terminate();
}
#endif

} // namespace forge::vm::wasm::optimizing
