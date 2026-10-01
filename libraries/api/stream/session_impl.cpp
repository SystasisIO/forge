module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module forge.api.stream.session;

import forge.asio.notification;
import forge.net.transport.exceptions;
import forge.net.transport.frame;
import forge.raw.exceptions;
import forge.raw.raw;

#include "details/session_impl.hxx"

namespace forge::api::stream {
namespace {

constexpr auto aggregate_inbound_limit = std::uint64_t{16U * 1024U * 1024U};

[[nodiscard]] forge::raw::unpack_limits wire_unpack_limits(std::size_t payload_size, std::uint32_t max_frame_size) {
   const auto bounded = static_cast<std::uint32_t>(std::min<std::size_t>(payload_size, max_frame_size));
   return forge::raw::unpack_limits{
       .max_container_elements = bounded,
       .max_total_container_elements = bounded,
       .max_bytes = bounded,
       .first_container_elements = bounded,
   };
}

[[nodiscard]] std::uint32_t clamp_u32(std::size_t value) {
   return static_cast<std::uint32_t>(std::min<std::size_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] std::uint32_t fair_inflight_limit(const forge::api::core::session_limits& limits) {
   const auto window_limited = limits.max_buffered_bytes / limits.initial_window_bytes;
   return static_cast<std::uint32_t>(std::min<std::uint64_t>(limits.max_inflight_calls, window_limited));
}

[[nodiscard]] std::uint32_t negotiated_timeout(std::chrono::milliseconds value) {
   if (value.count() <= 0) {
      return 0;
   }
   return clamp_u32(static_cast<std::size_t>(value.count()));
}

template <typename T>
[[nodiscard]] T decode_control_payload(const forge::api::core::frame& value, std::uint32_t max_frame_size) {
   try {
      return forge::raw::unpack_exact<T>(value.payload, wire_unpack_limits(value.payload.size(), max_frame_size));
   } catch (const forge::raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                            "API stream control frame exceeds decode limits");
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                            "API stream control frame allocation failed");
   } catch (const forge::raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream control frame is malformed");
   } catch (const forge::raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream control frame is malformed");
   }
}

} // namespace

session::impl::write_receipt::write_receipt(const strand_type& executor) : wake{executor} {
   wake.expires_at(timer::time_point::max());
}

session::impl::call_state::call_state(const strand_type& executor, forge::api::core::call_id value,
                                      forge::api::core::method_kind method_kind, bool local_origin_value)
    : id{value}, kind{method_kind}, local_origin{local_origin_value}, wake{executor}, deadline{executor} {
   wake.expires_at(timer::time_point::max());
   deadline.expires_at(timer::time_point::max());
}

session::impl::impl(forge::net::transport::stream stream_value, options settings_value)
    : stream{std::move(stream_value)}, settings{std::move(settings_value)} {
   validate_options();
}

session::impl::impl(forge::net::transport::stream stream_value, forge::api::core::binding_plan plan_value,
                    options settings_value, forge::api::core::dispatch_options dispatch_options,
                    std::optional<forge::api::core::trusted_invocation> trusted)
    : stream{std::move(stream_value)}, settings{std::move(settings_value)}, plan{std::move(plan_value)} {
   validate_options();
   next_call_id = std::uint64_t{1} << 63U;
   next_remote_call_id = 1;
   dispatch_options.codec = settings.codec;
   dispatch_options.max_inflight = settings.max_inflight;
   dispatch_options.deadline = settings.deadline;
   if (trusted) {
      dispatcher.emplace(*plan, std::move(dispatch_options), std::move(*trusted));
   } else {
      dispatcher.emplace(*plan, std::move(dispatch_options));
   }
}

bool session::impl::valid() const noexcept {
   return !closed.load(std::memory_order_acquire) && stream.valid();
}

session::impl::strand_type session::impl::ensure_strand(boost::asio::any_io_executor executor) {
   const auto lock = std::scoped_lock{executor_mutex};
   if (!strand) {
      strand.emplace(boost::asio::make_strand(std::move(executor)));
   }
   return *strand;
}

std::optional<session::impl::strand_type> session::impl::current_strand() const {
   const auto lock = std::scoped_lock{executor_mutex};
   return strand;
}

