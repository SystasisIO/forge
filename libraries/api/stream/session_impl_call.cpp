module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module forge.api.stream.session;

import forge.asio.notification;
import forge.raw.raw;

#include "details/session_impl.hxx"

namespace forge::api::stream {
namespace {

constexpr auto call_id_side_bit = std::uint64_t{1} << 63U;

[[nodiscard]] std::exception_ptr cancelled_call_error(std::uint64_t id) {
   try {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "API stream call was cancelled",
                            forge::exceptions::ctx("call_id", id));
   } catch (...) {
      return std::current_exception();
   }
}

[[nodiscard]] std::exception_ptr deadline_call_error(std::uint64_t id) {
   try {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::deadline_exceeded, "API stream call deadline exceeded",
                            forge::exceptions::ctx("call_id", id));
   } catch (...) {
      return std::current_exception();
   }
}

[[nodiscard]] forge::api::core::capability capability_for(forge::api::core::method_kind kind) {
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
}

[[nodiscard]] forge::raw::unpack_limits payload_limits(std::size_t payload_size, std::uint32_t max_bytes) {
   const auto bounded = static_cast<std::uint32_t>(std::min<std::size_t>(payload_size, max_bytes));
   return forge::raw::unpack_limits{
       .max_container_elements = bounded,
       .max_total_container_elements = bounded,
       .max_bytes = bounded,
       .first_container_elements = bounded,
   };
}

} // namespace

std::shared_ptr<session::impl::call_state> session::impl::make_remote_call(const forge::api::core::frame& request,
                                                                           forge::api::core::method_kind kind,
                                                                           const forge::api::core::method_descriptor* descriptor,
                                                                           forge::api::core::pinned_binding_plan selection) {
   auto call = std::make_shared<call_state>(*strand, request.id, kind, false);
   call->api = request.api;
   call->method = request.method;
   call->codec = request.codec;
   call->admission_order = next_admission_order++;
   if (descriptor != nullptr) {
      call->descriptor = *descriptor;
   }
   call->selection.emplace(std::move(selection));
   const auto per_call_bytes = static_cast<std::size_t>(negotiated_limits.max_buffered_bytes);
   const auto make_pipe = [&] {
      return forge::api::core::detail::make_local_stream_pair(*strand, negotiated_limits.max_item_bytes,
                                                              negotiated_limits.initial_window_items, per_call_bytes);
   };

   if (kind == forge::api::core::method_kind::client_stream ||
       kind == forge::api::core::method_kind::bidirectional_stream) {
      auto pipe = make_pipe();
      call->inbound.emplace(flow_state{
          .direction = forge::api::core::stream_direction::input,
          .endpoint = std::move(pipe.writer),
      });
   }
   if (kind == forge::api::core::method_kind::server_stream ||
       kind == forge::api::core::method_kind::bidirectional_stream) {
      auto pipe = make_pipe();
      call->outbound.emplace(flow_state{
          .direction = forge::api::core::stream_direction::output,
          .endpoint = std::move(pipe.reader),
      });
   }
   return call;
}

