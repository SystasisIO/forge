module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

module forge.net.yamux.session;

import forge.asio.gate;
import forge.asio.notification;
import forge.net.transport.exceptions;

#include "details/session_impl.hxx"
#include "details/session_impl_stream_state.hxx"
#include "details/session_impl_stream_model.hxx"
#include "details/session_close_failure.hxx"
#include "details/session_start_failure.hxx"

namespace forge::net::yamux {
namespace {

[[nodiscard]] std::exception_ptr make_exception(exceptions::code value, const char* message) noexcept {
   try {
      switch (value) {
      case exceptions::code::invalid_options:
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, message);
      case exceptions::code::protocol_error:
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, message);
      case exceptions::code::resource_limit:
         FORGE_THROW_EXCEPTION(exceptions::resource_limit, message);
      case exceptions::code::stream_reset:
         FORGE_THROW_EXCEPTION(exceptions::stream_reset, message);
      case exceptions::code::closed:
         FORGE_THROW_EXCEPTION(exceptions::closed, message);
      case exceptions::code::canceled:
         FORGE_THROW_EXCEPTION(exceptions::canceled, message);
      }
   } catch (...) {
      return std::current_exception();
   }
   return {};
}

void cancel_timer_noexcept(boost::asio::steady_timer& timer) noexcept {
   try {
      timer.cancel();
   } catch (...) {
   }
}

} // namespace

session::impl::impl(transport::stream stream, side session_side, options session_options)
    : stream_(std::move(stream)), side_(session_side), options_(session_options) {
   validate_options();
   next_stream_id_ = side_ == side::initiator ? 1U : 2U;
}

bool session::impl::valid() const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return stream_.valid() && !closed_ && !canceled_;
}

bool session::impl::exceeds_limit(std::size_t current, std::size_t addition, std::size_t limit) noexcept {
   return current > limit || addition > limit - current;
}

bool session::impl::remote_opens_stream(side local_side, std::uint32_t stream_id) noexcept {
   const auto remote_is_initiator = local_side == side::responder;
   const auto id_is_odd = (stream_id % 2U) == 1U;
   return remote_is_initiator == id_is_odd;
}

std::chrono::steady_clock::time_point session::impl::deadline_after(
    std::chrono::milliseconds timeout) noexcept {
   const auto now = std::chrono::steady_clock::now();
   const auto remaining = std::chrono::steady_clock::time_point::max() - now;
   const auto maximum_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
   if (timeout >= maximum_timeout) {
      return std::chrono::steady_clock::time_point::max();
   }
   return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
}

void session::impl::validate_options() const {
   if (!stream_.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "yamux requires a valid transport stream");
   }
   if (options_.initial_window < detail::initial_stream_window ||
       options_.max_stream_window < options_.initial_window || options_.max_frame_size == 0 ||
       options_.max_streams == 0 || options_.max_pending_accepts == 0 ||
       options_.max_stream_buffer < options_.initial_window || options_.max_session_buffer < options_.initial_window ||
       options_.write_timeout.count() <= 0 || options_.close_timeout.count() <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid yamux options");
   }
   if (options_.max_frame_size > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "yamux frame size exceeds wire limit");
   }
}

std::uint32_t session::impl::local_window_delta() const noexcept {
   return options_.initial_window - detail::initial_stream_window;
}

std::uint32_t session::impl::checked_peer_window(std::uint32_t current, std::uint32_t delta) const {
   if (current > options_.max_stream_window || delta > (std::numeric_limits<std::uint32_t>::max)() - current ||
       delta > options_.max_stream_window - current) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "yamux peer window update exceeds configured limit");
   }
   return current + delta;
}