void session::impl::validate_options() const {
   if (!stream.valid()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream session requires a valid stream");
   }
   if (settings.version.major != 2 || settings.codec.value.empty() || settings.max_inflight == 0 ||
       settings.max_frame_size == 0 || settings.max_item_size == 0 || settings.initial_window_items == 0 ||
       settings.initial_window_bytes == 0 || settings.max_buffered_bytes == 0 || settings.max_tombstones == 0 ||
       settings.max_buffered_bytes > aggregate_inbound_limit || settings.max_item_size > settings.max_frame_size ||
       settings.max_item_size > settings.max_buffered_bytes ||
       settings.initial_window_bytes > settings.max_buffered_bytes || settings.control_timeout.count() <= 0 ||
       settings.disconnect_grace.count() < 0) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "invalid API stream session options");
   }
}

forge::api::core::session_hello session::impl::local_hello() const {
   auto result = forge::api::core::session_hello{
       .version = settings.version,
       .capabilities = settings.capabilities,
       .codec = settings.codec,
       .limits =
           {
               .max_frame_bytes = settings.max_frame_size,
               .max_item_bytes = settings.max_item_size,
               .max_inflight_calls = clamp_u32(settings.max_inflight),
               .initial_window_items = settings.initial_window_items,
               .initial_window_bytes = settings.initial_window_bytes,
               .max_buffered_bytes = settings.max_buffered_bytes,
               .idle_timeout_ms = negotiated_timeout(settings.idle_timeout),
               .shutdown_grace_ms = negotiated_timeout(settings.disconnect_grace),
           },
   };
   result.limits.max_item_bytes =
       std::min(result.limits.max_item_bytes,
                static_cast<std::uint32_t>(std::min<std::uint64_t>(result.limits.initial_window_bytes,
                                                                   std::numeric_limits<std::uint32_t>::max())));
   result.limits.max_inflight_calls = fair_inflight_limit(result.limits);
   return result;
}

void session::impl::initialize_on_strand(const strand_type& executor) {
   if (initialized) {
      return;
   }
   initialized = true;
   writer_wake = std::make_shared<timer>(executor);
   idle_timer = std::make_shared<timer>(executor);
   writer_wake->expires_at(timer::time_point::max());
   idle_timer->expires_at(timer::time_point::max());

   auto hello = forge::api::core::frame{
       .kind = forge::api::core::frame_kind::session_hello,
       .id = {},
       .codec = settings.codec,
       .payload = forge::raw::pack(local_hello()),
   };
   enqueue_control(std::move(hello));
   writer_running = true;
   boost::asio::co_spawn(
       executor, [self = shared_from_this()]() -> boost::asio::awaitable<void> { co_await self->writer_loop(); },
       boost::asio::detached);
   reader_running = true;
   boost::asio::co_spawn(
       executor, [self = shared_from_this()]() -> boost::asio::awaitable<void> { co_await self->reader_loop(); },
       boost::asio::detached);
   boost::asio::co_spawn(
       executor, [self = shared_from_this()]() -> boost::asio::awaitable<void> { co_await self->idle_watchdog(); },
       boost::asio::detached);
   touch_activity();
}