std::shared_ptr<session::impl::call_state>
session::impl::reserve_local_call(forge::api::core::frame& request, forge::api::core::method_kind kind,
                                  std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                                  std::shared_ptr<forge::api::core::detail::stream_endpoint> output,
                                  call_options& value, const forge::api::core::method_descriptor* descriptor) {
   if (failure) {
      std::rethrow_exception(failure);
   }
   if (!accepting || closing || closed.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "API stream session is not accepting calls");
   }
   if (calls.size() + draining_tombstones() >= settings.max_inflight) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "API stream max inflight calls exceeded");
   }
   const auto directions_match = (kind == forge::api::core::method_kind::unary && !input && !output) ||
                                 (kind == forge::api::core::method_kind::server_stream && !input && output) ||
                                 (kind == forge::api::core::method_kind::client_stream && input && !output) ||
                                 (kind == forge::api::core::method_kind::bidirectional_stream && input && output);
   if (!directions_match) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream endpoints do not match method direction");
   }

   const auto local_high_side = (next_call_id & call_id_side_bit) != 0;
   const auto side_limit = local_high_side ? std::numeric_limits<std::uint64_t>::max() : call_id_side_bit;
   const auto requested_id = value.id.value != 0 ? value.id.value : request.id.value;
   if (requested_id != 0) {
      const auto requested_high_side = (requested_id & call_id_side_bit) != 0;
      if (requested_high_side != local_high_side || requested_id < next_call_id || requested_id >= side_limit) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "API stream call_id is outside the local monotonic range");
      }
      request.id.value = requested_id;
   } else {
      if (next_call_id == 0 || next_call_id >= side_limit) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                               "API stream local call_id space is exhausted");
      }
      request.id.value = next_call_id;
   }
   next_call_id = request.id.value + 1;
   if (calls.contains(request.id.value) || tombstones.contains(request.id.value)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream call_id is reserved or stale");
   }
   request.kind = forge::api::core::frame_kind::request;
   request.codec = settings.codec;
   if (!value.meta.empty()) {
      request.meta = std::move(value.meta);
   }

   auto call = std::make_shared<call_state>(*strand, request.id, kind, true);
   call->api = request.api;
   call->method = request.method;
   call->codec = request.codec;
   if (descriptor) {
      call->descriptor = *descriptor;
   }
   call->admission_order = next_admission_order++;
   if (input) {
      call->outbound.emplace(flow_state{
          .direction = forge::api::core::stream_direction::input,
          .endpoint = std::move(input),
      });
   }
   if (output) {
      call->inbound.emplace(flow_state{
          .direction = forge::api::core::stream_direction::output,
          .endpoint = std::move(output),
      });
   }
   calls.emplace(request.id.value, call);
   const auto deadline = value.deadline.count() > 0 ? value.deadline : settings.deadline;
   if (deadline.count() > 0) {
      start_deadline(call, deadline);
   }
   return call;
}

boost::asio::awaitable<forge::api::core::frame>
session::impl::async_call_on_strand(forge::api::core::frame request, forge::api::core::method_kind kind,
                                    std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                                    std::shared_ptr<forge::api::core::detail::stream_endpoint> output,
                                    call_options value, const forge::api::core::method_descriptor* descriptor) {
   auto call = reserve_local_call(request, kind, std::move(input), std::move(output), value, descriptor);
   try {
      co_await ensure_handshake_on_strand(call);
      if (!negotiated_capabilities.supports(capability_for(kind))) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                               "API stream method kind was not negotiated");
      }
      if (call->descriptor && call->descriptor->kind != kind) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "API stream method kind does not match its descriptor");
      }
      const auto admitted_before = std::count_if(calls.begin(), calls.end(), [&](const auto& entry) {
         return !entry.second->done && entry.second->admission_order <= call->admission_order;
      });
      if (admitted_before + draining_tombstones() > negotiated_limits.max_inflight_calls) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                               "API stream peer negotiated a smaller inflight limit");
      }
      if (call->descriptor && call->descriptor->request_decoder) {
         call->descriptor->request_decoder(request.payload,
                                           payload_limits(request.payload.size(), negotiated_limits.max_frame_bytes));
      }
      throw_if_terminated_on_strand(call);
      auto receipt = enqueue_call_frame(call, request);
      co_await wait_receipt_on_strand(receipt);
      call->request_written = true;

      install_inbound_observer(call);
      start_inbound_pump(call);
      replenish_inbound_credit();
      start_outbound_pump(call);
      co_await wait_for_terminal(call);
   } catch (...) {
      if (!call->done) {
         cancel_call(call, std::current_exception(), call->request_written);
      }
      if (call->error) {
         std::rethrow_exception(call->error);
      }
      throw;
   }

   if (call->error) {
      std::rethrow_exception(call->error);
   }
   if (!call->terminal) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "API stream call completed without a terminal");
   }
   co_return std::move(*call->terminal);
}

