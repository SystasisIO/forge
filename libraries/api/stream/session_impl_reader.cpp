module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

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
import forge.raw.raw;

#include "details/session_impl.hxx"

namespace forge::api::stream {
namespace {

constexpr auto compact_threshold = std::size_t{65'536};
constexpr auto frame_prefix_size = std::size_t{4};
constexpr auto call_id_side_bit = std::uint64_t{1} << 63U;

[[nodiscard]] std::span<const std::uint8_t> available_bytes(const std::vector<std::uint8_t>& buffer,
                                                            std::size_t consumed) noexcept {
   if (consumed >= buffer.size()) {
      return {};
   }
   return {buffer.data() + consumed, buffer.size() - consumed};
}

void compact_buffer(std::vector<std::uint8_t>& buffer, std::size_t& consumed) {
   if (consumed == 0) {
      return;
   }
   if (consumed >= buffer.size()) {
      buffer.clear();
      consumed = 0;
      return;
   }
   auto compacted = std::vector<std::uint8_t>{};
   compacted.reserve(buffer.size() - consumed);
   compacted.insert(compacted.end(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed), buffer.end());
   buffer = std::move(compacted);
   consumed = 0;
}

[[nodiscard]] bool is_clean_close(const forge::exceptions::base& error) noexcept {
   return forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::closed) ||
          forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::canceled);
}

} // namespace

boost::asio::awaitable<forge::net::transport::chunk> session::impl::read_wire_frame() {
   while (true) {
      const auto limit = peer_hello_received ? negotiated_limits.max_frame_bytes : settings.max_frame_size;
      auto decoded = forge::net::transport::frame_view_decode_result{};
      try {
         decoded = forge::net::transport::decode_frame_view(available_bytes(read_buffer, read_consumed),
                                                            forge::net::transport::frame_options{.max_size = limit});
      } catch (const forge::net::transport::exceptions::frame_too_large&) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                               "API stream frame exceeds the negotiated size limit");
      }
      if (decoded.status == forge::net::transport::frame_decode_status::complete) {
         auto payload = forge::net::transport::chunk{decoded.payload};
         read_consumed += decoded.consumed;
         if (read_consumed >= read_buffer.size() || read_consumed > compact_threshold) {
            compact_buffer(read_buffer, read_consumed);
         }
         co_return payload;
      }

      compact_buffer(read_buffer, read_consumed);
      auto next = co_await stream.async_read_chunk();
      const auto bytes = next.bytes();
      const auto buffer_limit = static_cast<std::uint64_t>(settings.max_buffered_bytes) + limit + frame_prefix_size;
      if (bytes.size() > buffer_limit || read_buffer.size() > buffer_limit - bytes.size()) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                               "API stream receive buffer exceeds the negotiated limit");
      }
      read_buffer.insert(read_buffer.end(), bytes.begin(), bytes.end());
   }
}

boost::asio::awaitable<void> session::impl::reader_loop() {
   try {
      while (!closed.load(std::memory_order_acquire)) {
         auto value = decode_wire_frame(co_await read_wire_frame());
         touch_activity();
         co_await handle_inbound_frame(std::move(value));
      }
   } catch (const forge::exceptions::base& error) {
      reader_running = false;
      const auto calls_terminal =
          std::all_of(calls.begin(), calls.end(), [](const auto& entry) { return entry.second->done; });
      if (closing || (calls_terminal && is_clean_close(error))) {
         closed.store(true, std::memory_order_release);
         wake_session();
         wake_writer();
         co_return;
      }
      fail_session(std::current_exception());
      stop_transport();
      co_return;
   } catch (...) {
      reader_running = false;
      if (!closing) {
         fail_session(std::current_exception());
         stop_transport();
      }
      co_return;
   }
   reader_running = false;
   wake_session();
}

