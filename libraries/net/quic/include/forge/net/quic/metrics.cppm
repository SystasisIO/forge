module;

#include <cstdint>
#include <cstddef>

export module forge.net.quic.metrics;

export namespace forge::net::quic {

struct connection_metrics {
   std::uint64_t connections_opened = 0;
   std::uint64_t connections_closed = 0;
   std::uint64_t handshakes_started = 0;
   std::uint64_t handshakes_completed = 0;
   std::uint64_t handshakes_failed = 0;
   std::uint64_t streams_opened = 0;
   std::uint64_t streams_accepted = 0;
   // Unique streams reset in either direction, not the number of reset frames.
   std::uint64_t streams_reset = 0;
   std::uint64_t frames_sent = 0;
   std::uint64_t frames_received = 0;
   std::uint64_t bytes_sent = 0;
   std::uint64_t bytes_received = 0;
   std::uint64_t packets_sent = 0;
   std::uint64_t packets_received = 0;
   std::uint64_t timeouts = 0;
   std::uint64_t cancellations = 0;
   std::uint64_t backpressure_rejections = 0;
   std::size_t queued_bytes = 0;
   std::size_t active_streams = 0;
   bool closed = false;
   std::uint64_t retry_packets_received = 0;
   std::uint64_t new_tokens_received = 0;
   std::uint64_t new_tokens_submitted = 0;
};

} // namespace forge::net::quic
