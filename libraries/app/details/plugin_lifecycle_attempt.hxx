#pragma once

namespace forge::app {

struct plugin_lifecycle::attempt {
   lifecycle_operation value;
   std::uint64_t id;
   std::uint64_t generation;
   bool completed = false;
   std::exception_ptr error;
   std::exception_ptr abandoned_error;
   std::exception_ptr stopped_error;
};

} // namespace forge::app
