module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

export module forge.log.async_sink;

export import forge.log.record;
import forge.asio.compute;

export namespace forge {

struct async_sink_options {
   std::size_t max_records = 8192;
   std::size_t max_bytes = 8 * 1024 * 1024;
};

struct async_sink_metrics {
   // accepted == delivered + failed + queued_records. Admission drops (including
   // executor refusal and recursive emission) are not acceptance-fence entries.
   std::uint64_t accepted = 0;
   std::uint64_t delivered = 0;
   std::uint64_t dropped = 0;
   std::uint64_t failed = 0;
   std::uint64_t suppressed_reentrant = 0;
   // Include the record currently inside the downstream callback.
   std::size_t queued_records = 0;
   // Conservative retained payload charge: node, vector capacities and every
   // string capacity plus terminator; excludes allocator/runtime bookkeeping.
   std::size_t queued_bytes = 0;
   bool closed = false;
};

// The executor must belong to an external, dedicated blocking-worker compute
// pool, never HTTP I/O. Its owner must drain sinks before stopping/joining it.
// A callback that never returns cannot be killed: timeouts only stop waiting,
// and the pool owner cannot be destroyed until that callback is released.
// Copies own all record payloads; source_location still references compiler
// strings, which must outlive delivery (do not unload their module meanwhile).
class async_sink final : public sink {
 public:
   explicit async_sink(std::shared_ptr<sink> downstream, forge::asio::compute::executor executor,
                       async_sink_options options = {});
   ~async_sink() override;

   async_sink(const async_sink&) = delete;
   async_sink& operator=(const async_sink&) = delete;
   async_sink(async_sink&&) = delete;
   async_sink& operator=(async_sink&&) = delete;

   // Reentrant emissions from ANY async_sink downstream callback are dropped,
   // preventing recursive routes from bouncing indefinitely between workers.
   void log(const log_record& record) noexcept override;
   void close() noexcept;
   [[nodiscard]] async_sink_metrics snapshot() const noexcept;

   // Non-coroutine facades capture ownership, acceptance fence and absolute
   // deadline at CALL time, even if awaiting starts later. Completion includes
   // failed delivery attempts; no downstream flush()/durability is implied.
   // Cancellation affects only this waiter, never the drain pump.
   boost::asio::awaitable<bool> async_flush(
       std::chrono::milliseconds timeout = std::chrono::seconds{5});
   boost::asio::awaitable<bool> async_shutdown(
       std::chrono::milliseconds timeout = std::chrono::seconds{5});

 private:
   class impl;
   std::shared_ptr<impl> _impl;
};

} // namespace forge
