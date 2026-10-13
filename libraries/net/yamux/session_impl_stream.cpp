module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

module forge.net.yamux.session;

import forge.asio.gate;
import forge.asio.notification;

#include "details/session_impl.hxx"
#include "details/stream_close_failure.hxx"
#include "details/session_impl_stream_state.hxx"

namespace forge::net::yamux {

boost::asio::awaitable<transport::stream> session::impl::async_open_stream() {
   co_await ensure_started();

   std::shared_ptr<stream_state> state;
   co_await write_prepared([this, &state]() -> std::optional<detail::bytes> {
      auto lock = std::scoped_lock{mutex_};
      rethrow_terminal_locked();
      reclaim_closed_streams_locked();
      if (streams_.size() >= options_.max_streams) {
         FORGE_THROW_EXCEPTION(exceptions::resource_limit, "yamux maximum stream count reached");
      }
      if (!next_stream_id_) {
         FORGE_THROW_EXCEPTION(exceptions::resource_limit, "yamux stream ids are exhausted");
      }
      const auto id = *next_stream_id_;
      auto encoded = detail::encode_frame(detail::frame_type::window_update, detail::syn, id, local_window_delta());
      auto prepared = make_stream_locked(id, detail::initial_stream_window);
      prepared->accepted = true;
      streams_.emplace(id, prepared);
      next_stream_id_ = detail::can_advance_stream_id(id) ? std::optional<std::uint32_t>{id + 2U} : std::nullopt;
      state = std::move(prepared);
      return encoded;
   });
   co_return make_transport_stream(state);
}

boost::asio::awaitable<transport::stream> session::impl::async_accept_stream() {
   co_await ensure_started();

   while (true) {
      const auto observed = accept_notification_.epoch();
      auto state = std::shared_ptr<stream_state>{};
      {
         auto lock = std::scoped_lock{mutex_};
         if (!pending_accepts_.empty()) {
            const auto id = pending_accepts_.front();
            pending_accepts_.pop_front();
            if (const auto found = streams_.find(id); found != streams_.end()) {
               found->second->accepted = true;
               state = found->second;
            }
         } else {
            rethrow_terminal_locked();
         }
      }
      if (state) {
         co_return make_transport_stream(state);
      }
      (void)co_await accept_notification_.async_wait(observed);
   }
}

boost::asio::awaitable<void> session::impl::write_stream(const std::shared_ptr<stream_state>& state,
                                                         detail::bytes payload, std::shared_ptr<void> lifetime) {
   co_await ensure_started();

   auto transport_write = transport_writes_.try_reserve();
   if (!transport_write) {
      auto lock = std::scoped_lock{mutex_};
      require_stream_owned_locked(state);
      rethrow_terminal_locked();
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux transport write admission is closed");
   }
   auto operation_lifetime = std::move(lifetime);
   auto offset = std::size_t{0};
   while (offset < payload.size()) {
      {
         auto lock = std::scoped_lock{mutex_};
         require_stream_owned_locked(state);
         if (state->local_fin) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "yamux stream is locally closed");
         }
      }

      while (true) {
         const auto observed = state->window_notification.epoch();
         {
            auto lock = std::scoped_lock{mutex_};
            require_stream_owned_locked(state);
            rethrow_terminal_locked();
            if (state->send_window > 0) {
               break;
            }
         }
         (void)co_await state->window_notification.async_wait(observed);
      }

