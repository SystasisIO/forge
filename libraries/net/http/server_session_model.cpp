module;

#include <coroutine>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "details/router_server_access.hxx"
#include "details/stream_server_access.hxx"
#include "details/server_log.hxx"
#include "details/request_read_ownership.hxx"

#include <forge/exceptions/macros.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/scope/scope_exit.hpp>
#include <openssl/ssl.h>

module forge.net.http.server;

import forge.asio.exceptions;
import forge.asio.runtime;
import forge.crypto.digest.sha256;
import forge.net.http.body;
import forge.net.http.exceptions;
import forge.net.http.negotiation;
import forge.net.http.route_context;
import forge.net.http.stream;
import forge.net.tls.context;
import forge.net.websocket.connection;

#include "details/server_session.hxx"

#include "details/server_session_model.hxx"

extern "C++" {
namespace forge::net::http::detail {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
namespace beast_websocket = boost::beast::websocket;
using tcp = asio::ip::tcp;
using asio::awaitable;
using asio::use_awaitable;

bool same_header_name(std::string_view left, std::string_view right) noexcept {
   if (left.size() != right.size()) {
      return false;
   }
   for (auto index = std::size_t{0}; index != left.size(); ++index) {
      if (std::tolower(static_cast<unsigned char>(left[index])) !=
          std::tolower(static_cast<unsigned char>(right[index]))) {
         return false;
      }
   }
   return true;
}

stream_limits limits_from(const server_config& config) {
   return stream_limits{
       .max_body_bytes = config.max_request_body_bytes,
       .read_timeout = config.read_timeout,
       .write_timeout = config.idle_timeout,
   };
}

method to_http_method(beast_http::verb value) noexcept {
   switch (value) {
   case beast_http::verb::delete_:
      return method::delete_;
   case beast_http::verb::get:
      return method::get;
   case beast_http::verb::head:
      return method::head;
   case beast_http::verb::options:
      return method::options;
   case beast_http::verb::patch:
      return method::patch;
   case beast_http::verb::post:
      return method::post;
   case beast_http::verb::put:
      return method::put;
   default:
      return method::unknown;
   }
}

beast_http::status to_beast_status(status value) noexcept {
   return static_cast<beast_http::status>(static_cast<unsigned>(value));
}

request make_header_request(const beast_http::request_parser<beast_http::buffer_body>& parser) {
   const auto& source = parser.get();
   auto request_value = request{};
   request_value.method(to_http_method(source.method()));
   request_value.target(std::string_view{source.target().data(), source.target().size()});
   request_value.version(source.version());
   request_value.keep_alive(source.keep_alive());
   for (const auto& field_value : source) {
      request_value.insert(field_value.name_string(), field_value.value());
   }
   return request_value;
}

void copy_headers(const response& source, beast_http::response<beast_http::buffer_body>& target) {
   for (const auto& field_value : source.headers()) {
      if (same_header_name(field_value.name, "Set-Cookie")) {
         target.insert(field_value.name, field_value.text);
      } else {
         target.set(field_value.name, field_value.text);
      }
   }
}

beast_http::response<beast_http::string_body> to_beast_response(const response& source) {
   auto target = beast_http::response<beast_http::string_body>{to_beast_status(source.result()), source.version()};
   for (const auto& header : source.headers()) {
      target.insert(header.name, header.text);
   }
   target.keep_alive(source.keep_alive());
   target.body() = source.body();
   return target;
}

bool expects_continue(const request& value) {
   const auto found = value.find(field::expect);
   return found != value.end() && normalize_token(found->value()) == "100-continue";
}

beast_http::request<beast_http::string_body>
to_websocket_request(const beast_http::request<beast_http::buffer_body>& source) {
   auto target = beast_http::request<beast_http::string_body>{source.method(), source.target(), source.version()};
   for (const auto& header : source) {
      target.insert(header.name_string(), header.value());
   }
   target.keep_alive(source.keep_alive());
   return target;
}

std::size_t request_buffer_limit(const server_config& config) noexcept {
   const auto size_limit = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
   const auto body_limit = std::min(config.max_request_body_bytes, size_limit);
   const auto header_limit = std::min(config.max_header_bytes, size_limit - body_limit);
   return static_cast<std::size_t>(std::max<std::uint64_t>(1, body_limit + header_limit));
}

template <typename Stream> beast::tcp_stream& lowest_stream(Stream& stream) {
   return beast::get_lowest_layer(stream);
}

template <typename Stream> tcp::socket& stream_socket(Stream& stream) {
   return lowest_stream(stream).socket();
}

template <typename Stream> void expire_stream(Stream& stream, std::chrono::milliseconds timeout) {
   lowest_stream(stream).expires_after(timeout);
}

template <typename Stream> void disarm_stream_expiry(Stream& stream) {
   lowest_stream(stream).expires_never();
}

template <typename Stream>
[[nodiscard]] bool received_clean_peer_close(Stream& stream, const boost::system::error_code& error) {
   if (error == asio::error::eof) {
      return true;
   }
   if (error != beast_http::error::end_of_stream) {
      return false;
   }
   if constexpr (std::same_as<Stream, forge::net::tls::beast_tls_stream>) {
      return (SSL_get_shutdown(stream.native_handle()) & SSL_RECEIVED_SHUTDOWN) != 0;
   }
   return true;
}

constexpr auto tls_close_notify_timeout = std::chrono::seconds{5};

template <typename Stream> awaitable<void> run_tls_shutdown(Stream& stream) {
   auto [error] = co_await stream.async_shutdown(asio::as_tuple(use_awaitable));
   static_cast<void>(error);
}

template <typename Stream>
awaitable<void> cancel_tls_shutdown_at_deadline(Stream& stream, asio::steady_timer& deadline) {
   auto [error] = co_await deadline.async_wait(asio::as_tuple(use_awaitable));
   if (!error) {
      auto ignored = boost::system::error_code{};
      stream_socket(stream).cancel(ignored);
   }
}

template <typename Stream> awaitable<void> close_after_response(Stream& stream) {
   if constexpr (std::same_as<Stream, forge::net::tls::beast_tls_stream>) {
      auto deadline = asio::steady_timer{stream.get_executor()};
      deadline.expires_after(tls_close_notify_timeout);
      using namespace asio::experimental::awaitable_operators;
      static_cast<void>(co_await (run_tls_shutdown(stream) || cancel_tls_shutdown_at_deadline(stream, deadline)));

      auto ignored = boost::system::error_code{};
      stream_socket(stream).shutdown(tcp::socket::shutdown_both, ignored);
      stream_socket(stream).close(ignored);
      co_return;
   }

   auto ignored = boost::system::error_code{};
   stream_socket(stream).shutdown(tcp::socket::shutdown_send, ignored);
}

template <typename Stream>
template <typename Result>
server_session::model<Stream>::owner_handler_state<Result>::owner_handler_state(asio::any_io_executor executor, std::shared_ptr<request_read_ownership> ownership)
    : owner_completion(executor, (std::chrono::steady_clock::time_point::max)()),
      monitor_completion(std::move(executor), (std::chrono::steady_clock::time_point::max)()),
      read_ownership(std::move(ownership)) {}

template <typename Stream>
template <typename Result>
void server_session::model<Stream>::owner_handler_state<Result>::complete_owner(std::exception_ptr error_value, Result response_value) noexcept {
   try {
      owner_error = std::move(error_value);
      if (!owner_error) {
         owner_response.emplace(std::move(response_value));
      }
   } catch (...) {
      owner_error = std::current_exception();
      owner_response.reset();
   }
   owner_done = true;
   try {
      owner_completion.cancel();
   } catch (...) {
   }
}

template <typename Stream>
template <typename Result>
void server_session::model<Stream>::owner_handler_state<Result>::complete_monitor(std::exception_ptr error_value) noexcept {
   try {
      if (error_value && !owner_done) {
         mark_disconnected();
      }
   } catch (...) {
   }
   monitor_done = true;
   try {
      monitor_completion.cancel();
   } catch (...) {
   }
}

template <typename Stream>
template <typename Result>
void server_session::model<Stream>::owner_handler_state<Result>::mark_disconnected() {
   disconnected = true;
   read_ownership->mark_connection_closed();
   if (!owner_done) {
      owner_cancellation.emit(asio::cancellation_type::all);
   }
}

template <typename Stream>
server_session::model<Stream>::model(forge::asio::runtime& runtime, Stream stream, server_config config,
                                   server_handler handler, std::shared_ptr<router> router_value,
                                   std::shared_ptr<server_log> log,
                                   forge::net::tls::context_snapshot_ptr tls_snapshot)
    : runtime_{runtime}, stream_(std::move(stream)), executor_(stream_.get_executor()), config_(std::move(config)),
      buffer_(request_buffer_limit(config_)),
      run_completion_(stream_.get_executor(), (std::chrono::steady_clock::time_point::max)()),
      handler_(std::move(handler)), router_(std::move(router_value)), tls_snapshot_(std::move(tls_snapshot)),
      log_(std::move(log)) {}

template <typename Stream>
void server_session::model<Stream>::cancel() {
   auto self = this->shared_from_this();
   asio::dispatch(executor_, [self] { self->cancel_on_executor(); });
}

template <typename Stream>
void server_session::model<Stream>::cancel_after_runtime_stopped() {
   cancel_on_executor();
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::async_cancel() {
   auto self = this->shared_from_this();
   // dispatch() does not migrate the caller's associated executor. The
   // entire check/register/wake loop must share complete_run()'s strand.
   co_await asio::co_spawn(executor_, self->async_cancel_on_executor(), use_awaitable);
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::run() {
   auto self = this->shared_from_this();
   static_cast<void>(self);

   try {
      co_await run_loop();
   } catch (...) {
      const auto error = std::current_exception();
      access_.finish(error);
      const auto typed = server_log::classify(error);
      if (typed.client && typed.code == static_cast<int>(exceptions::code::payload_too_large)) {
         log_->reject(server_log::rejection::body_limit);
      }
      log_->session_failed(error);
      complete_run();
      throw;
   }
   access_.finish();
   complete_run();
}

template <typename Stream>
asio::any_io_executor server_session::model<Stream>::executor() noexcept {
   return executor_;
}

template <typename Stream>
asio::cancellation_slot server_session::model<Stream>::cancellation_slot() noexcept {
   return run_cancellation_.slot();
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::async_cancel_on_executor() {
   cancel_on_executor();
   while (!run_completed_) {
      auto [error] = co_await run_completion_.async_wait(asio::as_tuple(use_awaitable));
      if (error && error != asio::error::operation_aborted) {
         throw boost::system::system_error{error};
      }
   }
}

template <typename Stream>
void server_session::model<Stream>::complete_run() noexcept {
   run_completed_ = true;
   try {
      run_completion_.cancel();
   } catch (...) {
   }
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::run_loop() {
   auto first_request = true;
   for (;;) {
      access_.finish();
      if (stopping_) {
         co_return;
      }
      access_.begin(config_.access_log);
      auto parser = beast_http::request_parser<beast_http::buffer_body>{};
      parser.body_limit(config_.max_request_body_bytes);
      parser.header_limit(static_cast<std::uint32_t>(
          std::min<std::uint64_t>(config_.max_header_bytes, std::numeric_limits<std::uint32_t>::max())));
      expire_stream(stream_, first_request ? config_.read_timeout : config_.idle_timeout);
      auto [read_error, bytes] =
          co_await beast_http::async_read_header(stream_, buffer_, parser, asio::as_tuple(use_awaitable));
      access_.received_bytes += bytes;
      access_.active = bytes != 0U || parser.got_some() || buffer_.size() != 0U;

      if (received_clean_peer_close(stream_, read_error)) {
         if constexpr (std::same_as<Stream, forge::net::tls::beast_tls_stream>) {
            co_await close_after_response(stream_);
         } else if (!first_request) {
            co_await close_after_response(stream_);
         }
         co_return;
      }
      if (read_error == beast_http::error::header_limit) {
         access_.active = true;
         log_->reject(server_log::rejection::header_limit);
         auto response_value = response{status::request_header_fields_too_large, 11};
         response_value.set(field::content_type, "text/plain");
         response_value.body() = "headers too large";
         response_value.prepare_payload();
         response_value.keep_alive(false);
         co_await write_response(response_value);
         break;
      }
      if (read_error == beast_http::error::body_limit) {
         access_.active = true;
         log_->reject(server_log::rejection::body_limit);
         auto response_value = response{status::payload_too_large, 11};
         response_value.set(field::content_type, "text/plain");
         response_value.body() = "payload too large";
         response_value.prepare_payload();
         response_value.keep_alive(false);
         co_await write_response(response_value);
         break;
      }
      if (read_error) {
         throw boost::system::system_error{read_error};
      }
      first_request = false;

      access_.active = true;
      access_.method = static_cast<unsigned>(to_http_method(parser.get().method()));
      auto request_value = make_header_request(parser);
      auto context_storage = std::optional<route_context>{};
      auto invalid_target = false;
      try {
         context_storage.emplace(make_context(request_value));
      } catch (...) {
         invalid_target = true;
      }
      if (invalid_target) {
         auto response_value = make_text_response(request_value, status::bad_request, "bad request");
         response_value.version(request_value.version());
         response_value.keep_alive(false);
         co_await write_response(response_value);
         break;
      }
      auto& context = *context_storage;
      if (beast_websocket::is_upgrade(parser.get())) {
         if (co_await try_upgrade(parser.get(), context)) {
            co_return;
         }
      }

      if (router_) {
         auto preflight_response = response{};
         if (detail::router_server_access::reject_without_body(*router_, context, preflight_response)) {
            preflight_response.version(request_value.version());
            preflight_response.keep_alive(request_value.keep_alive() && parser.is_done());
            co_await write_response(preflight_response);
            if (!preflight_response.keep_alive()) {
               break;
            }
            continue;
         }
      }

      const auto stream_capable = router_ && router_->can_handle_stream(context);
      auto read_ownership = stream_capable ? std::make_shared<request_read_ownership>(stream_.get_executor())
                                           : std::shared_ptr<request_read_ownership>{};
      auto body_source = std::make_shared<beast_body_reader_source<Stream>>(
          stream_, buffer_, parser, limits_from(config_), expects_continue(request_value), read_ownership, &access_);
      auto request_body_marker = std::make_shared<int>(0);
      auto request_body =
          detail::stream_server_access::mark_request_body(body_reader{body_source}, request_body_marker);
      if (stream_capable) {
         auto stream_request_value =
             detail::stream_server_access::make_request(context, std::move(request_body), request_body_marker);
         expire_stream(stream_, config_.idle_timeout);
         auto keep_alive = co_await handle_owned_operation(
             handle_and_write_stream(std::move(stream_request_value), body_source, request_body_marker,
                                     read_ownership, request_value.version(), request_value.keep_alive()),
             read_ownership);
         if (!keep_alive.has_value()) {
            access_.aborted = true;
            co_return;
         }
         if (!*keep_alive) {
            break;
         }
         continue;
      }

      auto body_error_response = std::optional<response>{};
      try {
         request_value.body() = co_await request_body.async_read_all();
      } catch (const exceptions::payload_too_large&) {
         access_.failure = server_log::classify(std::current_exception());
         log_->reject(server_log::rejection::body_limit);
         auto response_value = make_text_response(request_value, status::payload_too_large, "payload too large");
         response_value.version(request_value.version());
         response_value.keep_alive(false);
         body_error_response = std::move(response_value);
      }
      if (body_error_response.has_value()) {
         co_await write_response(*body_error_response);
         break;
      }
      request_value.prepare_payload();
      context_storage.emplace(make_context(request_value));
      auto& buffered_context = *context_storage;

      expire_stream(stream_, config_.idle_timeout);
      read_ownership = std::make_shared<request_read_ownership>(stream_.get_executor());
      auto response_value = co_await handle_owned_operation(handle_http(buffered_context), read_ownership);
      if (!response_value.has_value()) {
         access_.aborted = true;
         co_return;
      }
      response_value->version(request_value.version());
      response_value->keep_alive(request_value.keep_alive() &&
                                 read_ownership->can_read_next_request(buffer_.size() != 0U));

      co_await write_response(*response_value);
      if (!response_value->keep_alive()) {
         break;
      }
   }

   access_.finish();
   co_await close_after_response(stream_);
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::wait_for_completion(asio::steady_timer& completion, const bool& done) {
   while (!done) {
      auto [error] = co_await completion.async_wait(asio::as_tuple(use_awaitable));
      if (error && error != asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP owner completion wait failed",
                               forge::exceptions::ctx("reason", error.message()));
      }
   }
}

template <typename Stream>
template <typename Result>
awaitable<void> server_session::model<Stream>::monitor_owner_disconnect(const std::shared_ptr<owner_handler_state<Result>>& state) {
   for (;;) {
      if (!(co_await state->read_ownership->begin_monitor_read())) {
         co_return;
      }

      auto read_error = boost::system::error_code{};
      auto bytes = std::size_t{0};
      auto buffer_exhausted = false;
      {
         auto read_scope = monitor_read_scope{state->read_ownership};
         const auto remaining = buffer_.max_size() - buffer_.size();
         if (remaining == 0U) {
            buffer_exhausted = true;
         } else {
            const auto read_size = std::min<std::size_t>(remaining, 64U * 1024U);
            auto destination = buffer_.prepare(read_size);
            auto [operation_error, transferred] = co_await stream_.async_read_some(
                destination, asio::bind_cancellation_slot(state->read_ownership->monitor_cancellation_slot(),
                                                          asio::as_tuple(use_awaitable)));
            read_error = operation_error;
            bytes = transferred;
            buffer_.commit(bytes);
         }
      }

      if (buffer_exhausted) {
         while (buffer_.size() == buffer_.max_size()) {
            if (!(co_await state->read_ownership->wait_for_monitor_retry())) {
               co_return;
            }
         }
         continue;
      }
      if (read_error == asio::error::operation_aborted) {
         if (state->owner_done || state->read_ownership->monitor_stopping()) {
            co_return;
         }
         if (state->read_ownership->body_read_requested()) {
            continue;
         }
      } else if (read_error == asio::error::eof) {
         state->read_ownership->mark_peer_read_closed();
         co_return;
      } else if (!read_error && bytes != 0U) {
         continue;
      }
      state->mark_disconnected();
      co_return;
   }
}

template <typename Stream>
template <typename Result>
awaitable<std::optional<Result>> server_session::model<Stream>::handle_owned_operation(awaitable<Result> operation,
                                                        std::shared_ptr<request_read_ownership> read_ownership) {
   if (stopping_) {
      co_return std::nullopt;
   }
   auto state = std::make_shared<owner_handler_state<Result>>(stream_.get_executor(), std::move(read_ownership));
   owner_stop_ = [weak = std::weak_ptr{state}] {
      if (auto active = weak.lock()) {
         active->mark_disconnected();
      }
   };
   auto clear_owner_stop = boost::scope::scope_exit{[this] { owner_stop_ = {}; }};
   auto self = this->shared_from_this();
   auto error = std::exception_ptr{};
   auto monitor_started = false;
   auto owner_started = false;
   try {
      // The outer-stream monitor observes disconnects while user work runs; it must not inherit a socket I/O
      // deadline.
      disarm_stream_expiry(stream_);
      asio::co_spawn(stream_.get_executor(), monitor_owner_disconnect(state),
                     [self = std::move(self), state](std::exception_ptr error) mutable {
                        state->complete_monitor(std::move(error));
                     });
      monitor_started = true;
      asio::co_spawn(stream_.get_executor(), std::move(operation),
                     asio::bind_cancellation_slot(state->owner_cancellation.slot(),
                                                  [state](std::exception_ptr error, Result response_value) mutable {
                                                     state->complete_owner(std::move(error),
                                                                           std::move(response_value));
                                                  }));
      owner_started = true;

      co_await wait_for_completion(state->owner_completion, state->owner_done);
      state->read_ownership->stop_monitor();
      co_await wait_for_completion(state->monitor_completion, state->monitor_done);

   } catch (...) {
      error = std::current_exception();
   }
   if (error) {
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
      state->mark_disconnected();
      state->read_ownership->stop_monitor();
      if (owner_started) {
         co_await wait_for_completion(state->owner_completion, state->owner_done);
      }
      if (monitor_started) {
         co_await wait_for_completion(state->monitor_completion, state->monitor_done);
      }
   }
   if (error) {
      std::rethrow_exception(error);
   }
   if (state->disconnected) {
      co_return std::nullopt;
   }
   if (state->owner_error) {
      std::rethrow_exception(state->owner_error);
   }
   if (!state->owner_response.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP owner handler completed without a response");
   }
   co_return std::move(state->owner_response);
}

template <typename Stream>
awaitable<bool> server_session::model<Stream>::handle_and_write_stream(stream_request request_value,
                                        const std::shared_ptr<beast_body_reader_source<Stream>>& body_source,
                                        const std::shared_ptr<int>& request_body_marker,
                                        const std::shared_ptr<request_read_ownership>& read_ownership,
                                        unsigned version, bool request_keep_alive) {
   auto response_value = co_await router_->handle_stream(request_value);
   const auto request_body_deferred_to_response =
       detail::stream_server_access::response_body_uses_request(response_value, request_body_marker) &&
       !body_source->done();
   response_value.head.version(version);
   response_value.head.keep_alive(request_keep_alive && body_source->done() &&
                                  read_ownership->can_read_next_request(buffer_.size() != 0U));
   if (request_body_deferred_to_response) {
      co_await body_source->send_continue_if_needed();
   }
   co_await write_stream_response(response_value);
   co_return response_value.head.keep_alive();
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::write_response(response& response_value) {
   access_.status = response_value.result_int();
   auto beast_response = to_beast_response(response_value);
   expire_stream(stream_, config_.idle_timeout);
   auto [write_error, written] =
       co_await beast_http::async_write(stream_, beast_response, asio::as_tuple(use_awaitable));
   access_.sent_bytes += written;
   if (write_error) {
      throw boost::system::system_error{write_error};
   }
   disarm_stream_expiry(stream_);
   access_.sent = true;
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::write_stream_response(stream_response& response_value) {
   auto operation = send_stream_response(response_value);
   if (response_value.send_guard) {
      co_await response_value.send_guard(std::move(operation), [self = this->shared_from_this()] {
         self->close_http_socket();
      });
   } else {
      co_await std::move(operation);
   }
}

template <typename Stream>
awaitable<void> server_session::model<Stream>::send_stream_response(stream_response& response_value) {
   access_.status = response_value.head.result_int();
   if (!response_value.body) {
      co_await write_response(response_value.head);
      co_return;
   }

   auto message = beast_http::response<beast_http::buffer_body>{to_beast_status(response_value.head.result()),
                                                                response_value.head.version()};
   copy_headers(response_value.head, message);
   message.keep_alive(response_value.head.keep_alive());
   if (message.find(field_name(field::content_length)) == message.end()) {
      message.chunked(true);
   }

   auto serializer = beast_http::response_serializer<beast_http::buffer_body>{message};
   serializer.split(true);
   expire_stream(stream_, config_.idle_timeout);
   auto [header_error, header_bytes] =
       co_await beast_http::async_write_header(stream_, serializer, asio::as_tuple(use_awaitable));
   access_.sent_bytes += header_bytes;
   if (header_error) {
      throw boost::system::system_error{header_error};
   }
   disarm_stream_expiry(stream_);

   while (auto chunk = co_await response_value.body()) {
      auto& body = message.body();
      body.data = chunk->bytes.data();
      body.size = chunk->bytes.size();
      body.more = true;

      expire_stream(stream_, config_.idle_timeout);
      auto [body_error, body_bytes] =
          co_await beast_http::async_write(stream_, serializer, asio::as_tuple(use_awaitable));
      access_.sent_bytes += body_bytes;
      if (body_error && body_error != beast_http::error::need_buffer) {
         throw boost::system::system_error{body_error};
      }
      disarm_stream_expiry(stream_);
      ++access_.response_chunks;
   }

   auto& body = message.body();
   body.data = nullptr;
   body.size = 0;
   body.more = false;
   expire_stream(stream_, config_.idle_timeout);
   auto [final_error, final_bytes] =
       co_await beast_http::async_write(stream_, serializer, asio::as_tuple(use_awaitable));
   access_.sent_bytes += final_bytes;
   if (final_error && final_error != beast_http::error::need_buffer) {
      throw boost::system::system_error{final_error};
   }
   disarm_stream_expiry(stream_);
   if (!serializer.is_done()) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP streaming response send did not complete");
   }
   access_.sent = true;
}

template <typename Stream>
route_context server_session::model<Stream>::make_context(const request& request_value) {
   try {
      auto context = make_route_context(request_value);
      context.runtime = &runtime_;
      if constexpr (std::same_as<Stream, forge::net::tls::beast_tls_stream>) {
         if (tls_snapshot_ &&
             tls_snapshot_->verification() == forge::net::tls::peer_verification::require_peer_certificate) {
            const auto certificate = forge::net::tls::extract_peer_certificate(stream_.native_handle());
            if (certificate) {
               context.client_certificate_fingerprint =
                   forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{certificate->der});
            }
         }
      }
      return context;
   } catch (const exceptions::bad_request&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "invalid HTTP request target",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "invalid HTTP request target");
   }
}

template <typename Stream>
awaitable<response> server_session::model<Stream>::handle_http(route_context& context) {
   try {
      if (router_) {
         co_return co_await router_->handle(context);
      }
      co_return co_await handler_(context);
   } catch (const exceptions::bad_request&) {
      access_.failure = server_log::classify(std::current_exception());
      co_return make_text_response(context.request, status::bad_request, "bad request");
   } catch (...) {
      const auto error = std::current_exception();
      access_.failure = server_log::classify(error);
      server_log::emit(server_log::event::handler_failed, error);
      co_return make_text_response(context.request, status::internal_server_error, "internal server error");
   }
}

template <typename Stream>
awaitable<bool> server_session::model<Stream>::try_upgrade(const beast_http::request<beast_http::buffer_body>& request_value,
                            route_context& context) {
   if (!router_) {
      co_return false;
   }

   auto handler = router_->match_websocket(context);
   auto session = websocket_session_handler{};
   if (auto guard = router_->match_websocket_guarded(context)) {
      auto result = websocket_upgrade_result{};
      try {
         auto ownership = std::make_shared<request_read_ownership>(stream_.get_executor());
         auto admitted = co_await handle_owned_operation((*guard)(context), std::move(ownership));
         if (!admitted || stopping_) {
            access_.aborted = true;
            co_return true;
         }
         result = std::move(*admitted);
      } catch (...) {
         const auto error = std::current_exception();
         access_.failure = server_log::classify(error);
         server_log::emit(server_log::event::handler_failed, error);
         result.rejection =
             make_text_response(context.request, status::service_unavailable, "WebSocket admission failed");
      }
      if (stopping_) {
         access_.aborted = true;
         co_return true;
      }
      if (result.rejection || (!result.handler && !result.session) || (result.handler && result.session)) {
         auto rejected = result.rejection ? std::move(*result.rejection)
                                           : make_text_response(context.request, status::forbidden,
                                                                "WebSocket admission denied");
         rejected.version(context.request.version());
         rejected.keep_alive(false);
         rejected.prepare_payload();
         co_await write_response(rejected);
         co_return true;
      }
      handler = std::move(result.handler);
      session = std::move(result.session);
   }
   if (!session && (!handler.has_value() || !*handler)) {
      co_return false;
   }

   auto connection = forge::net::websocket::connection::ptr{};
   if constexpr (std::same_as<Stream, forge::net::tls::beast_tls_stream>) {
      connection = forge::net::websocket::connection::create(std::move(stream_), std::move(tls_snapshot_));
   } else {
      connection = forge::net::websocket::connection::create(std::move(stream_));
   }
   upgraded_ = true;
   auto websocket_request = to_websocket_request(request_value);
   auto buffered_input = beast::buffers_to_string(buffer_.data());
   buffer_.consume(buffer_.size());
   access_.sent_bytes_known = false;
   try {
      co_await connection->accept(websocket_request, std::move(buffered_input));
   } catch (const boost::system::system_error& error) {
      if (const auto status = server_log::upgrade_rejection_status(error.code()); status != 0) {
         access_.status = status;
         access_.sent = true;
         access_.failure = server_log::classify(error.code());
         access_.finish();
      }
      throw;
   }
   access_.status = 101;
   access_.sent = true;
   access_.finish();
   if (session) {
      connection->start_read_loop();
      auto error = std::exception_ptr{};
      try {
         co_await session(connection);
      } catch (...) {
         error = std::current_exception();
      }
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
      co_await connection->close();
      if (error) {
         std::rethrow_exception(error);
      }
   } else {
      (*handler)(connection);
      connection->start_read_loop();
   }
   co_return true;
}

template <typename Stream>
void server_session::model<Stream>::cancel_on_executor() {
   stopping_ = true;
   if (owner_stop_) {
      owner_stop_();
   }
   run_cancellation_.emit(asio::cancellation_type::all);
   if (!upgraded_) {
      close_http_socket();
   }
}

template <typename Stream>
void server_session::model<Stream>::close_http_socket() {
   auto ignored = boost::system::error_code{};
   stream_socket(stream_).cancel(ignored);
   stream_socket(stream_).shutdown(tcp::socket::shutdown_both, ignored);
   stream_socket(stream_).close(ignored);
}

template class server_session::model<beast::tcp_stream>;
template class server_session::model<forge::net::tls::beast_tls_stream>;

} // namespace forge::net::http::detail
}