boost::asio::awaitable<void> session::impl::ensure_started() {
   auto executor = co_await boost::asio::this_coro::executor;
   while (true) {
      const auto observed = start_notification_.epoch();
      auto owns_start = false;
      auto start_error = std::exception_ptr{};
      {
         auto lock = std::scoped_lock{mutex_};
         if (!executor_) {
            executor_ = executor;
         }
         switch (start_state_) {
         case start_state::idle:
            // Do not race a pre-start cancellation into launching workers.
            if (terminal_error_ || canceled_) {
               start_error = terminal_error_ ? terminal_error_
                                             : make_exception(exceptions::code::canceled, "yamux session canceled");
               break;
            }
            start_state_ = start_state::starting;
            owns_start = true;
            break;
         case start_state::starting:
            break;
         case start_state::started:
            co_return;
         case start_state::failed:
            start_error = start_error_;
            break;
         }
      }
      if (start_error) {
         std::rethrow_exception(start_error);
      }
      if (!owns_start) {
         (void)co_await start_notification_.async_wait(observed);
         continue;
      }

      auto setup_error = std::exception_ptr{};
      auto stream_cancel_worker_started = false;
      try {
         if (detail::consume_session_start_failure_for_test()) {
            throw std::bad_alloc{};
         }
         stream_cancel_worker_state_.store(
             (stream_cancel_publication_state_.load(std::memory_order_acquire) &
              stream_cancel_publication_closed) == 0
                 ? stream_cancel_worker_state::running
                 : stream_cancel_worker_state::stopping,
             std::memory_order_release);
         auto cancel_owner = shared_from_this();
         boost::asio::co_spawn(
             executor, cancel_owner->stream_cancel_loop(),
             boost::asio::bind_executor(executor, [cancel_owner](std::exception_ptr error) noexcept {
                if (error) {
                   cancel_owner->fail_session(exceptions::code::closed, "yamux stream reset writer stopped");
                   (void)cancel_owner->cancel_transport_noexcept();
                }
                cancel_owner->finish_stream_cancel_loop();
             }));
         stream_cancel_worker_started = true;

         auto read_owner = shared_from_this();
         boost::asio::co_spawn(executor, read_owner->read_loop(), [read_owner](std::exception_ptr error) noexcept {
            if (error) {
               read_owner->fail_session(exceptions::code::closed, "yamux read loop stopped");
               read_owner->finish_read_loop();
            }
         });
      } catch (...) {
         setup_error = std::current_exception();
      }
      if (setup_error) {
         request_stream_cancel_loop_stop();
         if (stream_cancel_worker_started) {
            co_await wait_for_stream_cancel_loop();
         } else {
            finish_stream_cancel_loop();
         }
         fail_start(setup_error);
         std::rethrow_exception(setup_error);
      }
      {
         auto lock = std::scoped_lock{mutex_};
         if (start_state_ == start_state::starting) {
            start_state_ = start_state::started;
         }
      }
      start_notification_.notify();
      co_return;
   }
}

boost::asio::awaitable<void> session::impl::async_close() {
   auto terminal_error = std::exception_ptr{};
   auto workers_started = false;
   auto locally_canceled = false;
   {
      auto lock = std::scoped_lock{mutex_};
      terminal_error = terminal_error_;
      workers_started = start_state_ != start_state::idle;
      locally_canceled = canceled_;
   }
   if (locally_canceled) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      if (!start_close(detail::go_away_normal)) {
         co_await wait_for_close();
         co_return;
      }
      try {
         // A prior cancellation may leave a reset writer blocked in lower I/O.
         // Close the lower transport first, then join the Yamux workers it wakes.
         co_await stream_.async_close();
      } catch (...) {
      }
      if (workers_started) {
         co_await wait_for_stream_cancel_loop();
         co_await wait_for_read_loop();
      }
      finish_close(terminal_error);
      std::rethrow_exception(terminal_error);
   }

   auto start_error = std::exception_ptr{};
   try {
      co_await ensure_started();
   } catch (...) {
      start_error = std::current_exception();
   }
   if (start_error) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      if (start_close(detail::go_away_normal)) {
         try {
            // A failed worker start has only canceled the lower stream so far.
            co_await stream_.async_close();
         } catch (...) {
         }
         finish_close(start_error);
      } else {
         try {
            co_await wait_for_close();
         } catch (...) {
         }
      }
      std::rethrow_exception(start_error);
   }
   if (!start_close(detail::go_away_normal)) {
      co_await wait_for_close();
      co_return;
   }

   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});

   auto error = std::exception_ptr{};
   auto deadline_timer = std::shared_ptr<boost::asio::steady_timer>{};
   auto lower_close_started = false;
   auto close_lower_after_error = false;
   try {
      auto executor = co_await boost::asio::this_coro::executor;
      const auto deadline = deadline_after(options_.close_timeout);
      deadline_timer = std::make_shared<boost::asio::steady_timer>(executor, deadline);
      auto weak = weak_from_this();
      deadline_timer->async_wait([weak, deadline_timer](const boost::system::error_code& timer_error) {
         if (timer_error) {
            return;
         }
         if (auto self = weak.lock()) {
            self->fail_session(exceptions::code::closed, "yamux close deadline expired");
            (void)self->cancel_transport_noexcept();
         }
      });

      if (detail::consume_session_close_failure_for_test()) {
         throw std::bad_alloc{};
      }

      request_stream_cancel_loop_stop();
      co_await wait_for_stream_cancel_loop();

      auto writes_drained = true;
      transport_writes_.seal();
      writes_drained = co_await transport_writes_.async_wait_until(deadline);
      if (!writes_drained) {
         write_gate_.close();
         (void)cancel_transport_noexcept();
      }

      auto terminal_writer = forge::asio::gate::ticket{};
      try {
         terminal_writer = co_await write_gate_.acquire();
      } catch (const forge::asio::exceptions::rejected&) {
      }

      if (writes_drained) {
         try {
            auto outbound =
                transport::chunk{detail::encode_frame(detail::frame_type::go_away, 0, 0, close_go_away_code())};
            co_await stream_.async_write(std::move(outbound));
         } catch (...) {
            // Closing is best-effort once the underlying byte stream has already failed.
         }
      }
      try {
         lower_close_started = true;
         co_await stream_.async_close();
      } catch (...) {
      }
      fail_session(exceptions::code::closed, "yamux session closed");
      if (!co_await wait_for_read_loop_until(deadline)) {
         if (cancel_transport_noexcept()) {
            co_await wait_for_read_loop();
         }
      }
   } catch (...) {
      error = std::current_exception();
      request_stream_cancel_loop_stop();
      (void)cancel_transport_noexcept();
      close_lower_after_error = !lower_close_started;
   }
   if (close_lower_after_error) {
      lower_close_started = true;
      try {
         co_await stream_.async_close();
      } catch (...) {
      }
   }
   if (error) {
      if (stream_cancel_worker_state_.load(std::memory_order_acquire) != stream_cancel_worker_state::done) {
         co_await wait_for_stream_cancel_loop();
      }
      co_await wait_for_read_loop();
   }
   if (deadline_timer) {
      cancel_timer_noexcept(*deadline_timer);
   }
   finish_close(error);
   if (error) {
      std::rethrow_exception(error);
   }
}