      auto written = std::size_t{0};
      const auto sent = co_await write_admitted(
          [this, state, &payload, &offset, &written]() -> std::optional<detail::bytes> {
             auto lock = std::scoped_lock{mutex_};
             require_stream_owned_locked(state);
             rethrow_terminal_locked();
             if (state->local_fin) {
                FORGE_THROW_EXCEPTION(exceptions::closed, "yamux stream is locally closed");
             }
             if (state->send_window == 0) {
                return std::nullopt;
             }
             written = std::min<std::size_t>({payload.size() - offset, options_.max_frame_size, state->send_window});
             const auto chunk = std::span<const std::uint8_t>{payload.data() + offset, written};
             auto encoded = detail::encode_frame(detail::frame_type::data, 0, state->id,
                                                 static_cast<std::uint32_t>(written), chunk);
             state->send_window -= static_cast<std::uint32_t>(written);
             return encoded;
          },
          false, operation_lifetime, state);
      if (sent) {
         offset += written;
      }
   }
}

boost::asio::awaitable<detail::bytes> session::impl::read_stream(const std::shared_ptr<stream_state>& state) {
   co_await ensure_started();

   while (true) {
      const auto observed = state->read_notification.epoch();
      auto has_data = false;
      {
         auto lock = std::scoped_lock{mutex_};
         require_stream_owned_locked(state, true);
         if (!state->inbound.empty()) {
            has_data = true;
         } else {
            rethrow_terminal_locked();
            if (state->remote_fin) {
               FORGE_THROW_EXCEPTION(exceptions::end_of_stream, "yamux stream closed by remote");
            }
         }
      }

      if (has_data) {
         auto consumed = std::uint32_t{0};
         auto out = detail::bytes{};
         auto credit_pending = false;
         auto sent = false;
         try {
            sent = co_await write_prepared(
                [this, state, &consumed, &out, &credit_pending]() -> std::optional<detail::bytes> {
                   auto lock = std::scoped_lock{mutex_};
                   require_stream_owned_locked(state);
                   rethrow_terminal_locked();
                   if (state->inbound.empty()) {
                      return std::nullopt;
                   }
                   consumed = static_cast<std::uint32_t>(state->inbound.front().size());
                   if (state->receive_window > options_.initial_window ||
                       state->pending_receive_credit > options_.initial_window - state->receive_window ||
                       consumed > options_.initial_window - state->receive_window - state->pending_receive_credit) {
                      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "yamux receive window accounting overflow");
                   }
                   auto encoded = detail::encode_frame(detail::frame_type::window_update, 0, state->id, consumed);
                   out = std::move(state->inbound.front());
                   state->inbound.pop_front();
                   state->buffered -= out.size();
                   session_buffer_ -= out.size();
                   state->pending_receive_credit += consumed;
                   credit_pending = true;
                   return encoded;
                },
                false, {}, state);
         } catch (...) {
            if (credit_pending) {
               auto lock = std::scoped_lock{mutex_};
               state->pending_receive_credit -= consumed;
               state->receive_credit_notification.notify();
            }
            throw;
         }
         if (!sent) {
            continue;
         }
         {
            auto lock = std::scoped_lock{mutex_};
            state->pending_receive_credit -= consumed;
            state->receive_window += consumed;
            state->receive_credit_notification.notify();
         }
         co_return out;
      }
      (void)co_await state->read_notification.async_wait(observed);
   }
}

boost::asio::awaitable<void> session::impl::close_stream(const std::shared_ptr<stream_state>& state) {
   auto error = std::exception_ptr{};
   auto reset_required = false;
   try {
      co_await ensure_started();
      const auto sent = co_await write_prepared(
          [this, state]() -> std::optional<detail::bytes> {
             auto lock = std::scoped_lock{mutex_};
             require_stream_owned_locked(state, true);
             if (state->local_fin || state->reset) {
                return std::nullopt;
             }
             if (detail::consume_stream_close_failure_for_test()) {
                throw std::bad_alloc{};
             }
             auto encoded = detail::encode_frame(detail::frame_type::data, detail::fin, state->id, 0);
             state->local_fin = true;
             return encoded;
          },
          false, {}, state);
      if (sent) {
         auto lock = std::scoped_lock{mutex_};
         state->local_fin_sent = true;
      }
   } catch (...) {
      error = std::current_exception();
      auto lock = std::scoped_lock{mutex_};
      // An exception before local FIN leaves the logical stream live. Its
      // terminal reset must complete before the facade publishes this error.
      reset_required = !state->local_fin && !state->reset && !terminal_error_ && !canceled_ && !closed_;
   }
   if (reset_required) {
      request_cancel_stream(state);
      co_await wait_for_stream_terminal_reset(state);
   }
   if (error) {
      std::rethrow_exception(error);
   }
}

