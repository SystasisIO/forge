#pragma once

#include <memory>
#include <cstddef>
#include <mutex>

namespace forge::vm::wasm::interpret::detail {

class engine_load_budget;

class engine_runtime : public std::enable_shared_from_this<engine_runtime> {
 public:
   static std::shared_ptr<engine_runtime> acquire();
   ~engine_runtime();
   engine_runtime(const engine_runtime&) = delete;
   engine_runtime& operator=(const engine_runtime&) = delete;
   struct cleanup_report {
      std::size_t attempted;
      std::size_t remaining;
   };
   void retain_cleanup(const std::shared_ptr<engine_load_budget>&) noexcept;
   void retire_cleanup(engine_load_budget*) noexcept;
   cleanup_report retry_cleanup(std::size_t max_owners) noexcept;
   static cleanup_report retry_pending_cleanup(std::size_t max_owners) noexcept;
   std::size_t pending_cleanup() const noexcept;

 private:
   engine_runtime();
   static std::mutex _initialization_mutex;
   static std::weak_ptr<engine_runtime> _initialized;
   mutable std::mutex _cleanup_mutex;
   engine_load_budget* _cleanup_head = nullptr;
   engine_load_budget* _cleanup_tail = nullptr;
   std::size_t _cleanup_count = 0;
};

} // namespace forge::vm::wasm::interpret::detail
