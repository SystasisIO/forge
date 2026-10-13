#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "queued_record.hxx"
#include "export_operation.hxx"

namespace forge::otlp {

struct log_exporter::impl : std::enable_shared_from_this<impl> {
   impl(forge::asio::runtime& runtime, log_exporter_options options);
   void validate_options() const;
   bool enqueue(const forge::log_record& record);
   exporter_metrics metrics() const;
   void close_without_flush();

   static boost::asio::awaitable<void> flush_owned(std::shared_ptr<impl> owner);
   static boost::asio::awaitable<void> shutdown_owned(std::shared_ptr<impl> owner);
   static boost::asio::awaitable<export_result> export_owned(
       std::shared_ptr<impl> owner, std::vector<forge::log_record> records);

   void on_enqueue(bool should_flush);
   void arm_flush_timer();
   void start_flush();
   boost::asio::awaitable<void> wait_for_flush();
   boost::asio::awaitable<void> shutdown();
   void begin_shutdown();
   void finish_shutdown();
   void cancel_exports();
   boost::asio::awaitable<void> drain();
   boost::asio::awaitable<export_result> export_now(std::vector<forge::log_record> records);
   boost::asio::awaitable<bool> export_records(std::vector<forge::log_record> records);
   boost::asio::awaitable<void> wait_operation(std::shared_ptr<export_operation> operation);
   void complete_operation(const std::shared_ptr<export_operation>& operation, std::exception_ptr error);
   std::vector<forge::log_record> take_batch();
   boost::asio::awaitable<bool> export_batch(const std::vector<forge::log_record>& records);
   forge::net::http::request make_request(const std::string& payload) const;
   std::chrono::milliseconds retry_delay(std::uint32_t attempt);
   boost::asio::awaitable<bool> sleep_before_retry(std::chrono::milliseconds delay);
   std::chrono::milliseconds remaining_request_timeout() const;
   bool shutdown_expired() const;
   std::string encode_logs(const std::vector<forge::log_record>& records) const;
   bool empty_queue() const;
   bool closed_snapshot() const;
   void drop_all_queued();
   void record_export_attempt();
   void record_retry();
   void record_exported(std::size_t count);
   void record_failed(std::size_t count);
   void record_direct_failed(std::size_t count);
   void refresh_queue_metrics_locked() const;

   forge::asio::runtime& runtime;
   log_exporter_options options;
   boost::asio::strand<boost::asio::io_context::executor_type> strand;
   boost::asio::steady_timer flush_timer;
   boost::asio::steady_timer shutdown_timer;
   forge::net::http::base_url endpoint;
   std::unique_ptr<forge::net::http::client> client;

   // Only admission, the record queue and metrics cross the strand boundary.
   mutable std::mutex mutex;
   mutable exporter_metrics current_metrics;
   std::deque<queued_record> queue;
   std::size_t queue_bytes = 0;
   bool closed = false;
   bool shutdown_finished = false;

   bool flush_active = false;
   bool timer_armed = false;
   bool abandoned = false;
   std::uint64_t flush_timer_epoch = 0;
   std::optional<std::chrono::steady_clock::time_point> shutdown_deadline;
   std::exception_ptr shutdown_error;
   std::shared_ptr<export_operation> flush_operation;
   std::shared_ptr<export_operation> shutdown_operation;
   std::vector<std::shared_ptr<export_operation>> direct_operations;
   std::minstd_rand jitter_source{0x0f7c10};
};

} // namespace forge::otlp