void session::impl::negotiate_hello(const forge::api::core::session_hello& peer) {
   if (peer.version.major != 2 || peer.codec != settings.codec) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                            "API stream session hello is incompatible",
                            forge::exceptions::ctx("peer_major", peer.version.major),
                            forge::exceptions::ctx("peer_codec", peer.codec.value));
   }
   if (!peer.capabilities.supports(forge::api::core::capability::unary) ||
       !peer.capabilities.supports(forge::api::core::capability::stream_window)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                            "API stream peer lacks mandatory wire-v2 capabilities");
   }

   const auto local = local_hello();
   negotiated_capabilities.bits = local.capabilities.bits & peer.capabilities.bits;
   negotiated_limits = forge::api::core::session_limits{
       .max_frame_bytes = std::min(local.limits.max_frame_bytes, peer.limits.max_frame_bytes),
       .max_item_bytes = std::min(local.limits.max_item_bytes, peer.limits.max_item_bytes),
       .max_inflight_calls = std::min(local.limits.max_inflight_calls, peer.limits.max_inflight_calls),
       .initial_window_items = std::min(local.limits.initial_window_items, peer.limits.initial_window_items),
       .initial_window_bytes = std::min(local.limits.initial_window_bytes, peer.limits.initial_window_bytes),
       .max_buffered_bytes = std::min(local.limits.max_buffered_bytes, peer.limits.max_buffered_bytes),
       .idle_timeout_ms = std::min(local.limits.idle_timeout_ms, peer.limits.idle_timeout_ms),
       .shutdown_grace_ms = std::min(local.limits.shutdown_grace_ms, peer.limits.shutdown_grace_ms),
   };
   if (negotiated_limits.max_frame_bytes == 0 || negotiated_limits.max_item_bytes == 0 ||
       negotiated_limits.max_inflight_calls == 0 || negotiated_limits.initial_window_items == 0 ||
       negotiated_limits.initial_window_bytes == 0 || negotiated_limits.max_buffered_bytes == 0 ||
       negotiated_limits.max_item_bytes > negotiated_limits.max_frame_bytes ||
       negotiated_limits.max_item_bytes > negotiated_limits.initial_window_bytes ||
       negotiated_limits.initial_window_bytes > negotiated_limits.max_buffered_bytes ||
       negotiated_limits.max_item_bytes > negotiated_limits.max_buffered_bytes) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                            "API stream peer advertised invalid session limits");
   }
   negotiated_limits.max_inflight_calls = fair_inflight_limit(negotiated_limits);
   peer_hello_received = true;
   touch_activity();
   wake_session();
}

void session::impl::touch_activity() noexcept {
   if (!idle_timer) {
      return;
   }
   const auto timeout =
       peer_hello_received ? std::chrono::milliseconds{negotiated_limits.idle_timeout_ms} : settings.idle_timeout;
   try {
      if (timeout.count() <= 0) {
         idle_timer->expires_at(timer::time_point::max());
      } else {
         idle_timer->expires_after(timeout);
      }
   } catch (...) {
      // Activity tracking must stay noexcept during shutdown races.
   }
}

boost::asio::awaitable<void> session::impl::idle_watchdog() {
   while (!closed.load(std::memory_order_acquire)) {
      auto error = boost::system::error_code{};
      co_await idle_timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      if (error) {
         continue;
      }
      if (idle_timer->expiry() == timer::time_point::max()) {
         continue;
      }
      auto timeout = std::make_exception_ptr(
          forge::api::core::exceptions::deadline_exceeded{"API stream session idle timeout exceeded"});
      fail_session(timeout);
      stop_transport();
      co_return;
   }
}

void session::impl::throw_if_terminated_on_strand(const std::shared_ptr<call_state>& call) const {
   if (call && call->done) {
      if (call->error) {
         std::rethrow_exception(call->error);
      }
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "API stream call already ended");
   }
   if (failure) {
      std::rethrow_exception(failure);
   }
   if (closed.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "API stream session is closed");
   }
}

boost::asio::awaitable<void> session::impl::ensure_handshake_on_strand(const std::shared_ptr<call_state>& call) {
   while (true) {
      const auto observed = session_wake.epoch();
      throw_if_terminated_on_strand(call);
      if (hello_sent && peer_hello_received) {
         co_return;
      }
      auto wait_cancelled = false;
      try {
         static_cast<void>(co_await session_wake.async_wait(observed));
      } catch (const boost::system::system_error& error) {
         if (!call || error.code() != boost::asio::error::operation_aborted) {
            throw;
         }
         wait_cancelled = true;
      }
      if (wait_cancelled) {
         const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
         if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled,
                                  "API stream call was cancelled during hello");
         }
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "API stream handshake wait was cancelled");
      }
   }
}

boost::asio::awaitable<void> session::impl::wait_receipt_on_strand(const std::shared_ptr<write_receipt>& receipt) {
   while (!receipt->done) {
      auto error = boost::system::error_code{};
      co_await receipt->wake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "API stream call was cancelled during write");
      }
   }
   if (receipt->error) {
      std::rethrow_exception(receipt->error);
   }
}

std::vector<std::uint8_t> session::impl::encode_wire_frame(const forge::api::core::frame& value) const {
   auto payload = forge::raw::pack(value);
   const auto limit = peer_hello_received ? negotiated_limits.max_frame_bytes : settings.max_frame_size;
   return forge::net::transport::encode_frame(payload, forge::net::transport::frame_options{.max_size = limit});
}

