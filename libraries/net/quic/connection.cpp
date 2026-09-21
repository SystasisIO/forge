module;

#include <forge/exceptions/macros.hpp>

#include "details/wrapper_handles.hxx"

#include <memory>
#include <optional>
#include <utility>

#include <boost/asio/awaitable.hpp>

module forge.net.quic.connection;

import forge.net.quic.exceptions;
import forge.net.quic.endpoint;
import forge.net.quic.security;
import forge.net.quic.stream;

namespace forge::net::quic {
namespace {

[[nodiscard]] exceptions::code map_error(detail::engine_error_kind kind) noexcept {
   switch (kind) {
   case detail::engine_error_kind::invalid_endpoint:
      return exceptions::code::invalid_endpoint;
   case detail::engine_error_kind::invalid_options:
      return exceptions::code::invalid_options;
   case detail::engine_error_kind::dependency_unavailable:
      return exceptions::code::dependency_unavailable;
   case detail::engine_error_kind::connect_timeout:
      return exceptions::code::connect_timeout;
   case detail::engine_error_kind::handshake_timeout:
      return exceptions::code::handshake_timeout;
   case detail::engine_error_kind::idle_timeout:
      return exceptions::code::idle_timeout;
   case detail::engine_error_kind::tls_failed:
      return exceptions::code::tls_failed;
   case detail::engine_error_kind::peer_verification_failed:
      return exceptions::code::peer_verification_failed;
   case detail::engine_error_kind::alpn_mismatch:
      return exceptions::code::alpn_mismatch;
   case detail::engine_error_kind::frame_too_large:
      return exceptions::code::frame_too_large;
   case detail::engine_error_kind::malformed_frame:
      return exceptions::code::malformed_frame;
   case detail::engine_error_kind::backpressure_rejected:
      return exceptions::code::backpressure_rejected;
   case detail::engine_error_kind::connection_rejected:
      return exceptions::code::connection_rejected;
   case detail::engine_error_kind::connection_closed:
      return exceptions::code::connection_closed;
   case detail::engine_error_kind::stream_closed:
      return exceptions::code::stream_closed;
   case detail::engine_error_kind::stream_reset:
      return exceptions::code::stream_reset;
   case detail::engine_error_kind::canceled:
      return exceptions::code::canceled;
   case detail::engine_error_kind::internal_error:
      return exceptions::code::internal;
   }
   return exceptions::code::internal;
}

[[noreturn]] void raise_engine_failure(const detail::engine_failure& error) {
   FORGE_THROW_CODE(map_error(error.kind()), error.what());
}

[[nodiscard]] connection_metrics map_metrics(const detail::engine_connection_metrics& metrics) noexcept {
   return connection_metrics{
       .connections_opened = metrics.connections_opened,
       .connections_closed = metrics.connections_closed,
       .handshakes_started = metrics.handshakes_started,
       .handshakes_completed = metrics.handshakes_completed,
       .handshakes_failed = metrics.handshakes_failed,
       .streams_opened = metrics.streams_opened,
       .streams_accepted = metrics.streams_accepted,
       .streams_reset = metrics.streams_reset,
       .frames_sent = metrics.frames_sent,
       .frames_received = metrics.frames_received,
       .bytes_sent = metrics.bytes_sent,
       .bytes_received = metrics.bytes_received,
       .packets_sent = metrics.packets_sent,
       .packets_received = metrics.packets_received,
       .timeouts = metrics.timeouts,
       .cancellations = metrics.cancellations,
       .backpressure_rejections = metrics.backpressure_rejections,
       .queued_bytes = metrics.queued_bytes,
       .active_streams = metrics.active_streams,
       .closed = metrics.closed,
       .retry_packets_received = metrics.retry_packets_received,
       .new_tokens_received = metrics.new_tokens_received,
       .new_tokens_submitted = metrics.new_tokens_submitted,
   };
}

} // namespace

struct connection::impl {
   std::shared_ptr<detail::engine_connection> engine;
};

connection::connection() = default;

connection::connection(detail::connection_handle handle)
    : impl_(std::make_shared<impl>(impl{.engine = std::move(handle.engine)})) {}

connection::~connection() = default;

connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;

bool connection::valid() const noexcept {
   return impl_ != nullptr;
}

endpoint connection::local_endpoint() const {
   if (!impl_ || !impl_->engine) {
      return endpoint{};
   }
   const auto local = impl_->engine->local_endpoint();
   return endpoint{.host = local.host, .port = local.port, .zone = local.zone};
}

endpoint connection::remote_endpoint() const {
   if (!impl_ || !impl_->engine) {
      return endpoint{};
   }
   const auto remote = impl_->engine->remote_endpoint();
   return endpoint{.host = remote.host, .port = remote.port, .zone = remote.zone};
}

connection_metrics connection::metrics() const {
   return impl_ && impl_->engine ? map_metrics(impl_->engine->metrics()) : connection_metrics{};
}

std::optional<peer_certificate> connection::peer_certificate() const {
   if (!impl_ || !impl_->engine) {
      return std::nullopt;
   }
   auto peer = impl_->engine->peer_certificate();
   if (!peer) {
      return std::nullopt;
   }
   return forge::net::quic::peer_certificate{
       .der = std::move(peer->der),
       .sha256_fingerprint = std::move(peer->sha256_fingerprint),
   };
}

boost::asio::awaitable<stream> connection::async_open_stream() {
   if (!impl_ || !impl_->engine) {
      FORGE_THROW_EXCEPTION(exceptions::connection_closed, "invalid QUIC connection");
   }
   try {
      auto engine_stream = co_await impl_->engine->async_open_stream();
      co_return detail::stream_access::make(
          detail::stream_handle{.engine = std::move(engine_stream), .connection = impl_->engine});
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   }
}

boost::asio::awaitable<stream> connection::async_accept_stream() {
   if (!impl_ || !impl_->engine) {
      FORGE_THROW_EXCEPTION(exceptions::connection_closed, "invalid QUIC connection");
   }
   try {
      auto engine_stream = co_await impl_->engine->async_accept_stream();
      co_return detail::stream_access::make(
          detail::stream_handle{.engine = std::move(engine_stream), .connection = impl_->engine});
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   }
}

boost::asio::awaitable<void> connection::async_close() {
   if (impl_ && impl_->engine) {
      try {
         co_await impl_->engine->async_close();
      } catch (const detail::engine_failure& error) {
         raise_engine_failure(error);
      }
   }
   co_return;
}

void connection::cancel() {
   if (impl_ && impl_->engine) {
      impl_->engine->cancel();
   }
}

void connection::request_cancel() noexcept {
   if (impl_ && impl_->engine) {
      impl_->engine->request_cancel();
   }
}

connection detail::connection_access::make(detail::connection_handle handle) {
   return connection{std::move(handle)};
}

std::shared_ptr<void> detail::connection_access::take_inbound_admission(connection& value) noexcept {
   return value.impl_ && value.impl_->engine ? value.impl_->engine->take_inbound_admission() : std::shared_ptr<void>{};
}

} // namespace forge::net::quic