void session::impl::cancel() {
   request_cancel();
}

void session::impl::request_cancel() noexcept {
   fail_session(exceptions::code::canceled, "yamux session canceled");
   stream_.request_cancel();
}

std::shared_ptr<session::impl::stream_state> session::impl::make_stream_locked(std::uint32_t id,
                                                                               std::uint32_t send_window) {
   if (send_window > options_.max_stream_window) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "yamux peer window exceeds configured limit");
   }
   auto state = std::make_shared<stream_state>(id);
   state->send_window = send_window;
   state->receive_window = options_.initial_window;
   return state;
}

void session::impl::require_stream_owned_locked(const std::shared_ptr<stream_state>& state,
                                                bool allow_completed) const {
   if (state->cancel_requested.load(std::memory_order_acquire) || state->reset) {
      FORGE_THROW_EXCEPTION(exceptions::stream_reset, "yamux stream reset");
   }
   const auto found = streams_.find(state->id);
   if (found == streams_.end() || found->second != state) {
      // A surviving facade still owns the terminal result after the bounded
      // session table reclaims an orderly, fully drained stream.
      if (allow_completed && found == streams_.end() && state->local_fin_sent &&
          state->remote_fin && state->inbound.empty() && state->pending_receive_credit == 0) {
         return;
      }
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux stream does not exist");
   }
}

bool session::impl::stream_valid(const std::shared_ptr<stream_state>& state) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   const auto found = streams_.find(state->id);
   return found != streams_.end() && found->second == state && !closed_ && !canceled_ && !state->reset &&
          !state->cancel_requested.load(std::memory_order_acquire);
}

transport::stream session::impl::make_transport_stream(const std::shared_ptr<stream_state>& state) {
   auto model = std::make_shared<stream_model>(shared_from_this(), state);
   auto cancel_on_failure = std::unique_ptr<stream_model, void (*)(stream_model*)>{
       model.get(), [](stream_model* stream) { stream->request_cancel(); }};
   auto weak = std::weak_ptr<stream_model>{model};
   auto result = transport::detail::stream_access::make_cancelable(
       std::move(model), [weak = std::move(weak)]() noexcept {
          if (auto stream = weak.lock()) {
             stream->request_cancel();
          }
       });
   static_cast<void>(cancel_on_failure.release());
   return result;
}

void session::impl::rethrow_terminal_locked() const {
   if (terminal_error_) {
      std::rethrow_exception(terminal_error_);
   }
   if (canceled_) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "yamux session canceled");
   }
   if (closed_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux session closed");
   }
}