forge::api::core::frame session::impl::decode_wire_frame(forge::net::transport::chunk payload) const {
   try {
      auto bytes = std::move(payload).into_vector();
      const auto limit = peer_hello_received ? negotiated_limits.max_frame_bytes : settings.max_frame_size;
      return forge::raw::unpack_exact<forge::api::core::frame>(std::span<const std::uint8_t>{bytes},
                                                               wire_unpack_limits(bytes.size(), limit));
   } catch (const forge::raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "API stream frame exceeds decode limits");
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "API stream frame allocation failed");
   } catch (const forge::raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream frame is malformed");
   } catch (const forge::raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream frame is malformed");
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (...) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream frame is malformed");
   }
}

forge::api::core::session_hello session::impl::decode_hello(const forge::api::core::frame& value) const {
   return decode_control_payload<forge::api::core::session_hello>(value, settings.max_frame_size);
}

forge::api::core::stream_window session::impl::decode_window(const forge::api::core::frame& value) const {
   return decode_control_payload<forge::api::core::stream_window>(value, negotiated_limits.max_frame_bytes);
}

forge::api::core::stream_end session::impl::decode_end(const forge::api::core::frame& value) const {
   return decode_control_payload<forge::api::core::stream_end>(value, negotiated_limits.max_frame_bytes);
}

void session::impl::wake_session() noexcept {
   session_wake.notify();
}

void session::impl::wake_call(const std::shared_ptr<call_state>& call) noexcept {
   try {
      call->wake.cancel();
   } catch (...) {
      // Call completion paths remain noexcept.
   }
}

void session::impl::complete_receipt(const std::shared_ptr<write_receipt>& receipt, std::exception_ptr error) noexcept {
   if (!receipt || receipt->done) {
      return;
   }
   receipt->error = std::move(error);
   receipt->done = true;
   try {
      receipt->wake.cancel();
   } catch (...) {
      // Write completion paths remain noexcept.
   }
}

bool session::impl::writer_idle() const noexcept {
   if (writer_write_in_flight || !control_queue.empty() || !round_robin.empty()) {
      return false;
   }
   return std::none_of(calls.begin(), calls.end(),
                       [](const auto& entry) { return !entry.second->write_queue.empty(); });
}

void session::impl::stop_transport() noexcept {
   closed.store(true, std::memory_order_release);
   try {
      stream.cancel();
   } catch (...) {
      // The terminal state is already visible; cancellation is best effort.
   }
   wake_session();
   wake_writer();
   if (idle_timer) {
      try {
         idle_timer->cancel();
      } catch (...) {
         // Session completion paths remain noexcept.
      }
   }
}

void session::impl::fail_session(std::exception_ptr error) noexcept {
   if (!error) {
      return;
   }
   if (!failure) {
      failure = error;
   }
   accepting = false;
   closed.store(true, std::memory_order_release);
   auto active_calls = std::vector<std::shared_ptr<call_state>>{};
   active_calls.reserve(calls.size());
   for (const auto& [_, call] : calls) {
      active_calls.push_back(call);
   }
   for (const auto& call : active_calls) {
      call->error = failure;
      call->done = true;
      if (call->handler_running) {
         try {
            call->handler_cancel.emit(boost::asio::cancellation_type::all);
         } catch (...) {
            // Session failure remains authoritative during cancellation.
         }
      }
      if (call->inbound && call->inbound->endpoint) {
         call->inbound->endpoint->fail(failure);
      }
      if (call->outbound && call->outbound->endpoint) {
         call->outbound->endpoint->fail(failure);
      }
      for (auto& queued : call->write_queue) {
         release_outbound_capacity(queued);
         complete_receipt(queued.receipt, failure);
      }
      call->write_queue.clear();
      wake_call(call);
   }
   calls.clear();
   for (auto& queued : control_queue) {
      complete_receipt(queued.receipt, failure);
   }
   control_queue.clear();
   round_robin.clear();
   outbound_buffered_items = 0;
   outbound_buffered_bytes = 0;
   wake_outbound_capacity();
   wake_session();
   wake_writer();
   if (idle_timer) {
      try {
         idle_timer->cancel();
      } catch (...) {
         // Session completion paths remain noexcept.
      }
   }
}

} // namespace forge::api::stream