boost::asio::awaitable<void> session::impl::wait_for_terminal(const std::shared_ptr<call_state>& call) {
   while (calls.contains(call->id.value)) {
      call->wake.expires_at(timer::time_point::max());
      auto error = boost::system::error_code{};
      co_await call->wake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != boost::asio::cancellation_type::none && !call->done) {
         cancel_call(call, cancelled_call_error(call->id.value), call->request_written);
      }
   }
}

boost::asio::awaitable<void> session::impl::wait_for_credit(const std::shared_ptr<call_state>& call,
                                                            std::size_t item_bytes) {
   while (true) {
      if (call->done || call->error) {
         if (call->error) {
            std::rethrow_exception(call->error);
         }
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled,
                               "API stream call ended while waiting for credit");
      }
      auto& flow = *call->outbound;
      if (flow.transferred_items < flow.limit_items && item_bytes <= flow.limit_bytes - flow.transferred_bytes) {
         co_return;
      }
      call->wake.expires_at(timer::time_point::max());
      auto error = boost::system::error_code{};
      co_await call->wake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   }
}

boost::asio::awaitable<void> session::impl::wait_for_outbound_capacity(const std::shared_ptr<call_state>& call,
                                                                       std::size_t item_bytes) {
   while (true) {
      if (call->done || call->error) {
         if (call->error) {
            std::rethrow_exception(call->error);
         }
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled,
                               "API stream call ended while waiting for outbound capacity");
      }
      const auto byte_limit = negotiated_limits.max_buffered_bytes;
      if (outbound_buffered_items < outbound_item_limit() && outbound_buffered_bytes <= byte_limit &&
          item_bytes <= byte_limit - outbound_buffered_bytes) {
         co_return;
      }
      call->wake.expires_at(timer::time_point::max());
      auto error = boost::system::error_code{};
      co_await call->wake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   }
}

boost::asio::awaitable<void> session::impl::pump_outbound(const std::shared_ptr<call_state>& call) {
   const auto drains_after_terminal = [&] {
      return call->local_origin && call->terminal && call->terminal->kind == forge::api::core::frame_kind::response;
   };
   try {
      while (call->outbound && !call->outbound->ended) {
         if (call->done && !drains_after_terminal()) {
            break;
         }
         auto item = std::optional<forge::api::core::bytes>{};
         try {
            item = co_await call->outbound->endpoint->async_read();
         } catch (...) {
            if (!call->local_origin && call->handler_running) {
               break;
            }
            throw;
         }
         if (!item) {
            auto& flow = *call->outbound;
            flow.ended = true;
            enqueue_call_frame(call, forge::api::core::frame{
                                         .kind = forge::api::core::frame_kind::stream_end,
                                         .id = call->id,
                                         .api = call->api,
                                         .method = call->method,
                                         .codec = call->codec,
                                         .payload = forge::raw::pack(forge::api::core::stream_end{
                                             .direction = flow.direction,
                                         }),
                                     });
            break;
         }
         if (drains_after_terminal()) {
            continue;
         }
         if (item->size() > negotiated_limits.max_item_bytes) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted,
                                  "API stream outbound item exceeds negotiated limit");
         }
         if (call->descriptor) {
            const auto& decoder = call->outbound->direction == forge::api::core::stream_direction::input
                                      ? call->descriptor->input_decoder
                                      : call->descriptor->output_decoder;
            if (decoder) {
               decoder(*item, payload_limits(item->size(), negotiated_limits.max_item_bytes));
            }
         }
         try {
            co_await wait_for_credit(call, item->size());
            co_await wait_for_outbound_capacity(call, item->size());
         } catch (...) {
            if (drains_after_terminal()) {
               continue;
            }
            throw;
         }
         if (drains_after_terminal()) {
            continue;
         }
         auto& flow = *call->outbound;
         if (flow.transferred_items == std::numeric_limits<std::uint64_t>::max() ||
             item->size() > std::numeric_limits<std::uint64_t>::max() - flow.transferred_bytes) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                  "API stream outbound credit counters overflowed");
         }
         ++flow.transferred_items;
         flow.transferred_bytes += item->size();
         enqueue_call_frame(call, forge::api::core::frame{
                                      .kind = forge::api::core::frame_kind::stream_item,
                                      .id = call->id,
                                      .api = call->api,
                                      .method = call->method,
                                      .codec = call->codec,
                                      .payload = std::move(*item),
                                  });
      }
   } catch (...) {
      if (!call->done) {
         cancel_call(call, std::current_exception(), call->request_written || !call->local_origin);
      }
   }
   if (call->outbound) {
      call->outbound->pump_done = true;
   }
   wake_call(call);
   finish_call(call);
}

