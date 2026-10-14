#pragma once

#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace forge::vm::wasm::interpret::detail {

class watchdog_stop_callback {
 public:
   watchdog_stop_callback(std::stop_token stop, std::mutex& mutex, std::condition_variable& condition) noexcept;
   ~watchdog_stop_callback();

   watchdog_stop_callback(const watchdog_stop_callback&) = delete;
   watchdog_stop_callback& operator=(const watchdog_stop_callback&) = delete;

 private:
   struct notification {
      std::mutex* mutex;
      std::condition_variable* condition;

      void operator()() const noexcept;
   };

   std::stop_callback<notification> _callback;
};

} // namespace forge::vm::wasm::interpret::detail
