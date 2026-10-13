module;

#include <forge/exceptions/macros.hpp>

#include "details/wrapper_handles.hxx"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

module forge.net.quic.stream;

import forge.net.quic.exceptions;

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
   case detail::engine_error_kind::end_of_stream:
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
   if (error.kind() == detail::engine_error_kind::end_of_stream) {
      FORGE_THROW_EXCEPTION(exceptions::end_of_stream, error.what());
   }
   FORGE_THROW_CODE(map_error(error.kind()), error.what());
}

} // namespace

struct stream::impl {
   std::shared_ptr<detail::engine_stream> engine;
   std::shared_ptr<detail::engine_connection> connection;
};

stream::stream() = default;

stream::stream(detail::stream_handle handle)
    : impl_(std::make_shared<impl>(
          impl{.engine = std::move(handle.engine), .connection = std::move(handle.connection)})) {}

stream::~stream() = default;

stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;

bool stream::valid() const noexcept {
   return impl_ != nullptr;
}

std::int64_t stream::id() const noexcept {
   return impl_ && impl_->engine ? impl_->engine->id() : -1;
}

boost::asio::awaitable<void> stream::async_write(std::span<const std::uint8_t> bytes) {
   if (!impl_ || !impl_->engine) {
      FORGE_THROW_EXCEPTION(exceptions::stream_closed, "invalid QUIC stream");
   }
   try {
      co_await impl_->engine->async_write(bytes);
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   }
   co_return;
}

boost::asio::awaitable<std::vector<std::uint8_t>> stream::async_read() {
   if (!impl_ || !impl_->engine) {
      FORGE_THROW_EXCEPTION(exceptions::stream_closed, "invalid QUIC stream");
   }
   try {
      co_return co_await impl_->engine->async_read();
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   }
}

boost::asio::awaitable<void> stream::async_close_write() {
   if (!impl_ || !impl_->engine) {
      FORGE_THROW_EXCEPTION(exceptions::stream_closed, "invalid QUIC stream");
   }
   try {
      co_await impl_->engine->async_close_write();
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   }
}

boost::asio::awaitable<void> stream::async_close() {
   if (impl_ && impl_->engine) {
      try {
         co_await impl_->engine->async_close();
      } catch (const detail::engine_failure& error) {
         raise_engine_failure(error);
      }
   }
   co_return;
}

void stream::cancel() {
   if (impl_ && impl_->engine) {
      impl_->engine->cancel();
   }
}

void stream::request_cancel() noexcept {
   if (impl_ && impl_->engine) {
      impl_->engine->request_cancel();
   }
}

stream detail::stream_access::make(detail::stream_handle handle) {
   return stream{std::move(handle)};
}

boost::asio::awaitable<void> detail::stream_access::async_write_chunk(stream& value,
                                                                      forge::net::transport::chunk bytes) {
   if (!value.impl_ || !value.impl_->engine) {
      FORGE_THROW_EXCEPTION(exceptions::stream_closed, "invalid QUIC stream");
   }
   auto [owned, lifetime] = forge::net::transport::detail::chunk_access::consume(std::move(bytes));
   try {
      co_await value.impl_->engine->async_write(std::move(owned), std::move(lifetime));
   } catch (const detail::engine_failure& error) {
      raise_engine_failure(error);
   }
}

void detail::stream_access::cancel_write(stream& value) {
   if (value.impl_ && value.impl_->engine) {
      value.impl_->engine->cancel_write();
   }
}

} // namespace forge::net::quic
