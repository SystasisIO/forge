#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>

namespace forge::net::http::detail {

// Synchronous facade completion only; resource ownership stays on the acceptor strand.
struct server_wait_state {
   std::mutex mutex;
   std::condition_variable ready;
   bool done = false;
   std::exception_ptr error;
};

} // namespace forge::net::http::detail