void session::impl::start_outbound_pump(const std::shared_ptr<call_state>& call) {
   if (!call->outbound || call->outbound->pump_started || call->outbound->pump_done) {
      return;
   }
   call->outbound->pump_started = true;
   try {
      boost::asio::co_spawn(
          *strand,
          [self = shared_from_this(), call]() -> boost::asio::awaitable<void> { co_await self->pump_outbound(call); },
          boost::asio::detached);
   } catch (...) {
      call->outbound->pump_started = false;
      throw;
   }
}

void session::impl::start_inbound_pump(const std::shared_ptr<call_state>& call) {
   if (!call->inbound || call->inbound->pump_started || call->inbound->pump_done) {
      return;
   }
   call->inbound->pump_started = true;
   try {
      boost::asio::co_spawn(
          *strand,
          [self = shared_from_this(), call]() -> boost::asio::awaitable<void> { co_await self->pump_inbound(call); },
          boost::asio::detached);
   } catch (...) {
      call->inbound->pump_started = false;
      throw;
   }
}

void session::impl::finish_unstarted_pumps(const std::shared_ptr<call_state>& call) noexcept {
   if (call->inbound && !call->inbound->pump_started) {
      call->inbound->pump_done = true;
   }
   if (call->outbound && !call->outbound->pump_started) {
      call->outbound->pump_done = true;
   }
}

boost::asio::awaitable<void> session::impl::pump_inbound(const std::shared_ptr<call_state>& call) {
   try {
      while (call->inbound) {
         auto& flow = *call->inbound;
         if (flow.discarding) {
            break;
         }
         if (!flow.pending_items.empty()) {
            auto item = std::move(flow.pending_items.front());
            flow.pending_items.pop_front();
            try {
               co_await flow.endpoint->async_write(std::move(item));
            } catch (...) {
               if (!call->local_origin && call->handler_running) {
                  break;
               }
               throw;
            }
            continue;
         }
         if (flow.ended) {
            flow.endpoint->close();
            break;
         }
         if (call->error) {
            break;
         }
         call->wake.expires_at(timer::time_point::max());
         auto error = boost::system::error_code{};
         co_await call->wake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
   } catch (...) {
      if (!call->done) {
         cancel_call(call, std::current_exception(), call->request_written || !call->local_origin);
      }
   }
   if (call->inbound) {
      call->inbound->pending_items.clear();
      call->inbound->pump_done = true;
   }
   wake_call(call);
   finish_call(call);
}

boost::asio::awaitable<void> session::impl::wait_for_outbound_pump(const std::shared_ptr<call_state>& call) {
   while (call->outbound && !call->outbound->pump_done && !call->done) {
      call->wake.expires_at(timer::time_point::max());
      auto error = boost::system::error_code{};
      co_await call->wake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   }
}

boost::asio::awaitable<void> session::impl::run_remote_call(forge::api::core::frame request,
                                                            const std::shared_ptr<call_state>& call) {
   try {
      auto response = forge::api::core::frame{};
      if (!dispatcher || !call->selection) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "API stream call has no pinned binding selection");
      }
      if (call->kind == forge::api::core::method_kind::unary) {
         response = co_await dispatcher->dispatch(std::move(request), *call->selection);
      } else {
         response =
             co_await dispatcher->dispatch_stream(std::move(request),
                                                  call->inbound ? call->inbound->endpoint : nullptr,
                                                  call->outbound ? call->outbound->endpoint : nullptr,
                                                  *call->selection);
      }
      discard_inbound(call);
      if (call->outbound) {
         co_await wait_for_outbound_pump(call);
      }
      if (!call->done) {
         response.id = call->id;
         response.api = call->api;
         response.method = call->method;
         response.codec = call->codec;
         if (response.kind == forge::api::core::frame_kind::response &&
             (call->kind == forge::api::core::method_kind::server_stream ||
              call->kind == forge::api::core::method_kind::bidirectional_stream)) {
            response.payload.clear();
         }
         call->terminal_enqueued = true;
         call->done = true;
         call->deadline.cancel();
         enqueue_call_frame(call, std::move(response));
         wake_call(call);
      }
   } catch (...) {
      cancel_call(call, std::current_exception(), true);
   }
   call->handler_running = false;
   call->handler_done = true;
   finish_call(call);
   wake_call(call);
}

