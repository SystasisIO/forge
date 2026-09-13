module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.reachability;
import forge.net.p2p.stream;
import forge.net.quic.exceptions;
import forge.net.transport.exceptions;
import forge.net.yamux.exceptions;

#include "details/autonat_v2_dialback.hxx"
#include "details/cancellation_latch.hxx"
#include "details/operation_deadline.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p::detail {
namespace {

bool child_cancel_error(const forge::exceptions::base& error) noexcept {
   return exceptions::is(error, exceptions::code::canceled) ||
          quic::exceptions::is(error, quic::exceptions::code::canceled) ||
          yamux::exceptions::is(error, yamux::exceptions::code::canceled) ||
          transport::exceptions::is(error, transport::exceptions::code::canceled);
}

void check_phase_failure(const std::exception_ptr& failure, bool child_expired) {
   if (!failure) { return; }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::exceptions::base& error) {
      if (!autonat_v2_remote_close(error) && !(child_expired && child_cancel_error(error))) {
         throw;
      }
   } catch (const boost::system::system_error& error) {
      if (!child_expired || error.code() != boost::asio::error::operation_aborted) { throw; }
   }
}

} // namespace

bool autonat_v2_remote_close(const forge::exceptions::base& error) noexcept {
   // QUIC's transport adapter canonicalizes stream/connection EOF to this
   // category. P2P closed (e.g. an invalid local facade) is intentionally absent.
   return transport::exceptions::is(error, transport::exceptions::code::closed) ||
          quic::exceptions::is(error, quic::exceptions::code::stream_reset) ||
          quic::exceptions::is(error, quic::exceptions::code::stream_closed) ||
          quic::exceptions::is(error, quic::exceptions::code::connection_closed) ||
          yamux::exceptions::is(error, yamux::exceptions::code::stream_reset) ||
          yamux::exceptions::is(error, yamux::exceptions::code::closed);
}

autonat_v2_dialback::autonat_v2_dialback(stream value, std::uint64_t value_nonce,
                                       boost::asio::io_context& value_context,
                                       std::chrono::steady_clock::time_point deadline)
    : channel{std::move(value)}, nonce{value_nonce}, context{value_context}, parent_deadline{deadline},
      stop{std::make_shared<worker_stop_bridge>()} {}

boost::asio::awaitable<void> autonat_v2_dialback::close() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   try {
      co_await channel.async_close();
   } catch (...) {
      close_completed = true; // Stream close is a terminal barrier even on error.
      throw;
   }
   close_completed = true;
}

boost::asio::awaitable<void> autonat_v2_dialback::run(std::shared_ptr<worker_terminal_owner> terminal) {
   // The bridge invokes this on its worker strand and seals the callback only
   // after work returns. The timer's finish barrier retires its channel callback.
   static_cast<void>(terminal->publish(worker_terminal_owner::callback{
       [this] noexcept { channel.request_cancel(); }}));
   const auto end = std::min(parent_deadline, std::chrono::steady_clock::now() + std::chrono::seconds{5});
   auto deadline = operation_deadline{context, std::max(std::chrono::milliseconds{1},
       std::chrono::ceil<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()))};
   deadline.arm([this] noexcept { channel.request_cancel(); });
   auto failure = std::exception_ptr{};
   auto cleanup_failure = std::exception_ptr{};
   auto io_expired = false;
   auto close_expired = false;
   try {
      if (terminal->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "AutoNAT v2 dial-back canceled before write");
      }
      if (std::chrono::steady_clock::now() >= parent_deadline) {
         throw_operation_timeout("AutoNAT v2 dial-back parent");
      }
      co_await channel.async_write(reachability::codec::encode_v2_dial_back({.nonce = nonce}));
      nonce_written = true;
      // Go 9cfe2cc server.go dialBack reads only to give the nonce time to arrive.
      // A decoded DialBackResponse is not part of DialStatus_OK. One read avoids
      // accumulating an optional frame or requiring its completion after RESET.
      static_cast<void>(co_await channel.async_read_chunk());
   } catch (...) {
      failure = std::current_exception();
      io_expired = deadline.timed_out() || std::chrono::steady_clock::now() >= end;
   }
   const auto cancel_cleanup = failure || deadline.timed_out() || terminal->stop_requested();
   if (cancel_cleanup) { channel.request_cancel(); }
   try { co_await close(); } catch (...) {
      cleanup_failure = std::current_exception();
      close_expired = deadline.timed_out() || std::chrono::steady_clock::now() >= end;
   }
   static_cast<void>(deadline.finish());
   // Do not turn a local resource/runtime failure into reachability evidence,
   // even if a remote reset or the optional read timeout happened concurrently.
   check_phase_failure(failure, io_expired);
   check_phase_failure(cleanup_failure, close_expired || cancel_cleanup);
}

boost::asio::awaitable<reachability::v2::dial_status> async_autonat_v2_dialback(
    stream channel, std::uint64_t nonce, boost::asio::io_context& context,
    std::chrono::steady_clock::time_point parent_deadline, std::shared_ptr<cancellation_latch> cancellation) {
   auto state = std::make_shared<autonat_v2_dialback>(std::move(channel), nonce, context, parent_deadline);
   auto parent = cancellation_latch::subscribe(cancellation, [stop = state->stop] noexcept { stop->request_stop(); });
   auto failure = std::exception_ptr{};
   try {
      co_await async_run_with_stop_bridge(state->stop, [state](std::shared_ptr<worker_terminal_owner> terminal) {
         return state->run(std::move(terminal));
      });
   } catch (...) {
      failure = std::current_exception();
   }
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   if (!state->close_completed) {
      state->channel.request_cancel();
      try { co_await state->close(); } catch (...) { if (!failure) { failure = std::current_exception(); } }
   }
   parent.reset();
   if (std::chrono::steady_clock::now() >= parent_deadline) { throw_operation_timeout("AutoNAT v2 dial-back parent"); }
   if (state->stop->stop_requested() || (cancellation && cancellation->stop_requested())) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "AutoNAT v2 dial-back canceled");
   }
   if (failure) { std::rethrow_exception(failure); }
   co_return state->nonce_written ? reachability::v2::dial_status::ok : reachability::v2::dial_status::dial_back_error;
}

} // namespace forge::net::p2p::detail
