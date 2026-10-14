#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace forge {

class async_sink::impl final : public std::enable_shared_from_this<async_sink::impl> {
 public:
   impl(std::shared_ptr<sink> downstream, forge::asio::compute::executor executor,
        async_sink_options options);
   ~impl();

   void enqueue(const log_record& record) noexcept;
   void close() noexcept;
   [[nodiscard]] async_sink_metrics snapshot() const noexcept;
   [[nodiscard]] std::uint64_t capture_fence(bool close_admission) noexcept;
   [[nodiscard]] static std::chrono::steady_clock::time_point deadline_after(
       std::chrono::milliseconds timeout) noexcept;
   static boost::asio::awaitable<bool> wait_fence(
       std::shared_ptr<impl> owner, std::uint64_t fence,
       std::chrono::steady_clock::time_point deadline);

 private:
   struct queued_record {
      log_record record;
      std::size_t bytes = 0;
      std::unique_ptr<queued_record> next;
   };

   static void increment(std::uint64_t& counter) noexcept;
   [[nodiscard]] static bool add_bytes(std::size_t& bytes, std::size_t amount,
                                       std::size_t limit) noexcept;
   [[nodiscard]] static bool record_bytes(const log_record& record, bool use_capacity,
                                          std::size_t limit, std::size_t& bytes) noexcept;
   [[nodiscard]] static log_record copy_record(const log_record& record);
   void drain(std::uint64_t generation) noexcept;

   std::shared_ptr<sink> _downstream;
   forge::asio::compute::executor _executor;
   const async_sink_options _options;
   mutable std::mutex _mutex;
   std::unique_ptr<queued_record> _head;
   queued_record* _tail = nullptr;
   async_sink_metrics _metrics;
   std::uint64_t _completed = 0;
   std::uint64_t _pump_generation = 0;
   bool _pump_scheduled = false;
   forge::asio::notification _changed;

   static thread_local bool _in_downstream;
};

} // namespace forge