bool session::impl::start_close(std::uint32_t go_away_code) {
   {
      auto lock = std::scoped_lock{mutex_};
      if (go_away_code != detail::go_away_normal && close_go_away_code_ == detail::go_away_normal) {
         close_go_away_code_ = go_away_code;
      }
      if (close_started_) {
         return false;
      }
      close_started_ = true;
      closed_ = true;
      wake_all_locked();
   }
   request_stream_cancel_loop_stop();
   return true;
}

std::uint32_t session::impl::close_go_away_code() const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return close_go_away_code_;
}

boost::asio::awaitable<void> session::impl::wait_for_close() {
   auto error = std::exception_ptr{};
   while (true) {
      const auto observed = close_notification_.epoch();
      {
         auto lock = std::scoped_lock{mutex_};
         if (close_done_) {
            error = close_error_;
            break;
         }
      }
      (void)co_await close_notification_.async_wait(observed);
   }
   if (error) {
      std::rethrow_exception(error);
   }
}

void session::impl::finish_close(std::exception_ptr error) noexcept {
   {
      auto lock = std::scoped_lock{mutex_};
      close_error_ = std::move(error);
      close_done_ = true;
   }
   close_notification_.notify();
}

bool session::impl::cancel_transport_noexcept() noexcept {
   stream_.request_cancel();
   return true;
}

void session::impl::fail_start(std::exception_ptr error) noexcept {
   auto close_write_gate = false;
   {
      auto lock = std::scoped_lock{mutex_};
      if (start_state_ == start_state::failed) {
         return;
      }
      start_state_ = start_state::failed;
      start_error_ = error;
      read_loop_done_ = true;
      if (!terminal_error_) {
         terminal_error_ = std::move(error);
         closed_ = true;
         close_write_gate = true;
      }
      wake_all_locked();
   }
   if (close_write_gate) {
      transport_writes_.seal();
      write_gate_.close();
   }
   start_notification_.notify();
   read_loop_notification_.notify();
   request_stream_cancel_loop_stop();
   (void)cancel_transport_noexcept();
}

void session::impl::fail_session(exceptions::code value, const char* message) noexcept {
   auto first_transition = false;
   {
      auto lock = std::scoped_lock{mutex_};
      if (terminal_error_) {
         return;
      }
      terminal_error_ = make_exception(value, message);
      first_transition = true;
      if (value == exceptions::code::canceled) {
         canceled_ = true;
      } else {
         closed_ = true;
      }
      for (const auto& [_, state] : streams_) {
         state->reset = value == exceptions::code::protocol_error || value == exceptions::code::resource_limit;
      }
      wake_all_locked();
   }
   if (first_transition) {
      // Writers rejected after this point must see the terminal error published above.
      request_stream_cancel_loop_stop();
      transport_writes_.seal();
      write_gate_.close();
   }
}

void session::impl::wake_all_locked() {
   accept_notification_.notify();
   for (const auto& [_, state] : streams_) {
      notify_stream_waiters_locked(state);
   }
}

boost::asio::awaitable<bool> session::impl::write_prepared(std::function<std::optional<detail::bytes>()> prepare,
                                                           bool allow_after_close, std::shared_ptr<void> lifetime,
                                                           std::shared_ptr<stream_state> frame_state) {
   if (!allow_after_close) {
      auto lock = std::scoped_lock{mutex_};
      rethrow_terminal_locked();
   }

   auto transport_write = transport_writes_.try_reserve();
   if (!transport_write) {
      auto lock = std::scoped_lock{mutex_};
      rethrow_terminal_locked();
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux transport write admission is closed");
   }
   auto operation_lifetime = std::move(lifetime);
   co_return co_await write_admitted(std::move(prepare), allow_after_close, std::move(operation_lifetime),
                                     std::move(frame_state));
}

