module;

#include <coroutine>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "details/router_server_access.hxx"
#include "details/stream_server_access.hxx"
#include "details/server_log.hxx"
#include "details/request_read_ownership.hxx"

#include <forge/exceptions/macros.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/scope/scope_exit.hpp>
#include <openssl/ssl.h>

module forge.net.http.server;

import forge.asio.exceptions;
import forge.asio.runtime;
import forge.crypto.digest.sha256;
import forge.net.http.body;
import forge.net.http.exceptions;
import forge.net.http.negotiation;
import forge.net.http.route_context;
import forge.net.http.stream;
import forge.net.tls.context;
import forge.net.websocket.connection;

#include "details/server_session.hxx"

#include "details/beast_body_reader_source.hxx"

namespace forge::net::http::detail {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
using asio::awaitable;
using asio::use_awaitable;

template <typename Stream>
beast_body_reader_source<Stream>::beast_body_reader_source(Stream& stream, beast::flat_buffer& buffer,
                         beast_http::request_parser<beast_http::buffer_body>& parser, stream_limits limits,
                         bool send_continue, std::shared_ptr<request_read_ownership> read_ownership,
                         server_log::request* access)
    : stream_(stream), buffer_(buffer), parser_(parser), limits_(limits), send_continue_(send_continue),
      read_ownership_(std::move(read_ownership)), access_(access) {}

template <typename Stream>
awaitable<std::optional<body_chunk>> beast_body_reader_source<Stream>::async_read() {
   try {
      if (parser_.is_done()) {
         co_return std::nullopt;
      }
      co_await send_continue_if_needed();

      const auto chunk_size = std::max<std::uint64_t>(1, limits_.max_chunk_bytes);
      for (;;) {
         auto storage = std::vector<std::byte>(static_cast<std::size_t>(chunk_size));
         auto& body = parser_.get().body();
         body.data = storage.data();
         body.size = storage.size();

         beast::get_lowest_layer(stream_).expires_after(limits_.read_timeout);
         if (read_ownership_) {
            co_await read_ownership_->begin_body_read();
         }
         auto read_scope = body_read_scope{read_ownership_};
         auto [read_error, bytes] =
             co_await beast_http::async_read_some(stream_, buffer_, parser_, asio::as_tuple(use_awaitable));
         access_->received_bytes += bytes;

         const auto produced = storage.size() - body.size;
         if (read_error == beast_http::error::need_buffer) {
            read_error = {};
         }
         if (read_error == beast_http::error::body_limit) {
            FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP request body is too large");
         }
         if (read_error) {
            if (read_error == asio::error::operation_aborted || read_error == asio::error::eof ||
                read_error == asio::error::connection_reset) {
               throw forge::asio::exceptions::canceled{"HTTP request body read was canceled"};
            }
            if (read_error == beast::error::timeout || read_error == asio::error::timed_out) {
               FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP request body read timed out");
            }
            FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP request body read failed",
                                  forge::exceptions::ctx("reason", read_error.message()));
         }

         bytes_read_ += produced;
         if (bytes_read_ > limits_.max_body_bytes) {
            FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP request body is too large");
         }

         if (produced != 0U) {
            storage.resize(produced);
            co_return body_chunk{.bytes = std::move(storage)};
         }
         if (parser_.is_done()) {
            co_return std::nullopt;
         }
      }
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const boost::system::system_error& error) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP request body reader failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP request body reader failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP request body reader failed");
   }
}

template <typename Stream>
std::uint64_t beast_body_reader_source<Stream>::bytes_read() const noexcept {
   return bytes_read_;
}

template <typename Stream>
bool beast_body_reader_source<Stream>::requires_continue_before_response() const noexcept {
   return send_continue_ && !parser_.is_done();
}

template <typename Stream>
bool beast_body_reader_source<Stream>::done() const noexcept {
   return parser_.is_done();
}

template <typename Stream>
awaitable<void> beast_body_reader_source<Stream>::send_continue_if_needed() {
   try {
      if (!send_continue_ || parser_.is_done()) {
         co_return;
      }
      send_continue_ = false;
      auto reply =
          beast_http::response<beast_http::empty_body>{beast_http::status::continue_, parser_.get().version()};
      reply.keep_alive(true);
      beast::get_lowest_layer(stream_).expires_after(limits_.write_timeout);
      auto [write_error, written] = co_await beast_http::async_write(stream_, reply, asio::as_tuple(use_awaitable));
      access_->sent_bytes += written;
      if (write_error) {
         if (write_error == asio::error::operation_aborted || write_error == asio::error::eof ||
             write_error == asio::error::connection_reset || write_error == asio::error::broken_pipe) {
            throw forge::asio::exceptions::canceled{"HTTP continue response was canceled"};
         }
         FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP continue response failed",
                               forge::exceptions::ctx("reason", write_error.message()));
      }
      beast::get_lowest_layer(stream_).expires_never();
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const boost::system::system_error& error) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP continue response failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP continue response failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP continue response failed");
   }
}

template class beast_body_reader_source<beast::tcp_stream>;
template class beast_body_reader_source<forge::net::tls::beast_tls_stream>;

} // namespace forge::net::http::detail
