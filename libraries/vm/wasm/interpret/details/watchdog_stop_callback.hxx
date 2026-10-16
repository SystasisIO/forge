#pragma once

#include <stop_token>

namespace forge::vm::wasm::interpret::detail {

class watchdog_stop_callback {
 public:
   watchdog_stop_callback(std::stop_token stop, void (*notify)(void*) noexcept, void* context) noexcept;
   ~watchdog_stop_callback();

   watchdog_stop_callback(const watchdog_stop_callback&) = delete;
   watchdog_stop_callback& operator=(const watchdog_stop_callback&) = delete;

 private:
   struct notification {
      void (*notify)(void*) noexcept;
      void* context;

      void operator()() const noexcept;
   };

   std::stop_callback<notification> _callback;
};

} // namespace forge::vm::wasm::interpret::detail
