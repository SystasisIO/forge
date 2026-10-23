module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <boost/asio/awaitable.hpp>

export module forge.expression.program:runtime;

import forge.expression.worker_protocol;
import forge.variant.value;
import forge.asio.runtime;

export namespace forge::expression {

class program;

struct memory_usage {
   std::uint64_t capacity = 0;
   std::uint64_t used = 0;
   std::uint64_t peak = 0;
   std::uint64_t failures = 0;
};

// One persistent, single-threaded native helper. Calls on the same runtime
// serialize; independent runtimes provide explicitly bounded parallelism.
class runtime {
 public:
   struct options {
      std::filesystem::path helper;
      std::size_t heap_bytes = 64U * 1024U * 1024U;
      std::chrono::milliseconds timeout{30000};
   };

   explicit runtime(options configuration);
   runtime(forge::asio::runtime& executor, options configuration);
   ~runtime();
   runtime(runtime&&) noexcept;
   runtime& operator=(runtime&&) noexcept;
   runtime(const runtime&) = delete;
   runtime& operator=(const runtime&) = delete;

   [[nodiscard]] memory_usage memory() const;
   [[nodiscard]] boost::asio::awaitable<memory_usage> memory_async() const;
   // Stop admission, cancel exchanges and join the sole child before returning.
   // Keep an injected executor alive until this barrier completes. On its IO
   // threads call this before destroying the wrapper; sync wrappers are host-only.
   boost::asio::awaitable<void> shutdown_async();

 private:
   friend class program;
   struct impl;
   std::shared_ptr<impl> _impl;

   static worker_protocol::response exchange(const std::shared_ptr<impl>&, worker_protocol::request,
                                             const forge::variant_object& = {},
                                             std::uint64_t* accepted_program = nullptr);
   static boost::asio::awaitable<worker_protocol::response> async_exchange(std::shared_ptr<impl>,
                                                                           worker_protocol::request,
                                                                           forge::variant_object = {},
                                                                           std::uint64_t* accepted_program = nullptr);
   static boost::asio::awaitable<memory_usage> read_memory(std::shared_ptr<impl>);
   static boost::asio::awaitable<void> shutdown_owned(std::shared_ptr<impl>);
   static void release(const std::shared_ptr<impl>&, std::uint64_t) noexcept;
};

} // namespace forge::expression
