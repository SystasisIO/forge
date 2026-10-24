#pragma once

namespace forge::expression::detail {

// A single-threaded, persistent child. stdin/stdout carry bounded Raw frames;
// fd 3 belongs exclusively to the private allocator's OOM notification.
int run_worker() noexcept;

} // namespace forge::expression::detail
