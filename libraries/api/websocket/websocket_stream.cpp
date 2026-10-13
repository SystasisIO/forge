module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module forge.api.websocket.stream;

import forge.net.transport.buffer;
import forge.net.transport.exceptions;
import forge.net.transport.frame;
import forge.net.websocket.exceptions;

#include "details/websocket_stream.hxx"

namespace forge::api::websocket::detail {
namespace {

constexpr auto frame_prefix_size = std::size_t{4};

[[nodiscard]] std::size_t checked_message_size(std::uint32_t max_frame_size) {
   const auto value = std::uint64_t{max_frame_size} + frame_prefix_size;
   if (max_frame_size == 0 ||
       value > std::numeric_limits<std::size_t>::max()) {
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::invalid_buffer,
                            "invalid WebSocket API frame limit");
   }
   return static_cast<std::size_t>(value);
}

[[nodiscard]] std::exception_ptr make_closed_error() noexcept {
   try {
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed,
                            "WebSocket transport stream is closed");
   } catch (...) {
      return std::current_exception();
   }
}

[[nodiscard]] std::exception_ptr make_cancelled_error() noexcept {
   try {
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::canceled,
                            "WebSocket transport stream was cancelled");
   } catch (...) {
      return std::current_exception();
   }
}

[[nodiscard]] std::exception_ptr make_frame_too_large_error() noexcept {
   try {
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::frame_too_large,
                            "WebSocket API message exceeds the configured frame limit");
   } catch (...) {
      return std::current_exception();
   }
}

[[nodiscard]] std::exception_ptr make_invalid_frame_error() noexcept {
   try {
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::invalid_buffer,
                            "WebSocket API message must contain one binary transport frame");
   } catch (...) {
      return std::current_exception();
   }
}

} // namespace

websocket_stream::websocket_stream(
   forge::net::websocket::connection::ptr connection,
   std::uint32_t max_frame_size, std::uint64_t max_buffered_bytes)
    : connection_{std::move(connection)},
      max_frame_size_{max_frame_size},
      max_message_size_{checked_message_size(max_frame_size)},
      max_buffered_bytes_{static_cast<std::size_t>(std::min<std::uint64_t>(
         max_buffered_bytes, std::numeric_limits<std::size_t>::max()))} {
   if (!connection_) {
      FORGE_THROW_EXCEPTION(forge::net::websocket::exceptions::closed,
                            "WebSocket API binding received a null connection");
   }
   if (max_buffered_bytes_ == 0 || max_message_size_ > max_buffered_bytes_) {
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::invalid_buffer,
                            "WebSocket API buffer limit is smaller than one frame");
   }
}

void websocket_stream::install_handlers() {
   auto weak = weak_from_this();
   connection_->on_received_message(
      [weak](forge::net::websocket::connection&,
             forge::net::websocket::received_message message)
         -> boost::asio::awaitable<void> {
         if (auto self = weak.lock()) {
            co_await self->accept_message(std::move(message));
         }
      });
   connection_->on_close([weak](forge::net::websocket::connection&) {
      if (auto self = weak.lock()) {
         self->close_state(make_closed_error());
      }
   });
}

bool websocket_stream::valid() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return !closed_ && static_cast<bool>(connection_);
}

std::int64_t websocket_stream::id() const noexcept {
   return static_cast<std::int64_t>(
      reinterpret_cast<std::intptr_t>(connection_.get()));
}

boost::asio::awaitable<void>
websocket_stream::async_write(std::span<const std::uint8_t> bytes) {
   auto owned = forge::net::transport::chunk{bytes};
   co_await async_write_chunk(std::move(owned));
}

boost::asio::awaitable<void>
websocket_stream::async_write_chunk(forge::net::transport::chunk bytes) {
   const auto executor = co_await boost::asio::this_coro::executor;
   remember_executor(executor);
   const auto size = bytes.size();
   if (size > max_message_size_) {
      auto error = make_frame_too_large_error();
      close_state(error);
      std::rethrow_exception(error);
   }

   co_await reserve_outbound(size);
   try {
      const auto view = bytes.bytes();
      co_await connection_->send_binary(
         std::string{reinterpret_cast<const char*>(view.data()), view.size()});
      release_outbound(size);
   } catch (...) {
      auto error = std::current_exception();
      release_outbound(size);
      close_state(error);
      throw;
   }
}

boost::asio::awaitable<std::vector<std::uint8_t>>
websocket_stream::async_read() {
   co_return std::move(co_await async_read_chunk()).into_vector();
}

boost::asio::awaitable<forge::net::transport::chunk>
websocket_stream::async_read_chunk() {
   const auto executor = co_await boost::asio::this_coro::executor;
   remember_executor(executor);
   for (;;) {
      auto wait = std::shared_ptr<timer>{};
      auto wake_inbound = std::vector<std::weak_ptr<timer>>{};
      auto failure = std::exception_ptr{};
      {
         const auto lock = std::scoped_lock{mutex_};
         if (!inbound_.empty()) {
            auto value = std::move(inbound_.front());
            inbound_.pop_front();
            inbound_bytes_ -= value.size();
            wake_inbound.swap(inbound_waiters_);
            wake(std::move(wake_inbound));
            co_return value;
         }
         if (closed_) {
            failure = failure_ ? failure_ : make_closed_error();
         } else {
            wait = std::make_shared<timer>(executor, timer::time_point::max());
            read_waiters_.push_back(wait);
         }
      }
      if (failure) {
         std::rethrow_exception(failure);
      }

      auto error = boost::system::error_code{};
      co_await wait->async_wait(
         boost::asio::redirect_error(boost::asio::use_awaitable, error));
      const auto cancellation =
         co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         cancel();
         FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::canceled,
                               "WebSocket transport read was cancelled");
      }
   }
}