void session::impl::start_deadline(const std::shared_ptr<call_state>& call, std::chrono::milliseconds value) {
   call->deadline.expires_after(value);
   boost::asio::co_spawn(
       *strand,
       [self = shared_from_this(), call]() -> boost::asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await call->deadline.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
          if (!error && !call->done) {
             self->cancel_call(call, deadline_call_error(call->id.value), call->request_written || !call->local_origin);
          }
       },
       boost::asio::detached);
}

void session::impl::cancel_call(const std::shared_ptr<call_state>& call, std::exception_ptr error, bool notify_peer) {
   if (call->done) {
      return;
   }
   call->done = true;
   call->error = error ? std::move(error) : cancelled_call_error(call->id.value);
   call->deadline.cancel();
   if (call->handler_running) {
      call->handler_cancel.emit(boost::asio::cancellation_type::all);
   }
   finish_unstarted_pumps(call);
   if (call->inbound && call->inbound->endpoint) {
      call->inbound->pending_items.clear();
      call->inbound->buffered_items = 0;
      call->inbound->buffered_bytes = 0;
      call->inbound->ended = true;
      call->inbound->endpoint->fail(call->error);
   }
   if (call->outbound && call->outbound->endpoint) {
      call->outbound->endpoint->fail(call->error);
   }
   for (auto& queued : call->write_queue) {
      release_outbound_capacity(queued);
      complete_receipt(queued.receipt, call->error);
   }
   call->write_queue.clear();
   if (notify_peer && peer_hello_received && !closed.load(std::memory_order_acquire)) {
      call->terminal_enqueued = true;
      enqueue_control(forge::api::core::frame{
          .kind = forge::api::core::frame_kind::cancel,
          .id = call->id,
          .api = call->api,
          .method = call->method,
          .codec = call->codec,
      });
   }
   finish_call(call);
   wake_call(call);
   wake_session();
}

void session::impl::discard_inbound(const std::shared_ptr<call_state>& call) noexcept {
   if (!call->inbound || call->inbound->discarding) {
      return;
   }
   auto& flow = *call->inbound;
   flow.discarding = true;
   flow.pending_items.clear();
   flow.buffered_items = 0;
   flow.buffered_bytes = 0;
   try {
      flow.endpoint->fail(std::make_exception_ptr(
          forge::api::core::exceptions::cancelled{"API stream handler completed before its input direction"}));
   } catch (...) {
      // Terminal response remains authoritative while ingress is discarded.
   }
   wake_call(call);
   replenish_inbound_credit();
}

void session::impl::finish_call(const std::shared_ptr<call_state>& call) {
   const auto found = calls.find(call->id.value);
   if (found == calls.end() || found->second != call) {
      return;
   }
   if (!call->done) {
      return;
   }
   if (call->handler_running && !call->handler_done) {
      return;
   }
   if (call->terminal_enqueued && !call->terminal_written) {
      return;
   }
   if (!call->write_queue.empty()) {
      return;
   }
   if (call->writes_in_flight != 0 || (call->outbound && !call->outbound->pump_done)) {
      return;
   }
   if (call->inbound && !call->inbound->pump_done) {
      return;
   }
   if (call->inbound && call->inbound->buffered_bytes != 0) {
      wake_session();
      return;
   }
   remember_tombstone(call);
   wake_call(call);
   calls.erase(found);
   replenish_inbound_credit();
   wake_session();
}