bool session::impl::is_reclaimable_stream_locked(const stream_state& state) const noexcept {
   if (state.cancel_requested.load(std::memory_order_acquire) && !state.local_reset_sent) {
      return false;
   }
   if (state.reset) {
      return true;
   }
   return state.local_fin_sent && state.remote_fin && state.inbound.empty() &&
          state.pending_receive_credit == 0;
}

void session::impl::reclaim_closed_streams_locked() {
   for (auto it = streams_.begin(); it != streams_.end();) {
      auto& state = *it->second;
      if (!is_reclaimable_stream_locked(state)) {
         ++it;
         continue;
      }
      std::erase(pending_accepts_, it->first);
      release_stream_buffers_locked(state);
      notify_stream_waiters_locked(it->second);
      it = streams_.erase(it);
   }
}

void session::impl::cancel_stream(const std::shared_ptr<stream_state>& state) {
   request_cancel_stream(state);
}

void session::impl::request_cancel_stream(const std::shared_ptr<stream_state>& state) noexcept {
   if (!enter_stream_cancel_publication()) {
      return;
   }
   if (!state->cancel_requested.exchange(true, std::memory_order_acq_rel)) {
      state->read_notification.notify();
      state->window_notification.notify();
      state->receive_credit_notification.notify();
   }
   leave_stream_cancel_publication();
   stream_cancel_notification_.notify();
}

boost::asio::awaitable<void> session::impl::stream_cancel_loop() {
   while (true) {
      const auto observed = stream_cancel_notification_.epoch();
      auto target = std::shared_ptr<stream_state>{};
      {
         auto lock = std::scoped_lock{mutex_};
         for (const auto& [_, candidate] : streams_) {
            if (!candidate->cancel_requested.load(std::memory_order_acquire) || candidate->local_reset_sent ||
                candidate->cancel_in_progress) {
               continue;
            }
            candidate->cancel_in_progress = true;
            target = candidate;
            break;
         }
      }

      if (target) {
         try {
            co_await write_frame(detail::frame_type::data, detail::rst, target->id, 0, {}, true, {}, target);
         } catch (...) {
            {
               auto lock = std::scoped_lock{mutex_};
               target->cancel_in_progress = false;
            }
            throw;
         }
         {
            auto lock = std::scoped_lock{mutex_};
            target->cancel_in_progress = false;
            target->local_reset_sent = true;
            if (!target->reset) {
               target->reset = true;
               release_stream_buffers_locked(*target);
            }
         }
         target->read_notification.notify();
         target->window_notification.notify();
         target->receive_credit_notification.notify();
         target->terminal_notification.notify();
         continue;
      }

      if (stream_cancel_worker_state_.load(std::memory_order_acquire) ==
              stream_cancel_worker_state::stopping &&
          (stream_cancel_publication_state_.load(std::memory_order_acquire) &
           stream_cancel_publication_count_mask) == 0) {
         co_return;
      }
      (void)co_await stream_cancel_notification_.async_wait(observed);
   }
}

boost::asio::awaitable<void> session::impl::wait_for_stream_terminal_reset(
    const std::shared_ptr<stream_state>& state) {
   while (true) {
      const auto observed = state->terminal_notification.epoch();
      {
         auto lock = std::scoped_lock{mutex_};
         if (state->reset || state->local_reset_sent || terminal_error_ || canceled_ || closed_) {
            co_return;
         }
      }
      (void)co_await state->terminal_notification.async_wait(observed);
   }
}