boost::asio::awaitable<void> session::impl::handle_inbound_frame(forge::api::core::frame value) {
   if (!peer_hello_received) {
      if (value.kind != forge::api::core::frame_kind::session_hello || value.id.value != 0 ||
          !value.api.id.value.empty() || !value.method.empty() || !value.meta.empty() ||
          value.codec != settings.codec) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "API stream peer did not send session hello first");
      }
      negotiate_hello(decode_hello(value));
      co_return;
   }

   if (value.kind == forge::api::core::frame_kind::session_hello || value.id.value == 0) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "invalid API stream control frame");
   }
   if (!hello_sent) {
      // The peer can reply to our delivered hello before its async_write completion resumes.
      co_await ensure_handshake_on_strand();
   }
   if (value.codec != settings.codec) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::codec_failed, "API stream frame codec is not negotiated",
                            forge::exceptions::ctx("codec", value.codec.value));
   }

   if (value.kind == forge::api::core::frame_kind::request) {
      co_await handle_request(std::move(value));
      co_return;
   }

   const auto found = calls.find(value.id.value);
   if (found == calls.end()) {
      if (const auto tombstone = tombstones.find(value.id.value); tombstone != tombstones.end()) {
         handle_tombstone_frame(tombstone->second, value);
         co_return;
      }
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream frame references an unknown call_id",
                            forge::exceptions::ctx("call_id", value.id.value));
   }
   auto call = found->second;
   if (value.api != call->api || value.method != call->method) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream frame identity does not match its call",
                            forge::exceptions::ctx("call_id", value.id.value));
   }
   if (call->done) {
      if (value.kind == forge::api::core::frame_kind::stream_item && call->inbound && call->inbound->discarding) {
         handle_stream_item(call, std::move(value));
         co_return;
      }
      if (value.kind == forge::api::core::frame_kind::stream_end && call->inbound && call->inbound->discarding &&
          !call->inbound->ended) {
         handle_stream_end(call, value);
         co_return;
      }
      if (value.kind == forge::api::core::frame_kind::cancel) {
         handle_cancel(call);
         co_return;
      }
      if (value.kind == forge::api::core::frame_kind::stream_window ||
          value.kind == forge::api::core::frame_kind::stream_end) {
         co_return;
      }
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream received data or a duplicate terminal for a completed call",
                            forge::exceptions::ctx("call_id", value.id.value));
   }
   switch (value.kind) {
   case forge::api::core::frame_kind::stream_item:
      handle_stream_item(call, std::move(value));
      break;
   case forge::api::core::frame_kind::stream_end:
      handle_stream_end(call, value);
      break;
   case forge::api::core::frame_kind::stream_window:
      handle_stream_window(call, value);
      break;
   case forge::api::core::frame_kind::response:
   case forge::api::core::frame_kind::error:
      handle_terminal(call, std::move(value));
      break;
   case forge::api::core::frame_kind::cancel:
      handle_cancel(call);
      break;
   default:
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream received an invalid frame kind");
   }
}

boost::asio::awaitable<void> session::impl::handle_request(forge::api::core::frame value) {
   if (!accepting || !plan) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream session does not admit requests");
   }
   const auto remote_high_side = (next_remote_call_id & call_id_side_bit) != 0;
   const auto request_high_side = (value.id.value & call_id_side_bit) != 0;
   const auto side_limit = remote_high_side ? std::numeric_limits<std::uint64_t>::max() : call_id_side_bit;
   if (value.id.value == 0 || request_high_side != remote_high_side || value.id.value < next_remote_call_id ||
       value.id.value >= side_limit) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream request call_id is outside the peer monotonic range");
   }
   next_remote_call_id = value.id.value + 1;
   if (calls.contains(value.id.value) || tombstones.contains(value.id.value)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream request reuses a call_id",
                            forge::exceptions::ctx("call_id", value.id.value));
   }
   if (calls.size() + draining_tombstones() >= negotiated_limits.max_inflight_calls) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "API stream max inflight calls exceeded");
   }

   auto selected = plan->pin(value.api);
   const auto* api = selected.describe(value.api);
   const auto* descriptor = api == nullptr ? nullptr : forge::api::core::find_method(*api, value.method);
   const auto kind = descriptor == nullptr ? forge::api::core::method_kind::unary : descriptor->kind;
   const auto required = [&] {
      switch (kind) {
      case forge::api::core::method_kind::unary:
         return forge::api::core::capability::unary;
      case forge::api::core::method_kind::server_stream:
         return forge::api::core::capability::server_stream;
      case forge::api::core::method_kind::client_stream:
         return forge::api::core::capability::client_stream;
      case forge::api::core::method_kind::bidirectional_stream:
         return forge::api::core::capability::bidirectional_stream;
      }
      return forge::api::core::capability::unary;
   }();
   if (!negotiated_capabilities.supports(required)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                            "API stream method kind was not negotiated");
   }

   auto call = make_remote_call(value, kind, descriptor, std::move(selected));
   const auto has_contextual_invoker = [&] {
      if (!call->descriptor) {
         return false;
      }
      if (kind == forge::api::core::method_kind::unary) {
         const auto* invoker = forge::api::core::detail::contextual_unary_for(*call->descriptor);
         return invoker != nullptr && static_cast<bool>(*invoker);
      }
      const auto* invoker = forge::api::core::detail::contextual_stream_for(*call->descriptor);
      return invoker != nullptr && static_cast<bool>(*invoker);
   }();
   if (call->descriptor && call->descriptor->request_decoder && !has_contextual_invoker) {
      const auto bounded =
          static_cast<std::uint32_t>(std::min<std::size_t>(value.payload.size(), negotiated_limits.max_frame_bytes));
      call->descriptor->request_decoder(value.payload, forge::raw::unpack_limits{
                                                           .max_container_elements = bounded,
                                                           .max_total_container_elements = bounded,
                                                           .max_bytes = bounded,
                                                           .first_container_elements = bounded,
                                                       });
   }
   calls.emplace(value.id.value, call);
   call->handler_running = true;
   install_inbound_observer(call);
   start_inbound_pump(call);
   replenish_inbound_credit();
   if (settings.deadline.count() > 0) {
      start_deadline(call, settings.deadline);
   }
   boost::asio::co_spawn(
       *strand,
       [self = shared_from_this(), value = std::move(value), call]() mutable -> boost::asio::awaitable<void> {
          co_await self->run_remote_call(std::move(value), call);
       },
       boost::asio::bind_cancellation_slot(call->handler_cancel.slot(), boost::asio::detached));
   start_outbound_pump(call);
   co_return;
}

