module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/ssl.h>
#include "details/handshake_deadline.hxx"
#include "details/connection_test_hooks.hxx"

module forge.net.stcp.connection;

import forge.asio.gate;
import forge.asio.notification;
import forge.net.tls.context;
import forge.net.tls.exceptions;
import forge.net.transport.stream;

#include "details/io_gates.hxx"

namespace forge::net::stcp::detail {

namespace {

[[noreturn]] void throw_io_error(std::string message, const boost::system::error_code& error) {
   FORGE_THROW_EXCEPTION(exceptions::io_error, std::move(message), forge::exceptions::ctx("reason", error.message()));
}

[[noreturn]] void throw_read_write_error(const boost::system::error_code& error) {
   if (error == boost::asio::error::operation_aborted) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "stcp connection operation canceled",
                            forge::exceptions::ctx("reason", error.message()));
   }
   if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset ||
       error == boost::asio::error::broken_pipe || error == boost::asio::ssl::error::stream_truncated) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "stcp connection closed",
                            forge::exceptions::ctx("reason", error.message()));
   }
   throw_io_error("stcp connection I/O failed", error);
}

} // namespace

io_gates::io_gates() = default;
io_gates::~io_gates() = default;

boost::asio::awaitable<forge::asio::gate::ticket> io_gates::acquire(forge::asio::gate& gate) {
   try {
      co_return co_await gate.acquire();
   } catch (const forge::asio::exceptions::canceled&) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "stcp operation canceled while waiting for I/O");
   } catch (const forge::asio::exceptions::rejected&) {
      throw_stopped();
   }
}

void io_gates::stop(io_stop_reason value) noexcept {
   auto expected = io_stop_reason::none;
   reason.compare_exchange_strong(expected, value, std::memory_order_release, std::memory_order_relaxed);
   read.close();
   write.close();
   terminal_requested.notify();
}

[[nodiscard]] bool io_gates::stopped() const noexcept {
   return reason.load(std::memory_order_acquire) != io_stop_reason::none;
}

[[noreturn]] void io_gates::throw_stopped() const {
   if (reason.load(std::memory_order_acquire) == io_stop_reason::canceled) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "stcp operation canceled while waiting for I/O");
   }
   FORGE_THROW_EXCEPTION(exceptions::closed, "stcp connection closed while waiting for I/O");
}

[[noreturn]] void terminalize_io_error(detail::stream_backend& stream, io_gates& gates,
                                       const boost::system::error_code& error) {
   if (gates.stopped()) {
      stream.request_cancel();
      gates.throw_stopped();
   }
   gates.stop(error == boost::asio::error::operation_aborted ? io_stop_reason::canceled : io_stop_reason::closed);
   stream.request_cancel();
   throw_read_write_error(error);
}

[[noreturn]] void terminalize_closed(detail::stream_backend* stream, io_gates& gates, std::string_view message) {
   gates.stop(io_stop_reason::closed);
   if (stream) {
      stream->request_cancel();
   }
   FORGE_THROW_EXCEPTION(exceptions::closed, std::string{message});
}

} // namespace forge::net::stcp::detail