boost::asio::awaitable<void> websocket_stream::async_close() {
   const auto executor = co_await boost::asio::this_coro::executor;
   remember_executor(executor);
   close_state(make_closed_error());
   co_await connection_->close();
}

void websocket_stream::cancel() {
   close_state(make_cancelled_error());
   connection_->cancel();
}

boost::asio::awaitable<void>
websocket_stream::accept_message(
   forge::net::websocket::received_message message) {
   const auto executor = co_await boost::asio::this_coro::executor;
   remember_executor(executor);
   if (!message.binary) {
      auto error = make_invalid_frame_error();
      close_state(error);
      std::rethrow_exception(error);
   }
   if (message.payload.size() > max_message_size_) {
      auto error = make_frame_too_large_error();
      close_state(error);
      std::rethrow_exception(error);
   }

   auto storage = std::vector<std::uint8_t>{message.payload.begin(), message.payload.end()};
   try {
      const auto decoded = forge::net::transport::decode_frame_view(
         storage, forge::net::transport::frame_options{.max_size = max_frame_size_});
      if (decoded.status != forge::net::transport::frame_decode_status::complete ||
          decoded.consumed != storage.size()) {
         auto error = make_invalid_frame_error();
         close_state(error);
         std::rethrow_exception(error);
      }
   } catch (...) {
      close_state(std::current_exception());
      throw;
   }
   for (;;) {
      auto wait = std::shared_ptr<timer>{};
      auto wake_reads = std::vector<std::weak_ptr<timer>>{};
      auto failure = std::exception_ptr{};
      {
         const auto lock = std::scoped_lock{mutex_};
         if (closed_) {
            failure = failure_ ? failure_ : make_closed_error();
         } else if (storage.size() <= max_buffered_bytes_ - inbound_bytes_) {
            inbound_bytes_ += storage.size();
            inbound_.emplace_back(std::move(storage));
            wake_reads.swap(read_waiters_);
         } else {
            wait = std::make_shared<timer>(executor, timer::time_point::max());
            inbound_waiters_.push_back(wait);
         }
      }
      wake(std::move(wake_reads));
      if (failure) {
         std::rethrow_exception(failure);
      }
      if (!wait) {
         co_return;
      }
      auto error = boost::system::error_code{};
      co_await wait->async_wait(
         boost::asio::redirect_error(boost::asio::use_awaitable, error));
   }
}

boost::asio::awaitable<void>
websocket_stream::reserve_outbound(std::size_t bytes) {
   const auto executor = co_await boost::asio::this_coro::executor;
   for (;;) {
      auto wait = std::shared_ptr<timer>{};
      auto failure = std::exception_ptr{};
      {
         const auto lock = std::scoped_lock{mutex_};
         if (closed_) {
            failure = failure_ ? failure_ : make_closed_error();
         } else if (bytes <= max_buffered_bytes_ - outbound_bytes_) {
            outbound_bytes_ += bytes;
         } else {
            wait = std::make_shared<timer>(executor, timer::time_point::max());
            outbound_waiters_.push_back(wait);
         }
      }
      if (failure) {
         std::rethrow_exception(failure);
      }
      if (!wait) {
         co_return;
      }
      auto error = boost::system::error_code{};
      co_await wait->async_wait(
         boost::asio::redirect_error(boost::asio::use_awaitable, error));
      const auto cancellation =
         co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         cancel();
         FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::canceled,
                               "WebSocket transport write was cancelled");
      }
   }
}

void websocket_stream::release_outbound(std::size_t bytes) noexcept {
   auto waiters = std::vector<std::weak_ptr<timer>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      outbound_bytes_ -= std::min(outbound_bytes_, bytes);
      waiters.swap(outbound_waiters_);
   }
   wake(std::move(waiters));
}

void websocket_stream::remember_executor(
   boost::asio::any_io_executor executor) {
   const auto lock = std::scoped_lock{mutex_};
   if (!executor_) {
      executor_ = std::move(executor);
   }
}

void websocket_stream::close_state(std::exception_ptr failure) noexcept {
   auto read_waiters = std::vector<std::weak_ptr<timer>>{};
   auto inbound_waiters = std::vector<std::weak_ptr<timer>>{};
   auto outbound_waiters = std::vector<std::weak_ptr<timer>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         return;
      }
      closed_ = true;
      failure_ = std::move(failure);
      read_waiters.swap(read_waiters_);
      inbound_waiters.swap(inbound_waiters_);
      outbound_waiters.swap(outbound_waiters_);
   }
   wake(std::move(read_waiters));
   wake(std::move(inbound_waiters));
   wake(std::move(outbound_waiters));
}

void websocket_stream::wake(
   std::vector<std::weak_ptr<timer>> waiters) noexcept {
   for (auto& weak : waiters) {
      if (auto value = weak.lock()) {
         try {
            boost::asio::post(value->get_executor(), [value] {
               try {
                  value->cancel();
               } catch (...) {
                  // A terminal wake is best effort if the timer is gone.
               }
            });
         } catch (...) {
            // A terminal wake is best effort if its executor is already gone.
         }
      }
   }
}

} // namespace forge::api::websocket::detail
