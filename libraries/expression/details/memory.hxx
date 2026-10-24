#pragma once

#include <cstddef>

namespace forge::expression::detail::memory {

// Helper-only allocation domain. The backing store and its allocator metadata
// have process lifetime. This is a native heap bound, not an RSS/OS-memory cap:
// the worker's image, native stack and system TLS have separate fixed ownership.
// The parent supplies a nonblocking allocation-failure endpoint at descriptor 3.
struct snapshot {
   std::size_t capacity = 0;
   std::size_t allocated = 0;
   std::size_t peak = 0;
   std::size_t failures = 0;
   bool startup_valid = false;
};

// FORGE_EXPRESSION_HEAP_BYTES may lower the fixed 64 MiB ceiling. Invalid or
// insufficient startup configuration leaves allocation closed, without an OS
// fallback. Snapshots include the domain header, allocator and mapping metadata,
// block overhead and alignment padding, rather than requested payload bytes.
snapshot stats() noexcept;

} // namespace forge::expression::detail::memory
