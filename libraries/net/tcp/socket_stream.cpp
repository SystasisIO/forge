module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include "details/connection_test_hooks.hxx"

module forge.net.tcp.connection;

import forge.asio.notification;
import forge.net.transport.stream;

#include "details/socket_stream.hxx"

namespace forge::net::tcp::detail {
namespace asio = boost::asio;
using asio_tcp = boost::asio::ip::tcp;

[[noreturn]] void throw_io_error(std::string message, const boost::system::error_code& error) {
   FORGE_THROW_EXCEPTION(exceptions::io_error, std::move(message), forge::exceptions::ctx("reason", error.message()));
}

[[noreturn]] void throw_read_write_error(const boost::system::error_code& error) {
   if (error == boost::asio::error::operation_aborted) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "tcp connection operation canceled",
                            forge::exceptions::ctx("reason", error.message()));
   }
   if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset ||
       error == boost::asio::error::broken_pipe) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "tcp connection closed",
                            forge::exceptions::ctx("reason", error.message()));
   }
   throw_io_error("tcp connection I/O failed", error);
}

void cancel_socket(asio_tcp::socket& socket,
                   const std::shared_ptr<detail::connection_test_hooks>& hooks) noexcept {
   if (!socket.is_open()) {
      return;
   }
   if (hooks && hooks->native_close) {
      hooks->native_close(hooks->state.get(), socket, detail::native_close_stage::before_close);
   }
   auto ignored = boost::system::error_code{};
   socket.cancel(ignored);
   socket.shutdown(asio_tcp::socket::shutdown_both, ignored);
   socket.close(ignored);
   if (hooks && hooks->native_close) {
      hooks->native_close(hooks->state.get(), socket, detail::native_close_stage::after_close);
   }
}

socket_stream::socket_stream(std::shared_ptr<asio_tcp::socket> socket, asio::strand<asio::any_io_executor> strand,
                             options tcp_options, std::int64_t id, std::shared_ptr<std::atomic<socket_state>> state,
                             std::shared_ptr<forge::asio::notification> terminal_requested,
                             std::shared_ptr<forge::asio::notification> terminal_completed, std::shared_ptr<void> lifetime)
    : socket_(std::move(socket)), strand_(std::move(strand)), options_(tcp_options), id_(id),
      state_(std::move(state)), terminal_requested_(std::move(terminal_requested)),
      terminal_completed_(std::move(terminal_completed)), lifetime_(std::move(lifetime)) {}

socket_stream::~socket_stream() {
   request_cancel();
}

void socket_stream::activate() noexcept {
   ownership_active_.store(true, std::memory_order_release);
}

[[nodiscard]] bool socket_stream::valid() const noexcept {
   return ownership_active_.load(std::memory_order_acquire) &&
          state_->load(std::memory_order_acquire) == socket_state::active;
}

[[nodiscard]] std::int64_t socket_stream::id() const noexcept {
   return id_;
}

boost::asio::awaitable<void> socket_stream::async_write(std::span<const std::uint8_t> bytes) {
   auto self = shared_from_this();
   co_await asio::co_spawn(
       strand_,
       [self = std::move(self), bytes]() -> asio::awaitable<void> {
          if (!self->valid()) {
             FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp stream");
          }
          auto error = boost::system::error_code{};
          co_await asio::async_write(*self->socket_, asio::buffer(bytes),
                                     asio::redirect_error(asio::use_awaitable, error));
          if (error) {
             throw_read_write_error(error);
          }
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::vector<std::uint8_t>> socket_stream::async_read() {
   auto self = shared_from_this();
   auto out = std::vector<std::uint8_t>(options_.read_chunk_size);
   const auto size = co_await asio::co_spawn(
       strand_,
       [self = std::move(self), writable = std::span<std::uint8_t>{out}]() -> asio::awaitable<std::size_t> {
          if (!self->valid()) {
             FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp stream");
          }
          auto error = boost::system::error_code{};
          const auto size = co_await self->socket_->async_read_some(
              asio::buffer(writable), asio::redirect_error(asio::use_awaitable, error));
          if (error) {
             throw_read_write_error(error);
          }
          co_return size;
       },
       asio::use_awaitable);
   out.resize(size);
   co_return out;
}

boost::asio::awaitable<transport::chunk> socket_stream::async_read_chunk() {
   auto builder = pool_.acquire(options_.read_chunk_size);
   auto writable = builder.writable();
   auto self = shared_from_this();
   const auto size = co_await asio::co_spawn(
       strand_,
       [self = std::move(self), writable]() -> asio::awaitable<std::size_t> {
          if (!self->valid()) {
             FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp stream");
          }
          auto error = boost::system::error_code{};
          const auto size = co_await self->socket_->async_read_some(
              asio::buffer(writable), asio::redirect_error(asio::use_awaitable, error));
          if (error) {
             throw_read_write_error(error);
          }
          co_return size;
       },
       asio::use_awaitable);
   co_return builder.commit(size);
}

boost::asio::awaitable<void> socket_stream::async_close() {
   if (request_terminal(socket_state::close_requested)) {
      terminal_requested_->notify();
   }
   static_cast<void>(co_await terminal_completed_->async_wait(0));
}

void socket_stream::cancel() {
   request_cancel();
}

void socket_stream::request_cancel() noexcept {
   if (request_terminal(socket_state::cancel_requested)) {
      terminal_requested_->notify();
   }
}

[[nodiscard]] bool socket_stream::request_terminal(socket_state requested) noexcept {
   if (!ownership_active_.load(std::memory_order_acquire)) {
      return false;
   }
   auto expected = socket_state::active;
   return state_->compare_exchange_strong(expected, requested, std::memory_order_acq_rel, std::memory_order_acquire);
}

[[nodiscard]] std::pair<std::shared_ptr<socket_stream>, transport::stream>
prepare_stream(std::shared_ptr<asio_tcp::socket> socket, asio::strand<asio::any_io_executor> strand,
               options tcp_options, std::int64_t id, std::shared_ptr<std::atomic<socket_state>> state,
               std::shared_ptr<forge::asio::notification> terminal_requested,
               std::shared_ptr<forge::asio::notification> terminal_completed, std::shared_ptr<void> lifetime) {
   auto model = std::make_shared<socket_stream>(std::move(socket), std::move(strand), tcp_options, id, std::move(state),
                                                std::move(terminal_requested), std::move(terminal_completed),
                                                std::move(lifetime));
   auto weak = std::weak_ptr<socket_stream>{model};
   auto stream = transport::detail::stream_access::make_cancelable(model, [weak = std::move(weak)]() noexcept {
      if (auto value = weak.lock()) {
         value->request_cancel();
      }
   });
   return {std::move(model), std::move(stream)};
}

} // namespace forge::net::tcp::detail