boost::asio::awaitable<void> session::impl::wait_for_stream_cancel_loop() {
   while (true) {
      const auto state = stream_cancel_worker_state_.load(std::memory_order_acquire);
      if (state == stream_cancel_worker_state::idle || state == stream_cancel_worker_state::done) {
         co_return;
      }
      const auto observed = stream_cancel_done_notification_.epoch();
      const auto rechecked = stream_cancel_worker_state_.load(std::memory_order_acquire);
      if (rechecked != stream_cancel_worker_state::idle && rechecked != stream_cancel_worker_state::done) {
         (void)co_await stream_cancel_done_notification_.async_wait(observed);
      }
   }
}

boost::asio::awaitable<bool>
session::impl::wait_for_stream_cancel_loop_until(std::chrono::steady_clock::time_point deadline) {
   while (true) {
      const auto state = stream_cancel_worker_state_.load(std::memory_order_acquire);
      if (state == stream_cancel_worker_state::idle || state == stream_cancel_worker_state::done) {
         co_return true;
      }
      const auto observed = stream_cancel_done_notification_.epoch();
      const auto rechecked = stream_cancel_worker_state_.load(std::memory_order_acquire);
      if (rechecked != stream_cancel_worker_state::idle && rechecked != stream_cancel_worker_state::done) {
         (void)co_await stream_cancel_done_notification_.async_wait_until(observed, deadline);
      }
      const auto completed = stream_cancel_worker_state_.load(std::memory_order_acquire);
      if (completed == stream_cancel_worker_state::idle || completed == stream_cancel_worker_state::done) {
         co_return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         co_return false;
      }
   }
}

bool session::impl::enter_stream_cancel_publication() noexcept {
   auto state = stream_cancel_publication_state_.load(std::memory_order_acquire);
   while ((state & stream_cancel_publication_closed) == 0) {
      if ((state & stream_cancel_publication_count_mask) == stream_cancel_publication_count_mask) {
         return false;
      }
      if (stream_cancel_publication_state_.compare_exchange_weak(
              state, state + 1U, std::memory_order_acq_rel, std::memory_order_acquire)) {
         return true;
      }
   }
   return false;
}

void session::impl::leave_stream_cancel_publication() noexcept {
   stream_cancel_publication_state_.fetch_sub(1U, std::memory_order_acq_rel);
}

void session::impl::request_stream_cancel_loop_stop() noexcept {
   stream_cancel_publication_state_.fetch_or(stream_cancel_publication_closed, std::memory_order_acq_rel);
   auto state = stream_cancel_worker_state_.load(std::memory_order_acquire);
   while (state == stream_cancel_worker_state::running &&
          !stream_cancel_worker_state_.compare_exchange_weak(
              state, stream_cancel_worker_state::stopping, std::memory_order_acq_rel, std::memory_order_acquire)) {
   }
   stream_cancel_notification_.notify();
}

void session::impl::finish_stream_cancel_loop() noexcept {
   stream_cancel_worker_state_.store(stream_cancel_worker_state::done, std::memory_order_release);
   stream_cancel_done_notification_.notify();
}

void session::impl::reset_stream_locked(const std::shared_ptr<stream_state>& state) {
   state->reset = true;
   release_stream_buffers_locked(*state);
   notify_stream_waiters_locked(state);
}

void session::impl::release_stream_buffers_locked(stream_state& state) {
   if (state.buffered == 0) {
      return;
   }
   session_buffer_ = state.buffered > session_buffer_ ? 0 : session_buffer_ - state.buffered;
   state.buffered = 0;
   state.inbound.clear();
}

void session::impl::notify_stream_waiters_locked(const std::shared_ptr<stream_state>& state) {
   state->read_notification.notify();
   state->window_notification.notify();
   state->receive_credit_notification.notify();
   state->terminal_notification.notify();
}

} // namespace forge::net::yamux