boost::asio::awaitable<bool> session::impl::write_admitted(std::function<std::optional<detail::bytes>()> prepare,
                                                           bool allow_after_close, std::shared_ptr<void> lifetime,
                                                           std::shared_ptr<stream_state> frame_state) {
   auto wrote = false;
   auto ticket = forge::asio::gate::ticket{};
   try {
      ticket = co_await write_gate_.acquire();
   } catch (const forge::asio::exceptions::canceled&) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "yamux write was canceled while waiting");
   } catch (const forge::asio::exceptions::rejected&) {
      auto lock = std::scoped_lock{mutex_};
      rethrow_terminal_locked();
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux write gate is closed");
   }

   // A prepared state transition and its serialized frame complete together once this owns the writer.
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});

   if (!allow_after_close) {
      auto lock = std::scoped_lock{mutex_};
      rethrow_terminal_locked();
   }

   auto executor = co_await boost::asio::this_coro::executor;
   auto deadline_state = std::make_shared<std::atomic<transport_write_state>>(transport_write_state::active);
   auto deadline_timer = std::make_shared<boost::asio::steady_timer>(executor, deadline_after(options_.write_timeout));
   auto weak = weak_from_this();
   deadline_timer->async_wait([weak, deadline_state, deadline_timer](const boost::system::error_code& error) noexcept {
      if (error) {
         return;
      }
      auto expected = transport_write_state::active;
      if (!deadline_state->compare_exchange_strong(expected, transport_write_state::expired,
                                                   std::memory_order_acq_rel, std::memory_order_acquire)) {
         return;
      }
      if (auto self = weak.lock()) {
         self->fail_session(exceptions::code::closed, "yamux transport write deadline expired");
         (void)self->cancel_transport_noexcept();
      }
   });

   auto encoded = std::optional<detail::bytes>{};
   try {
      encoded = prepare();
      if (!encoded) {
         deadline_state->store(transport_write_state::completed, std::memory_order_release);
         cancel_timer_noexcept(*deadline_timer);
         co_return false;
      }

      if (frame_state) {
         auto lock = std::scoped_lock{mutex_};
         if (allow_after_close) {
            const auto found = streams_.find(frame_state->id);
            if (found == streams_.end() || found->second != frame_state) {
               FORGE_THROW_EXCEPTION(exceptions::stream_reset, "yamux stream is no longer owned by this session");
            }
         } else {
            require_stream_owned_locked(frame_state);
         }
      }
   } catch (...) {
      deadline_state->store(transport_write_state::completed, std::memory_order_release);
      cancel_timer_noexcept(*deadline_timer);
      throw;
   }

   if (deadline_state->load(std::memory_order_acquire) != transport_write_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux transport write deadline expired");
   }
   try {
      auto outbound = transport::chunk{std::move(*encoded)};
      transport::detail::chunk_access::attach_lifetime(outbound, std::move(lifetime));
      co_await stream_.async_write(std::move(outbound));
      auto expected = transport_write_state::active;
      if (!deadline_state->compare_exchange_strong(expected, transport_write_state::completed,
                                                   std::memory_order_acq_rel, std::memory_order_acquire)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "yamux transport write deadline expired");
      }
      wrote = true;
   } catch (...) {
      auto expected = transport_write_state::active;
      static_cast<void>(deadline_state->compare_exchange_strong(
          expected, transport_write_state::completed, std::memory_order_acq_rel, std::memory_order_acquire));
      cancel_timer_noexcept(*deadline_timer);
      fail_session(exceptions::code::closed, "yamux underlying stream write failed");
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux underlying stream write failed");
   }
   cancel_timer_noexcept(*deadline_timer);
   co_return wrote;
}

boost::asio::awaitable<void> session::impl::write_frame(detail::frame_type type, std::uint16_t flags,
                                                        std::uint32_t stream_id, std::uint32_t length,
                                                        std::span<const std::uint8_t> payload, bool allow_after_close,
                                                        std::shared_ptr<void> lifetime,
                                                        std::shared_ptr<stream_state> frame_state) {
   (void)co_await write_prepared(
       [type, flags, stream_id, length, payload]() -> std::optional<detail::bytes> {
          return detail::encode_frame(type, flags, stream_id, length, payload);
       },
       allow_after_close, std::move(lifetime), std::move(frame_state));
}

boost::asio::awaitable<void> session::impl::wait_for_read_loop() {
   while (true) {
      const auto observed = read_loop_notification_.epoch();
      {
         auto lock = std::scoped_lock{mutex_};
         if (read_loop_done_) {
            co_return;
         }
      }
      (void)co_await read_loop_notification_.async_wait(observed);
   }
}

boost::asio::awaitable<bool> session::impl::wait_for_read_loop_until(std::chrono::steady_clock::time_point deadline) {
   while (true) {
      const auto observed = read_loop_notification_.epoch();
      {
         auto lock = std::scoped_lock{mutex_};
         if (read_loop_done_) {
            co_return true;
         }
      }
      (void)co_await read_loop_notification_.async_wait_until(observed, deadline);
      {
         auto lock = std::scoped_lock{mutex_};
         if (read_loop_done_) {
            co_return true;
         }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         co_return false;
      }
   }
}

void session::impl::finish_read_loop() noexcept {
   {
      auto lock = std::scoped_lock{mutex_};
      read_loop_done_ = true;
   }
   read_loop_notification_.notify();
}

} // namespace forge::net::yamux