void session::impl::handle_stream_item(const std::shared_ptr<call_state>& call, forge::api::core::frame value) {
   if (!call->inbound || call->inbound->ended || value.payload.size() > negotiated_limits.max_item_bytes) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream item violates call direction or item limit");
   }
   auto& flow = *call->inbound;
   if (flow.transferred_bytes > flow.limit_bytes || flow.transferred_items >= flow.limit_items ||
       value.payload.size() > flow.limit_bytes - flow.transferred_bytes) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                            "API stream peer exceeded advertised credit",
                            forge::exceptions::ctx("call_id", call->id.value));
   }
   if (call->descriptor) {
      const auto& decoder = flow.direction == forge::api::core::stream_direction::input
                                ? call->descriptor->input_decoder
                                : call->descriptor->output_decoder;
      if (decoder) {
         const auto bounded =
             static_cast<std::uint32_t>(std::min<std::size_t>(value.payload.size(), negotiated_limits.max_item_bytes));
         decoder(value.payload, forge::raw::unpack_limits{
                                    .max_container_elements = bounded,
                                    .max_total_container_elements = bounded,
                                    .max_bytes = bounded,
                                    .first_container_elements = bounded,
                                });
      }
   }

   if (flow.transferred_items == std::numeric_limits<std::uint64_t>::max() ||
       value.payload.size() > std::numeric_limits<std::uint64_t>::max() - flow.transferred_bytes ||
       flow.buffered_items == std::numeric_limits<std::uint64_t>::max() ||
       value.payload.size() > std::numeric_limits<std::uint64_t>::max() - flow.buffered_bytes) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream inbound credit counters overflowed");
   }
   ++flow.transferred_items;
   flow.transferred_bytes += value.payload.size();
   if (!flow.discarding) {
      ++flow.buffered_items;
      flow.buffered_bytes += value.payload.size();
      if (aggregate_buffered_bytes() > negotiated_limits.max_buffered_bytes) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                               "API stream aggregate inbound buffer limit exceeded");
      }
      flow.pending_items.push_back(std::move(value.payload));
   }
   wake_call(call);
}

void session::impl::handle_stream_end(const std::shared_ptr<call_state>& call, const forge::api::core::frame& value) {
   const auto end = decode_end(value);
   if (!call->inbound || call->inbound->ended || end.direction != call->inbound->direction) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream end violates call direction");
   }
   call->inbound->ended = true;
   wake_call(call);
}

