module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

module forge.api.http.binding;

import forge.api.core.binding;
import forge.api.core.exceptions;
import forge.api.core.error_projection;
import forge.api.core.stream_reader;
import forge.api.core.types;
import forge.api.core.invocation_context;
import forge.net.http.body;
import forge.raw.raw;

#include "details/stream_frame_decoder.hxx"
#include "details/server_stream_state.hxx"

namespace forge::api::http::detail {

server_stream_state::server_stream_state(boost::asio::any_io_executor executor,
                                         forge::api::core::pinned_binding_plan plan, forge::api::core::frame request,
                                         forge::api::core::trusted_invocation trusted, std::uint32_t max_frame_bytes,
                                         std::uint32_t max_item_bytes, std::uint32_t max_buffered_items,
                                         std::uint64_t max_buffered_bytes,
                                         forge::api::core::invocation_lifetime lifetime)
    : executor_{std::move(executor)}, plan_{std::move(plan)}, request_{std::move(request)},
      trusted_{std::move(trusted)},
      lifetime_{std::move(lifetime)},
      stream_{forge::api::core::detail::make_local_stream_pair(executor_, max_item_bytes, max_buffered_items,
                                                               static_cast<std::size_t>(max_buffered_bytes))},
      max_frame_bytes_{max_frame_bytes}, terminal_ready_{std::make_shared<boost::asio::steady_timer>(
                                             executor_, boost::asio::steady_timer::time_point::max())} {}

void server_stream_state::start() {
   if (cancelled_.load(std::memory_order_acquire) || started_.exchange(true, std::memory_order_acq_rel)) {
      return;
   }
   run_done_.store(false, std::memory_order_release);
   try {
      boost::asio::co_spawn(
         executor_, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->run();
         },
         boost::asio::bind_cancellation_slot(cancellation_.slot(),
            [self = shared_from_this()](std::exception_ptr) {
               self->run_done_.store(true, std::memory_order_release);
               try {
                  self->terminal_ready_->cancel();
               } catch (...) {
                  // The completed handler remains terminal even if its wake fails.
               }
            }));
   } catch (...) {
      run_done_.store(true, std::memory_order_release);
      throw;
   }
}

boost::asio::awaitable<void> server_stream_state::async_drain() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   cancel();
   while (!run_done_.load(std::memory_order_acquire)) {
      auto error = boost::system::error_code{};
      co_await terminal_ready_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   }
}

void server_stream_state::cancel() noexcept {
   if (cancelled_.exchange(true, std::memory_order_acq_rel)) {
      return;
   }
   const auto error = std::make_exception_ptr(
      forge::api::core::exceptions::cancelled{"HTTP API response stream was abandoned"});
   stream_.reader->fail(error);
   stream_.writer->fail(error);
   try {
      boost::asio::dispatch(executor_, [self = shared_from_this()] {
         self->cancellation_.emit(boost::asio::cancellation_type::all);
      });
   } catch (...) {
      // Stream endpoint failure still releases all bounded queues.
   }
}

boost::asio::awaitable<std::optional<forge::net::http::body_chunk>>
server_stream_state::async_next() {
   if (cancelled_.load(std::memory_order_acquire) && !started_.load(std::memory_order_acquire)) {
      throw forge::api::core::exceptions::cancelled{"HTTP API response stream was abandoned before consumption"};
   }
   start();
   if (!stream_end_sent_) {
      try {
         auto item = co_await stream_.reader->async_read();
         if (item) {
            co_return stream_frame_decoder::encode(
               forge::api::core::frame{
                  .kind = forge::api::core::frame_kind::stream_item,
                  .id = request_.id,
                  .api = request_.api,
                  .method = request_.method,
                  .codec = request_.codec,
                  .payload = std::move(*item),
               },
               max_frame_bytes_);
         }
      } catch (...) {
         // dispatch_stream publishes the projected terminal error separately.
      }
      stream_end_sent_ = true;
      co_return stream_frame_decoder::encode(
         forge::api::core::frame{
            .kind = forge::api::core::frame_kind::stream_end,
            .id = request_.id,
            .api = request_.api,
            .method = request_.method,
            .codec = request_.codec,
            .payload = forge::raw::pack(forge::api::core::stream_end{
               .direction = forge::api::core::stream_direction::output}),
         },
         max_frame_bytes_);
   }

   if (!terminal_sent_) {
      terminal_sent_ = true;
      co_return stream_frame_decoder::encode(co_await wait_terminal(), max_frame_bytes_);
   }
   co_return std::nullopt;
}

boost::asio::awaitable<void> server_stream_state::run() {
   try {
      auto terminal = co_await forge::api::core::enforce_invocation(
         plan_.dispatch_stream_contextual(request_, {}, stream_.writer, trusted_),
         forge::api::core::invocation_context{trusted_, lifetime_},
         [this](std::exception_ptr error) {
            stream_.reader->fail(error);
            stream_.writer->fail(error);
         });
      publish_terminal(std::move(terminal));
   } catch (const forge::api::core::exceptions::invocation_denied& error) {
      const auto failure = std::current_exception();
      stream_.reader->fail(failure);
      stream_.writer->fail(failure);
      auto terminal = internal_error();
      terminal.payload = forge::raw::pack(forge::api::core::make_core_error_payload(
         forge::api::core::exceptions::code::invocation_denied, error.message()));
      publish_terminal(std::move(terminal));
   } catch (...) {
      const auto error = std::current_exception();
      stream_.reader->fail(error);
      stream_.writer->fail(error);
      publish_terminal(internal_error());
   }
}

boost::asio::awaitable<forge::api::core::frame>
server_stream_state::wait_terminal() {
   for (;;) {
      {
         const auto lock = std::scoped_lock{mutex_};
         if (terminal_) {
            co_return *terminal_;
         }
      }
      auto error = boost::system::error_code{};
      co_await terminal_ready_->async_wait(
         boost::asio::redirect_error(boost::asio::use_awaitable, error));
      const auto cancellation =
         co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         throw forge::api::core::exceptions::cancelled{
            "HTTP API response stream was cancelled"};
      }
   }
}

void server_stream_state::publish_terminal(forge::api::core::frame value) noexcept {
   {
      const auto lock = std::scoped_lock{mutex_};
      if (terminal_) {
         return;
      }
      terminal_ = std::move(value);
   }
   boost::asio::dispatch(terminal_ready_->get_executor(), [ready = terminal_ready_] {
      try {
         ready->cancel();
      } catch (...) {
         // Completion notification is best effort during shutdown.
      }
   });
}

forge::api::core::frame server_stream_state::internal_error() const {
   return forge::api::core::frame{
      .kind = forge::api::core::frame_kind::error,
      .id = request_.id,
      .api = request_.api,
      .method = request_.method,
      .codec = request_.codec,
      .payload = forge::raw::pack(forge::api::core::make_internal_error_payload()),
   };
}

} // namespace forge::api::http::detail