void session::impl::remember_tombstone(const std::shared_ptr<call_state>& call) {
   auto state = tombstone_state{
       .api = call->api,
       .method = call->method,
       .codec = call->codec,
   };
   if (!call->local_origin && call->inbound && call->inbound->discarding && !call->inbound->ended) {
      const auto& flow = *call->inbound;
      state.inbound.emplace(tombstone_flow{
          .direction = flow.direction,
          .transferred_items = flow.transferred_items,
          .transferred_bytes = flow.transferred_bytes,
          .limit_items = flow.limit_items,
          .limit_bytes = flow.limit_bytes,
          .ended = flow.ended,
      });
   }
   const auto id = call->id.value;
   if (tombstones.emplace(id, std::move(state)).second) {
      const auto& inserted = tombstones.at(id);
      if (!inserted.inbound || inserted.inbound->ended) {
         tombstone_order.push_back(id);
      }
   }
   while (tombstone_order.size() > settings.max_tombstones) {
      tombstones.erase(tombstone_order.front());
      tombstone_order.pop_front();
   }
}

void session::impl::complete_tombstone(std::uint64_t id) {
   const auto found = tombstones.find(id);
   if (found == tombstones.end() || !found->second.inbound || !found->second.inbound->ended) {
      return;
   }
   tombstone_order.push_back(id);
   while (tombstone_order.size() > settings.max_tombstones) {
      tombstones.erase(tombstone_order.front());
      tombstone_order.pop_front();
   }
}

std::size_t session::impl::draining_tombstones() const noexcept {
   return std::count_if(tombstones.begin(), tombstones.end(),
                        [](const auto& entry) { return entry.second.inbound && !entry.second.inbound->ended; });
}

void session::impl::install_inbound_observer(const std::shared_ptr<call_state>& call) {
   if (!call->inbound || !call->inbound->endpoint) {
      return;
   }
   call->inbound->endpoint->set_observer([owner = weak_from_this(), id = call->id.value](
                                             forge::api::core::detail::stream_event event, std::size_t bytes) {
      if (auto self = owner.lock()) {
         const auto executor = self->current_strand();
         if (executor) {
            boost::asio::dispatch(*executor, [self, id, event, bytes] { self->on_inbound_event(id, event, bytes); });
         }
      }
   });
}

void session::impl::on_inbound_event(std::uint64_t id, forge::api::core::detail::stream_event event,
                                     std::size_t bytes) {
   const auto found = calls.find(id);
   if (found == calls.end() || !found->second->inbound) {
      return;
   }
   auto& flow = *found->second->inbound;
   const auto released = std::min<std::uint64_t>(flow.buffered_bytes, bytes);
   flow.buffered_bytes -= released;
   if (event == forge::api::core::detail::stream_event::consumed) {
      if (flow.buffered_items > 0) {
         --flow.buffered_items;
      }
   } else {
      flow.buffered_items = 0;
      flow.buffered_bytes = 0;
   }
   const auto call = found->second;
   if (call->done && flow.buffered_bytes == 0) {
      finish_call(call);
   }
   replenish_inbound_credit();
}

std::uint64_t session::impl::aggregate_buffered_bytes() const noexcept {
   auto total = std::uint64_t{0};
   for (const auto& [_, call] : calls) {
      if (call->inbound) {
         const auto value = call->inbound->buffered_bytes;
         total = value > std::numeric_limits<std::uint64_t>::max() - total ? std::numeric_limits<std::uint64_t>::max()
                                                                           : total + value;
      }
   }
   return total;
}

std::uint64_t session::impl::aggregate_outstanding_credit() const noexcept {
   auto total = std::uint64_t{0};
   for (const auto& [_, call] : calls) {
      if (call->inbound && !call->inbound->ended && !call->done &&
          call->inbound->limit_bytes >= call->inbound->transferred_bytes) {
         const auto value = call->inbound->limit_bytes - call->inbound->transferred_bytes;
         total = value > std::numeric_limits<std::uint64_t>::max() - total ? std::numeric_limits<std::uint64_t>::max()
                                                                           : total + value;
      }
   }
   return total;
}