void session::impl::handle_stream_window(const std::shared_ptr<call_state>& call,
                                         const forge::api::core::frame& value) {
   const auto window = decode_window(value);
   if (!call->outbound || window.direction != call->outbound->direction) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream window violates call direction");
   }
   if (call->outbound->ended) {
      return;
   }
   if (window.max_items < call->outbound->limit_items || window.max_bytes < call->outbound->limit_bytes) {
      return;
   }
   call->outbound->limit_items = window.max_items;
   call->outbound->limit_bytes = window.max_bytes;
   wake_call(call);
}

void session::impl::handle_terminal(const std::shared_ptr<call_state>& call, forge::api::core::frame value) {
   if (!call->local_origin || call->done || call->terminal) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream call received more than one terminal frame");
   }
   if (value.kind == forge::api::core::frame_kind::response &&
       (call->kind == forge::api::core::method_kind::server_stream ||
        call->kind == forge::api::core::method_kind::bidirectional_stream) &&
       (!call->inbound || !call->inbound->ended || !value.payload.empty())) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream success must follow output end with an empty response");
   }
   if (value.kind == forge::api::core::frame_kind::response && call->descriptor && call->descriptor->response_decoder) {
      const auto bounded =
          static_cast<std::uint32_t>(std::min<std::size_t>(value.payload.size(), negotiated_limits.max_frame_bytes));
      call->descriptor->response_decoder(value.payload, forge::raw::unpack_limits{
                                                            .max_container_elements = bounded,
                                                            .max_total_container_elements = bounded,
                                                            .max_bytes = bounded,
                                                            .first_container_elements = bounded,
                                                        });
   }

   call->terminal.emplace(std::move(value));
   call->done = true;
   call->deadline.cancel();
   if (call->terminal->kind == forge::api::core::frame_kind::error) {
      auto error = std::make_exception_ptr(forge::api::core::exceptions::cancelled{"API stream call failed remotely"});
      if (call->inbound && call->inbound->endpoint) {
         call->inbound->pending_items.clear();
         call->inbound->buffered_items = 0;
         call->inbound->buffered_bytes = 0;
         call->inbound->ended = true;
         call->inbound->endpoint->fail(error);
      }
      if (call->outbound && call->outbound->endpoint) {
         call->outbound->endpoint->fail(error);
      }
   } else if (call->outbound && call->outbound->endpoint) {
      call->outbound->endpoint->close();
   }
   finish_call(call);
   wake_call(call);
}

void session::impl::handle_cancel(const std::shared_ptr<call_state>& call) {
   if (call->done) {
      if (call->inbound && call->inbound->discarding && !call->inbound->ended) {
         call->inbound->ended = true;
         wake_call(call);
         finish_call(call);
      }
      return;
   }
   cancel_call(
       call,
       std::make_exception_ptr(forge::api::core::exceptions::cancelled{"API stream call was cancelled by the peer"}),
       false);
}

void session::impl::handle_tombstone_frame(tombstone_state& tombstone, const forge::api::core::frame& value) {
   if (value.api != tombstone.api || value.method != tombstone.method || value.codec != tombstone.codec) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream late frame identity does not match its completed call");
   }
   if (value.kind == forge::api::core::frame_kind::stream_item && tombstone.inbound && !tombstone.inbound->ended) {
      auto& flow = *tombstone.inbound;
      if (value.payload.size() > negotiated_limits.max_item_bytes || flow.transferred_items >= flow.limit_items ||
          flow.transferred_bytes > flow.limit_bytes ||
          value.payload.size() > flow.limit_bytes - flow.transferred_bytes) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                               "API stream late item exceeds completed-call credit");
      }
      ++flow.transferred_items;
      flow.transferred_bytes += value.payload.size();
      return;
   }
   if (value.kind == forge::api::core::frame_kind::stream_end) {
      if (!tombstone.inbound) {
         return;
      }
      const auto end = decode_end(value);
      if (tombstone.inbound->ended || end.direction != tombstone.inbound->direction) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "API stream late end is duplicated or has the wrong direction");
      }
      tombstone.inbound->ended = true;
      complete_tombstone(value.id.value);
      return;
   }
   if (value.kind == forge::api::core::frame_kind::cancel) {
      if (tombstone.inbound && !tombstone.inbound->ended) {
         tombstone.inbound->ended = true;
         complete_tombstone(value.id.value);
      }
      return;
   }
   if (value.kind == forge::api::core::frame_kind::stream_window) {
      return;
   }
   FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                         "API stream received invalid data for a completed call");
}

} // namespace forge::api::stream
