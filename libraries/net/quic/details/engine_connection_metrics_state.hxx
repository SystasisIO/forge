#pragma once

#include "quic_engine_support.hxx"

namespace forge::net::quic::detail {
struct engine_connection_metrics_state {
   std::atomic<std::uint64_t> connections_opened{0};
   std::atomic<std::uint64_t> connections_closed{0};
   std::atomic<std::uint64_t> handshakes_started{0};
   std::atomic<std::uint64_t> handshakes_completed{0};
   std::atomic<std::uint64_t> handshakes_failed{0};
   std::atomic<std::uint64_t> streams_opened{0};
   std::atomic<std::uint64_t> streams_accepted{0};
   std::atomic<std::uint64_t> streams_reset{0};
   std::atomic<std::uint64_t> frames_sent{0};
   std::atomic<std::uint64_t> frames_received{0};
   std::atomic<std::uint64_t> bytes_sent{0};
   std::atomic<std::uint64_t> bytes_received{0};
   std::atomic<std::uint64_t> packets_sent{0};
   std::atomic<std::uint64_t> packets_received{0};
   std::atomic<std::uint64_t> timeouts{0};
   std::atomic<std::uint64_t> cancellations{0};
   std::atomic<std::uint64_t> backpressure_rejections{0};
   std::atomic<std::uint64_t> retry_packets_received{0};
   std::atomic<std::uint64_t> new_tokens_received{0};
   std::atomic<std::uint64_t> new_tokens_submitted{0};
   std::atomic<std::size_t> queued_bytes{0};
   std::atomic<std::size_t> active_streams{0};
   std::atomic<bool> closed{false};

   [[nodiscard]] engine_connection_metrics snapshot() const noexcept;
};
} // namespace forge::net::quic::detail
