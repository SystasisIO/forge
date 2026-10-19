module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

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
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module forge.api.stream.session;

import forge.asio.notification;
import forge.net.transport.buffer;

#include "details/session_impl.hxx"

namespace forge::api::stream {
namespace {

constexpr auto max_control_burst = std::size_t{8};

} // namespace

void session::impl::wake_writer() noexcept {
   if (writer_wake) {
      try {
         writer_wake->cancel();
      } catch (...) {
         // Writer completion paths remain noexcept.
      }
   }
}

void session::impl::enqueue_control(forge::api::core::frame value) {
   control_queue.push_back(queued_frame{.value = std::move(value)});
   wake_writer();
}

std::uint64_t session::impl::outbound_item_limit() const noexcept {
   const auto calls = std::uint64_t{negotiated_limits.max_inflight_calls};
   const auto items = std::uint64_t{negotiated_limits.initial_window_items};
   if (calls != 0 && items > std::numeric_limits<std::uint64_t>::max() / calls) {
      return std::numeric_limits<std::uint64_t>::max();
   }
   return calls * items;
}

void session::impl::wake_outbound_capacity() noexcept {
   for (const auto& [_, call] : calls) {
      wake_call(call);
   }
}

void session::impl::release_outbound_capacity(const queued_frame& value) noexcept {
   if (!value.buffered_item) {
      return;
   }
   if (outbound_buffered_items > 0) {
      --outbound_buffered_items;
   }
   const auto bytes = static_cast<std::uint64_t>(value.buffered_item_bytes);
   outbound_buffered_bytes = bytes > outbound_buffered_bytes ? std::uint64_t{0} : outbound_buffered_bytes - bytes;
   wake_outbound_capacity();
}

std::shared_ptr<session::impl::write_receipt> session::impl::enqueue_call_frame(const std::shared_ptr<call_state>& call,
                                                                                forge::api::core::frame value) {
   auto receipt = std::make_shared<write_receipt>(*strand);
   const auto buffered_item = value.kind == forge::api::core::frame_kind::stream_item;
   const auto buffered_item_bytes = buffered_item ? value.payload.size() : 0;
   call->write_queue.push_back(queued_frame{
       .value = std::move(value),
       .receipt = receipt,
       .buffered_item_bytes = buffered_item_bytes,
       .buffered_item = buffered_item,
   });
   if (buffered_item) {
      ++outbound_buffered_items;
      outbound_buffered_bytes += buffered_item_bytes;
   }
   if (!call->rr_queued) {
      call->rr_queued = true;
      round_robin.push_back(call->id.value);
   }
   wake_writer();
   return receipt;
}

std::optional<session::impl::queued_frame> session::impl::next_write_on_strand() {
   if (!control_queue.empty() && (round_robin.empty() || control_burst < max_control_burst)) {
      auto next = std::move(control_queue.front());
      control_queue.pop_front();
      if (control_burst < max_control_burst) {
         ++control_burst;
      }
      return next;
   }

   while (!round_robin.empty()) {
      const auto id = round_robin.front();
      round_robin.pop_front();
      const auto found = calls.find(id);
      if (found == calls.end()) {
         continue;
      }
      auto& call = found->second;
      call->rr_queued = false;
      if (call->write_queue.empty()) {
         continue;
      }
      auto next = std::move(call->write_queue.front());
      call->write_queue.pop_front();
      ++call->writes_in_flight;
      if (!call->write_queue.empty()) {
         call->rr_queued = true;
         round_robin.push_back(id);
      }
      control_burst = 0;
      return next;
   }
   if (!control_queue.empty()) {
      auto next = std::move(control_queue.front());
      control_queue.pop_front();
      if (control_burst < max_control_burst) {
         ++control_burst;
      }
      return next;
   }
   return std::nullopt;
}

boost::asio::awaitable<void> session::impl::writer_loop() {
   try {
      while (!closed.load(std::memory_order_acquire)) {
         auto next = next_write_on_strand();
         if (!next) {
            writer_wake->expires_at(timer::time_point::max());
            next = next_write_on_strand();
            if (!next) {
               auto error = boost::system::error_code{};
               co_await writer_wake->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
               continue;
            }
         }

         const auto kind = next->value.kind;
         const auto id = next->value.id.value;
         if (kind == forge::api::core::frame_kind::request) {
            const auto found = calls.find(id);
            if (found != calls.end()) {
               found->second->request_written = true;
            }
         }
         writer_write_in_flight = true;
         try {
            co_await stream.async_write(forge::net::transport::chunk{encode_wire_frame(next->value)});
            writer_write_in_flight = false;
            wake_session();
            touch_activity();
            release_outbound_capacity(*next);
            complete_receipt(next->receipt);
         } catch (...) {
            writer_write_in_flight = false;
            wake_session();
            release_outbound_capacity(*next);
            complete_receipt(next->receipt, std::current_exception());
            if (kind != forge::api::core::frame_kind::session_hello) {
               const auto found = calls.find(id);
               if (found != calls.end() && found->second->writes_in_flight > 0) {
                  --found->second->writes_in_flight;
               }
            }
            throw;
         }

         if (kind != forge::api::core::frame_kind::session_hello) {
            const auto found = calls.find(id);
            if (found != calls.end() && found->second->writes_in_flight > 0) {
               --found->second->writes_in_flight;
            }
         }

         if (kind == forge::api::core::frame_kind::session_hello) {
            hello_sent = true;
            wake_session();
         }

         if (kind == forge::api::core::frame_kind::response || kind == forge::api::core::frame_kind::error ||
             kind == forge::api::core::frame_kind::cancel) {
            const auto found = calls.find(id);
            if (found != calls.end()) {
               found->second->terminal_written = true;
            }
            if (found != calls.end() && found->second->done) {
               finish_call(found->second);
            }
         }
         if (const auto found = calls.find(id); found != calls.end()) {
            finish_call(found->second);
         }
         wake_session();
      }
   } catch (...) {
      writer_running = false;
      if (!closing) {
         fail_session(std::current_exception());
         stop_transport();
      } else {
         // Owned shutdown must drain cleanup without turning its transport
         // error into a new session failure or replacing a received API error.
         stop_transport();
         fail_calls(std::current_exception());
      }
      co_return;
   }
   writer_running = false;
   wake_session();
}

} // namespace forge::api::stream
