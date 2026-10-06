module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/ssl.h>
#include "details/connection_test_hooks.hxx"

module forge.net.stcp.connection;

import forge.net.tls.context;
import forge.net.transport.stream;

#include "details/transport_stream_adapter.hxx"
#include "details/stream_backend.hxx"

namespace forge::net::stcp::detail {
namespace {

template <typename stream_type>
boost::asio::awaitable<boost::system::error_code>
run_handshake(stream_type& stream, boost::asio::ssl::stream_base::handshake_type type) {
   auto error = boost::system::error_code{};
   co_await stream.async_handshake(type, boost::asio::redirect_error(boost::asio::use_awaitable, error));
   co_return error;
}

template <typename stream_type>
boost::asio::awaitable<boost::system::error_code> run_write(stream_type& stream, std::span<const std::uint8_t> bytes) {
   auto error = boost::system::error_code{};
   co_await boost::asio::async_write(stream, boost::asio::buffer(bytes),
                                     boost::asio::redirect_error(boost::asio::use_awaitable, error));
   co_return error;
}

template <typename stream_type>
boost::asio::awaitable<std::pair<boost::system::error_code, std::size_t>>
run_read_some(stream_type& stream, std::span<std::uint8_t> bytes) {
   auto error = boost::system::error_code{};
   const auto size = co_await stream.async_read_some(boost::asio::buffer(bytes),
                                                      boost::asio::redirect_error(boost::asio::use_awaitable, error));
   co_return std::pair{error, size};
}

} // namespace

void close_native_socket(boost::asio::ip::tcp::socket& socket,
                         const std::shared_ptr<connection_test_hooks>& hooks) noexcept {
   if (!socket.is_open()) {
      return;
   }
   if (hooks && hooks->native_close) {
      hooks->native_close(hooks->state.get(), socket, native_close_stage::before_close);
   }
   auto ignored = boost::system::error_code{};
   socket.cancel(ignored);
   socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
   socket.close(ignored);
   if (hooks && hooks->native_close) {
      hooks->native_close(hooks->state.get(), socket, native_close_stage::after_close);
   }
}

native_stream_backend::native_stream_backend(std::shared_ptr<forge::net::tls::asio_tls_stream> stream,
                                             std::shared_ptr<void> lifetime,
                                             std::shared_ptr<connection_test_hooks> hooks)
    : stream_(std::move(stream)), lifetime_(std::move(lifetime)), hooks_(std::move(hooks)) {}

native_stream_backend::~native_stream_backend() {
   // Timer and worker references retain this real native owner, not just the
   // wrapper. Even pre-publication failure closes the socket before its token.
   request_cancel();
}

boost::asio::any_io_executor native_stream_backend::get_executor() const noexcept {
   return stream_->get_executor();
}

bool native_stream_backend::is_open() const noexcept {
   return stream_ && stream_->lowest_layer().is_open();
}

SSL* native_stream_backend::native_handle() const noexcept {
   return stream_ ? stream_->native_handle() : nullptr;
}

boost::asio::awaitable<boost::system::error_code>
native_stream_backend::async_handshake(boost::asio::ssl::stream_base::handshake_type type) {
   co_return co_await run_handshake(*stream_, type);
}

boost::asio::awaitable<boost::system::error_code> native_stream_backend::async_write(std::span<const std::uint8_t> bytes) {
   co_return co_await run_write(*stream_, bytes);
}

boost::asio::awaitable<std::pair<boost::system::error_code, std::size_t>>
native_stream_backend::async_read_some(std::span<std::uint8_t> bytes) {
   co_return co_await run_read_some(*stream_, bytes);
}

void native_stream_backend::request_cancel() noexcept {
   const auto lock = std::scoped_lock{close_mutex_};
   if (stream_) {
      close_native_socket(stream_->next_layer(), hooks_);
   }
}

boost::asio::awaitable<void> native_stream_backend::async_terminal_close() {
   request_cancel();
   co_return;
}

transport_stream_backend::transport_stream_backend(std::shared_ptr<tls_stream> stream) : stream_(std::move(stream)) {}

boost::asio::any_io_executor transport_stream_backend::get_executor() const noexcept {
   return stream_->get_executor();
}

bool transport_stream_backend::is_open() const noexcept {
   return stream_ && stream_->next_layer().valid();
}

SSL* transport_stream_backend::native_handle() const noexcept {
   return stream_ ? stream_->native_handle() : nullptr;
}

boost::asio::awaitable<boost::system::error_code>
transport_stream_backend::async_handshake(boost::asio::ssl::stream_base::handshake_type type) {
   co_return co_await run_handshake(*stream_, type);
}

boost::asio::awaitable<boost::system::error_code>
transport_stream_backend::async_write(std::span<const std::uint8_t> bytes) {
   co_return co_await run_write(*stream_, bytes);
}

boost::asio::awaitable<std::pair<boost::system::error_code, std::size_t>>
transport_stream_backend::async_read_some(std::span<std::uint8_t> bytes) {
   co_return co_await run_read_some(*stream_, bytes);
}

void transport_stream_backend::request_cancel() noexcept {
   if (stream_) {
      stream_->next_layer().request_cancel();
   }
}

boost::asio::awaitable<void> transport_stream_backend::async_terminal_close() {
   if (stream_) {
      co_await stream_->next_layer().async_close();
   }
}

std::shared_ptr<stream_backend>
make_native_stream_backend(std::shared_ptr<forge::net::tls::asio_tls_stream> stream,
                           std::shared_ptr<void> lifetime, std::shared_ptr<connection_test_hooks> hooks) {
   try {
      if (hooks && hooks->startup) {
         hooks->startup(hooks->state.get(), startup_stage::backend_allocation);
      }
      return std::make_shared<native_stream_backend>(stream, lifetime, hooks);
   } catch (...) {
      close_native_socket(stream->next_layer(), hooks);
      throw;
   }
}

std::shared_ptr<stream_backend>
make_transport_stream_backend(boost::asio::any_io_executor executor, forge::net::transport::stream stream,
                              forge::net::tls::context_snapshot_ptr context) {
   auto adapter = transport_stream_adapter{std::move(executor), std::move(stream)};
   auto tls_stream = forge::net::tls::make_asio_stream(std::move(context), std::move(adapter));
   return std::make_shared<transport_stream_backend>(std::move(tls_stream));
}

} // namespace forge::net::stcp::detail
