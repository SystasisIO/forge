#pragma once

#include "quic_engine_support.hxx"

namespace forge::net::quic::detail {
struct engine_stream::impl {
   struct pending_write {
      std::vector<std::uint8_t> data;
      std::shared_ptr<void> lifetime;
      std::size_t submitted = 0;
      std::uint64_t base_offset = 0;
      bool base_offset_set = false;
      bool fin = false;
      std::vector<std::weak_ptr<asio::steady_timer>> waiters;
   };

   struct retained_write {
      std::vector<std::uint8_t> data;
      std::shared_ptr<void> lifetime;
      std::uint64_t base_offset = 0;
      bool fin = false;
   };

   explicit impl(std::int64_t id_value) : id(id_value) {}

   std::int64_t id = -1;
   std::weak_ptr<engine_connection::impl> connection;
   std::map<std::uint64_t, std::vector<std::uint8_t>> inbound_segments;
   std::deque<std::vector<std::uint8_t>> inbound_ready;
   std::deque<pending_write> outbound;
   std::deque<retained_write> retained;
   acknowledged_ranges acknowledged;
   std::uint64_t recv_next_offset = 0;
   std::uint64_t send_next_offset = 0;
   bool remote_read_closed = false;
   bool remote_read_reset = false;
   bool local_write_closed = false;
   bool local_write_canceled = false;
   bool native_write_rejected = false;
   bool fin_queued = false;
   std::uint64_t fin_send_generation = 0;
   bool reset = false;
   bool reset_counted = false;
   bool closed = false;
   bool cancel_worker_started = false;
   // Strand-owned recovery work is distinct from native stream termination.
   std::size_t terminal_cleanup_owners = 0;
   std::exception_ptr terminal_cleanup_error;
   forge::asio::notification cancel_requested;
   // Owner-strand state is mirrored through atomics so callers can join a
   // terminal recovery without reading the strand-owned booleans.
   std::atomic_bool terminal_published = false;
   std::atomic_bool terminal_cleanup_complete = false;
   forge::asio::notification terminal_notification;
   std::vector<std::weak_ptr<asio::steady_timer>> read_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> write_waiters;
};
} // namespace forge::net::quic::detail
