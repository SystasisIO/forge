#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>

namespace forge::app::detail {

struct shutdown_state {
   std::mutex mutex;
   std::condition_variable ready;
   bool done = false;
   bool timed_out = false;
   std::exception_ptr error;
};

} // namespace forge::app::detail
