#include "details/engine_connection_metrics_state.hxx"
#include "details/engine_connection_impl.hxx"
#include "details/engine_listener_impl.hxx"

namespace forge::net::quic::detail {
[[nodiscard]] engine_connection_metrics engine_connection_metrics_state::snapshot() const noexcept {
   const auto relaxed = std::memory_order_relaxed;
   return engine_connection_metrics{
       .connections_opened = connections_opened.load(relaxed),
       .connections_closed = connections_closed.load(relaxed),
       .handshakes_started = handshakes_started.load(relaxed),
       .handshakes_completed = handshakes_completed.load(relaxed),
       .handshakes_failed = handshakes_failed.load(relaxed),
       .streams_opened = streams_opened.load(relaxed),
       .streams_accepted = streams_accepted.load(relaxed),
       .streams_reset = streams_reset.load(relaxed),
       .frames_sent = frames_sent.load(relaxed),
       .frames_received = frames_received.load(relaxed),
       .bytes_sent = bytes_sent.load(relaxed),
       .bytes_received = bytes_received.load(relaxed),
       .packets_sent = packets_sent.load(relaxed),
       .packets_received = packets_received.load(relaxed),
       .timeouts = timeouts.load(relaxed),
       .cancellations = cancellations.load(relaxed),
       .backpressure_rejections = backpressure_rejections.load(relaxed),
       .retry_packets_received = retry_packets_received.load(relaxed),
       .new_tokens_received = new_tokens_received.load(relaxed),
       .new_tokens_submitted = new_tokens_submitted.load(relaxed),
       .queued_bytes = queued_bytes.load(relaxed),
       .active_streams = active_streams.load(relaxed),
       .closed = closed.load(relaxed),
   };
}
} // namespace forge::net::quic::detail
