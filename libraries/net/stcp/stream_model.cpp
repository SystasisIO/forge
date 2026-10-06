module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/ssl.h>
#include "details/handshake_deadline.hxx"
#include "details/connection_test_hooks.hxx"

module forge.net.stcp.connection;

import forge.asio.gate;
import forge.asio.notification;
import forge.net.tls.context;
import forge.net.tls.exceptions;
import forge.net.transport.stream;

#include "details/stream_model.hxx"

namespace forge::net::stcp::detail {
namespace asio = boost::asio;

stream_model::stream_model(asio::strand<asio::any_io_executor> strand, std::shared_ptr<io_gates> gates,
                           std::shared_ptr<forge::asio::notification> terminal_completed,
                           std::shared_ptr<std::exception_ptr> terminal_failure,
                           std::size_t read_chunk_size, std::int64_t id, std::shared_ptr<void> lifetime)
    : strand_(std::move(strand)), gates_(std::move(gates)), read_chunk_size_(read_chunk_size), id_(id),
      terminal_completed_(std::move(terminal_completed)), terminal_failure_(std::move(terminal_failure)),
      lifetime_(std::move(lifetime)) {}

stream_model::~stream_model() {
   request_cancel();
}

[[nodiscard]] bool stream_model::valid() const noexcept {
   static_cast<void>(lifetime_);
   return stream_ && stream_->is_open() && !gates_->stopped();
}

[[nodiscard]] std::int64_t stream_model::id() const noexcept {
   return id_;
}

void stream_model::attach(std::shared_ptr<detail::stream_backend> stream) noexcept {
   stream_ = std::move(stream);
}

boost::asio::awaitable<void> stream_model::async_write(std::span<const std::uint8_t> bytes) {
   auto write_ticket = co_await gates_->acquire(gates_->write);
   auto stream = stream_;
   auto gates = gates_;
   co_await asio::co_spawn(
       strand_,
       [stream = std::move(stream), gates = std::move(gates), bytes]() -> asio::awaitable<void> {
          if (gates->stopped()) {
             gates->throw_stopped();
          }
          if (!stream || !stream->is_open()) {
             terminalize_closed(stream.get(), *gates, "invalid stcp stream");
         }
          const auto error = co_await stream->async_write(bytes);
          if (error) {
             terminalize_io_error(*stream, *gates, error);
          }
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::vector<std::uint8_t>> stream_model::async_read() {
   auto read_ticket = co_await gates_->acquire(gates_->read);
   auto out = std::vector<std::uint8_t>(read_chunk_size_);
   auto stream = stream_;
   auto gates = gates_;
   const auto size = co_await asio::co_spawn(
       strand_,
       [stream = std::move(stream), gates = std::move(gates),
        writable = std::span<std::uint8_t>{out}]() -> asio::awaitable<std::size_t> {
          if (gates->stopped()) {
             gates->throw_stopped();
          }
          if (!stream || !stream->is_open()) {
             terminalize_closed(stream.get(), *gates, "invalid stcp stream");
         }
          const auto [error, size] = co_await stream->async_read_some(writable);
          if (error) {
             terminalize_io_error(*stream, *gates, error);
          }
          co_return size;
       },
       asio::use_awaitable);
   out.resize(size);
   co_return out;
}

boost::asio::awaitable<transport::chunk> stream_model::async_read_chunk() {
   auto read_ticket = co_await gates_->acquire(gates_->read);
   auto builder = pool_.acquire(read_chunk_size_);
   auto writable = builder.writable();
   auto stream = stream_;
   auto gates = gates_;
   const auto size = co_await asio::co_spawn(
       strand_,
       [stream = std::move(stream), gates = std::move(gates), writable]() -> asio::awaitable<std::size_t> {
          if (gates->stopped()) {
             gates->throw_stopped();
          }
          if (!stream || !stream->is_open()) {
             terminalize_closed(stream.get(), *gates, "invalid stcp stream");
         }
          const auto [error, size] = co_await stream->async_read_some(writable);
          if (error) {
             terminalize_io_error(*stream, *gates, error);
          }
          co_return size;
       },
       asio::use_awaitable);
   co_return builder.commit(size);
}

boost::asio::awaitable<void> stream_model::async_close() {
   if (!stream_) {
      co_return;
   }
   gates_->stop(io_stop_reason::closed);
   static_cast<void>(co_await terminal_completed_->async_wait(0));
   if (*terminal_failure_) {
      std::rethrow_exception(*terminal_failure_);
   }
}

void stream_model::cancel() {
   request_cancel();
}

void stream_model::request_cancel() noexcept {
   if (stream_) {
      gates_->stop(io_stop_reason::canceled);
   }
}

} // namespace forge::net::stcp::detail