void session::impl::replenish_inbound_credit() {
   if (!peer_hello_received) {
      return;
   }
   const auto buffered = aggregate_buffered_bytes();
   const auto outstanding = aggregate_outstanding_credit();
   auto used = outstanding > std::numeric_limits<std::uint64_t>::max() - buffered
                   ? std::numeric_limits<std::uint64_t>::max()
                   : buffered + outstanding;
   for (auto& [_, call] : calls) {
      if (!call->inbound || call->inbound->ended || call->done) {
         continue;
      }
      auto& flow = *call->inbound;
      const auto consumed_items = flow.transferred_items - flow.buffered_items;
      const auto consumed_bytes = flow.transferred_bytes - flow.buffered_bytes;
      const auto item_window = std::uint64_t{negotiated_limits.initial_window_items};
      const auto byte_window = std::uint64_t{negotiated_limits.initial_window_bytes};
      const auto item_target = item_window > std::numeric_limits<std::uint64_t>::max() - consumed_items
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : consumed_items + item_window;
      const auto byte_target = byte_window > std::numeric_limits<std::uint64_t>::max() - consumed_bytes
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : consumed_bytes + byte_window;
      const auto available =
          used >= negotiated_limits.max_buffered_bytes ? std::uint64_t{0} : negotiated_limits.max_buffered_bytes - used;
      const auto desired = byte_target > flow.limit_bytes ? byte_target - flow.limit_bytes : std::uint64_t{0};
      const auto grant = std::min(available, desired);
      const auto next_items = std::max(flow.limit_items, item_target);
      const auto next_bytes = flow.limit_bytes + grant;
      if (next_items == flow.limit_items && next_bytes == flow.limit_bytes) {
         continue;
      }
      flow.limit_items = next_items;
      flow.limit_bytes = next_bytes;
      used += grant;
      enqueue_control(forge::api::core::frame{
          .kind = forge::api::core::frame_kind::stream_window,
          .id = call->id,
          .api = call->api,
          .method = call->method,
          .codec = call->codec,
          .payload = forge::raw::pack(forge::api::core::stream_window{
              .direction = flow.direction,
              .max_items = flow.limit_items,
              .max_bytes = flow.limit_bytes,
          }),
      });
   }
}

boost::asio::awaitable<void> session::impl::async_serve_on_strand() {
   if (!plan) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "API stream session has no binding plan");
   }
   co_await ensure_handshake_on_strand();
   while (!closed.load(std::memory_order_acquire)) {
      const auto observed = session_wake.epoch();
      if (failure) {
         std::rethrow_exception(failure);
      }
      static_cast<void>(co_await session_wake.async_wait(observed));
   }
   if (failure) {
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<void> session::impl::async_close_on_strand() {
   if (closed.load(std::memory_order_acquire)) {
      co_return;
   }
   accepting = false;
   closing = true;
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   const auto grace =
       peer_hello_received ? std::chrono::milliseconds{negotiated_limits.shutdown_grace_ms} : settings.disconnect_grace;
   const auto grace_deadline = timer::clock_type::now() + grace;
   while (!calls.empty() && grace.count() > 0 && timer::clock_type::now() < grace_deadline) {
      const auto observed = session_wake.epoch();
      static_cast<void>(co_await session_wake.async_wait_until(observed, grace_deadline));
   }

   auto active = std::vector<std::shared_ptr<call_state>>{};
   active.reserve(calls.size());
   for (const auto& [_, call] : calls) {
      active.push_back(call);
   }
   for (const auto& call : active) {
      cancel_call(call, cancelled_call_error(call->id.value), true);
   }

   const auto write_deadline = timer::clock_type::now() + settings.control_timeout;
   while (!writer_idle() && timer::clock_type::now() < write_deadline) {
      const auto observed = session_wake.epoch();
      static_cast<void>(co_await session_wake.async_wait_until(observed, write_deadline));
   }
   try {
      co_await stream.async_close();
   } catch (...) {
      // Local close remains deterministic even if the peer already closed.
   }
   closed.store(true, std::memory_order_release);
   wake_writer();
   wake_session();
   stream.cancel();
}

} // namespace forge::api::stream
