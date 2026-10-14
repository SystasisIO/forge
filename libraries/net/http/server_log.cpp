#include "details/server_log.hxx"

#include <boost/asio/error.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/system/system_error.hpp>
#include <forge/log/macros.hpp>

#include <limits>
#include <system_error>
#include <utility>

import forge.asio.exceptions;
import forge.net.http.exceptions;
import forge.log.logger;

namespace forge::net::http::detail {
namespace {

constexpr auto summary_interval = std::chrono::seconds{5};

void increment(std::uint64_t& value) noexcept {
   if (value != (std::numeric_limits<std::uint64_t>::max)()) {
      ++value;
   }
}

} // namespace

server_log::error server_log::classify(const boost::system::error_code& failure) noexcept {
   if (!failure) {
      return {};
   }
   return {.code = failure.value(), .category = "boost",
           .expected = failure == boost::asio::error::operation_aborted ||
                       failure == boost::asio::error::eof ||
                       failure == boost::asio::error::connection_reset ||
                       failure == boost::asio::error::connection_aborted ||
                       failure == boost::asio::error::broken_pipe ||
                       failure == boost::asio::ssl::error::stream_truncated ||
                       failure == boost::beast::http::error::end_of_stream ||
                       failure == boost::beast::error::timeout || failure == boost::asio::error::timed_out ||
                       failure == boost::beast::websocket::error::closed,
           .client = failure.category() ==
                     boost::beast::http::make_error_code(boost::beast::http::error::bad_method).category() ||
                     failure == boost::beast::websocket::condition::handshake_failed ||
                     failure == boost::beast::websocket::condition::protocol_violation ||
                     failure == boost::beast::websocket::error::message_too_big,
           .tls = failure.category() == boost::asio::error::get_ssl_category()};
}

unsigned server_log::upgrade_rejection_status(const boost::system::error_code& failure) noexcept {
   namespace websocket = boost::beast::websocket;
   if (failure.category() != websocket::make_error_code(websocket::error::no_sec_key).category()) {
      return 0;
   }
   // Beast returns these server-validation errors only after its refusal write succeeds.
   // Transport/write failures take precedence. Do not infer status for other errors.
   switch (static_cast<websocket::error>(failure.value())) {
   case websocket::error::bad_http_version:
   case websocket::error::bad_method:
   case websocket::error::no_host:
   case websocket::error::no_connection:
   case websocket::error::no_connection_upgrade:
   case websocket::error::no_upgrade:
   case websocket::error::no_upgrade_websocket:
   case websocket::error::no_sec_key:
   case websocket::error::bad_sec_key:
   case websocket::error::no_sec_version: return 400;
   case websocket::error::bad_sec_version: return 426;
   default: return 0;
   }
}

server_log::error server_log::classify(std::exception_ptr failure) noexcept {
   if (!failure) {
      return {};
   }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::asio::exceptions::canceled& value) {
      return {.code = value.code().value(), .category = "forge", .expected = true};
   } catch (const forge::exceptions::base& value) {
      const auto code = value.code();
      const auto client = code.category() == exceptions::make_error_code(exceptions::code::bad_request).category() &&
                          code.value() >= 400 && code.value() < 500;
      return {.code = code.value(), .category = "forge", .client = client};
   } catch (const boost::system::system_error& value) {
      return classify(value.code());
   } catch (const std::system_error& value) {
      const auto code = value.code();
      return {.code = code.value(), .category = "std",
              .expected = code == std::errc::operation_canceled || code == std::errc::connection_reset ||
                          code == std::errc::broken_pipe || code == std::errc::connection_aborted};
   } catch (...) {
      return {.code = -1, .category = "unknown"};
   }
}

void server_log::emit(event value, std::exception_ptr failure, std::uint16_t port, bool tls) noexcept {
   try {
      const auto name = [value] {
         switch (value) {
         case event::listening: return "http.listening";
         case event::drained: return "http.drained";
         case event::runtime_stopped: return "http.closed_after_runtime_stop";
         case event::start_failed: return "http.start.failed";
         case event::accept_failed: return "http.accept.failed";
         case event::drain_failed: return "http.drain.failed";
         case event::session_failed: return "http.session.failed";
         case event::handler_failed: return "http.handler.failed";
         }
         return "http.unknown";
      }();
      if (!failure) {
         if (value == event::listening) {
            ilog("forge.net.http.server", "${event}", ("event", name)("port", port)("tls", tls));
         } else {
            ilog("forge.net.http.server", "${event}", ("event", name));
         }
         return;
      }
      const auto error = classify(failure);
      if (error.expected || error.client) {
         dlog("forge.net.http.server", "${event}",
              ("event", name)("error_code", error.code)("error_category", error.category));
      } else {
         elog("forge.net.http.server", "${event}",
              ("event", name)("error_code", error.code)("error_category", error.category));
      }
   } catch (...) {
      // Logging must neither veto publication nor interrupt cleanup.
   }
}

void server_log::request::begin(bool value) noexcept {
   *this = request{};
   enabled = value;
   if (enabled) {
      started = std::chrono::steady_clock::now();
   }
}

void server_log::request::finish(std::exception_ptr error_value) noexcept {
   if (!active) {
      return;
   }
   active = false;
   if (!enabled) {
      return;
   }
   try {
      if (error_value) {
         failure = server_log::classify(error_value);
      }
      const auto completed = sent && !aborted && !error_value;
      const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
         std::chrono::steady_clock::now() - started).count();
      dlog("forge.net.http.server", "HTTP request ${outcome}",
           ("event", "http.request.finished")("method", method)("status", status)
           ("duration_us", duration)("received_bytes", received_bytes)("sent_bytes", sent_bytes)
           ("sent_bytes_known", sent_bytes_known)
           ("response_chunks", response_chunks)("outcome", completed ? "completed" : "aborted")
           ("error_code", failure.code)("error_category", failure.category));
   } catch (...) {
   }
}

void server_log::reject(rejection value) noexcept {
   try {
      {
         const auto lock = std::scoped_lock{_mutex};
         switch (value) {
         case rejection::tls_limit: increment(_pending.tls_rejected); break;
         case rejection::tls_context: increment(_pending.tls_context_missing); break;
         case rejection::header_limit: increment(_pending.header_rejected); break;
         case rejection::body_limit: increment(_pending.body_rejected); break;
         }
      }
      flush(false);
   } catch (...) {
   }
}

void server_log::handshake_failed(const boost::system::error_code& failure) noexcept {
   record_tls_failure(classify(failure));
}

void server_log::handshake_failed(std::exception_ptr failure) noexcept {
   record_tls_failure(classify(std::move(failure)));
}

void server_log::session_failed(std::exception_ptr failure) noexcept {
   const auto error = classify(failure);
   if (!error.tls || error.expected) {
      emit(event::session_failed, std::move(failure));
      return;
   }
   try {
      {
         const auto lock = std::scoped_lock{_mutex};
         increment(_pending.tls_session_failed);
         _pending.last_session_error = error;
      }
      flush(false);
   } catch (...) {
   }
}

void server_log::record_tls_failure(error failure) noexcept {
   try {
      {
         const auto lock = std::scoped_lock{_mutex};
         increment(failure.expected ? _pending.tls_closed : _pending.tls_failed);
         _pending.last_tls_error = failure;
      }
      flush(false);
   } catch (...) {
   }
}

void server_log::flush() noexcept {
   flush(true);
}

void server_log::flush(bool force) noexcept {
   try {
      auto values = summary{};
      {
         const auto lock = std::scoped_lock{_mutex};
         const auto now = std::chrono::steady_clock::now();
         if (!force && now < _next_summary) {
            return;
         }
         if (_pending.tls_failed == 0 && _pending.tls_closed == 0 &&
             _pending.tls_session_failed == 0 && _pending.tls_rejected == 0 &&
             _pending.tls_context_missing == 0 && _pending.header_rejected == 0 && _pending.body_rejected == 0) {
            return;
         }
         values = std::exchange(_pending, summary{});
         _next_summary = now + summary_interval;
      }
      // Fixed counters, no peer/category-key map, and no sink calls under the mutex.
      if (values.tls_failed || values.tls_session_failed || values.tls_context_missing) {
         wlog("forge.net.http.server", "HTTP admission summary",
              ("event", "http.admission.summary")("tls_failed", values.tls_failed)
              ("tls_closed", values.tls_closed)("tls_rejected", values.tls_rejected)
              ("tls_session_failed", values.tls_session_failed)
              ("session_error_code", values.last_session_error.code)
              ("session_error_category", values.last_session_error.category)
              ("tls_context_missing", values.tls_context_missing)("header_rejected", values.header_rejected)
              ("body_rejected", values.body_rejected)("error_code", values.last_tls_error.code)
              ("error_category", values.last_tls_error.category));
      } else {
         dlog("forge.net.http.server", "HTTP admission summary",
              ("event", "http.admission.summary")("tls_failed", values.tls_failed)
              ("tls_closed", values.tls_closed)("tls_rejected", values.tls_rejected)
              ("tls_session_failed", values.tls_session_failed)
              ("session_error_code", values.last_session_error.code)
              ("session_error_category", values.last_session_error.category)
              ("tls_context_missing", values.tls_context_missing)("header_rejected", values.header_rejected)
              ("body_rejected", values.body_rejected)("error_code", values.last_tls_error.code)
              ("error_category", values.last_tls_error.category));
      }
   } catch (...) {
   }
}

} // namespace forge::net::http::detail
