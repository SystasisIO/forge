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

#include "details/server_impl.hxx"

namespace forge::net::http {
namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using asio::awaitable;
using asio::use_awaitable;

server::impl::impl(forge::asio::runtime& runtime_value, server_config config_value, server_handler handler_value,
     std::shared_ptr<router> router_value)
    : runtime(runtime_value), config(std::move(config_value)), handler(std::move(handler_value)),
      router_value(std::move(router_value)), acceptor_executor(asio::make_strand(runtime.context())),
      acceptor(acceptor_executor),
      accept_loop_completion(acceptor_executor, (std::chrono::steady_clock::time_point::max)()),
      stop_completion(acceptor_executor, (std::chrono::steady_clock::time_point::max)()),
      tls_handshake_completion(acceptor_executor, (std::chrono::steady_clock::time_point::max)()),
      tls_enabled(static_cast<bool>(config.tls_context_provider)) {}

awaitable<void> server::impl::accept_loop() {
   for (;;) {
      auto session_strand = asio::make_strand(runtime.context());
      auto socket = tcp::socket{session_strand};
      auto [error] = co_await acceptor.async_accept(socket, asio::as_tuple(use_awaitable));
      if (error == asio::error::operation_aborted) {
         co_return;
      }
      if (error) {
         throw boost::system::system_error{error};
      }
      if (stopping) {
         auto ignored = boost::system::error_code{};
         socket.close(ignored);
         co_return;
      }

      if (!tls_enabled) {
         auto client = detail::server_session::create(
             runtime, beast::tcp_stream{std::move(socket)}, config, handler, router_value, log);
         remember_session(client);
         asio::co_spawn(session_strand, client->run(), asio::bind_cancellation_slot(
             client->cancellation_slot(), [client](std::exception_ptr error) {
            static_cast<void>(error);
         }));
         continue;
      }

      if (!reserve_tls_handshake()) {
         log->reject(detail::server_log::rejection::tls_limit);
         auto ignored = boost::system::error_code{};
         socket.close(ignored);
         continue;
      }

      // A connection owns precisely this snapshot through its HTTP or WebSocket lifetime.
      auto snapshot = config.tls_context_provider->snapshot();
      if (!snapshot) {
         log->reject(detail::server_log::rejection::tls_context);
         release_tls_handshake();
         auto ignored = boost::system::error_code{};
         socket.close(ignored);
         continue;
      }
      try {
         auto tls_stream = forge::net::tls::make_beast_stream(snapshot, beast::tcp_stream{std::move(socket)});
         remember_pending_tls_handshake(tls_stream);
         auto self = shared_from_this();
         asio::co_spawn(session_strand, complete_tls_handshake(tls_stream, std::move(snapshot)),
                        [self](std::exception_ptr error, std::shared_ptr<detail::server_session> session) {
                           asio::dispatch(self->acceptor_executor,
                                          [self, error = std::move(error), session = std::move(session)]() mutable {
                                             self->release_tls_handshake();
                                             if (session) {
                                                self->remember_session(session);
                                             }
                                             if (error) {
                                                self->log->handshake_failed(std::move(error));
                                             }
                                          });
                        });
      } catch (...) {
         release_tls_handshake();
         throw;
      }
   }
}

awaitable<std::shared_ptr<detail::server_session>>
server::impl::complete_tls_handshake(std::shared_ptr<forge::net::tls::beast_tls_stream> stream,
                       forge::net::tls::context_snapshot_ptr snapshot) {
   beast::get_lowest_layer(*stream).expires_after(config.handshake_timeout);
   auto [error] = co_await stream->async_handshake(asio::ssl::stream_base::server, asio::as_tuple(use_awaitable));
   if (error) {
      log->handshake_failed(error);
      auto ignored = boost::system::error_code{};
      beast::get_lowest_layer(*stream).socket().cancel(ignored);
      beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
      beast::get_lowest_layer(*stream).socket().close(ignored);
      co_return nullptr;
   }

   beast::get_lowest_layer(*stream).expires_never();
   auto client = detail::server_session::create(
       runtime, std::move(*stream), config, handler, router_value, log, std::move(snapshot));
   asio::co_spawn(client->executor(), client->run(), asio::bind_cancellation_slot(
       client->cancellation_slot(), [client](std::exception_ptr run_error) {
      static_cast<void>(run_error);
   }));
   co_return client;
}

void server::impl::prune_sessions() {
   sessions.erase(
       std::remove_if(sessions.begin(), sessions.end(),
                      [](const std::weak_ptr<detail::server_session>& session) { return session.expired(); }),
       sessions.end());
}

void server::impl::remember_session(const std::shared_ptr<detail::server_session>& session) {
   prune_sessions();
   sessions.push_back(session);
}

std::vector<std::shared_ptr<detail::server_session>> server::impl::active_sessions() {
   auto active = std::vector<std::shared_ptr<detail::server_session>>{};
   for (const auto& session : sessions) {
      if (auto locked = session.lock()) {
         active.push_back(std::move(locked));
      }
   }
   sessions.clear();
   sessions.reserve(active.size());
   for (const auto& session : active) {
      sessions.push_back(session);
   }
   return active;
}

bool server::impl::reserve_tls_handshake() {
   if (pending_tls_handshake_count >= config.max_pending_tls_handshakes) {
      return false;
   }
   ++pending_tls_handshake_count;
   return true;
}

void server::impl::remember_pending_tls_handshake(const std::shared_ptr<forge::net::tls::beast_tls_stream>& stream) {
   pending_tls_handshakes.erase(std::remove_if(pending_tls_handshakes.begin(), pending_tls_handshakes.end(),
                                               [](const std::weak_ptr<forge::net::tls::beast_tls_stream>& pending) {
                                                  return pending.expired();
                                               }),
                                pending_tls_handshakes.end());
   pending_tls_handshakes.push_back(stream);
}

void server::impl::release_tls_handshake() noexcept {
   if (pending_tls_handshake_count != 0U) {
      --pending_tls_handshake_count;
   }
   try {
      tls_handshake_completion.cancel();
   } catch (...) {
   }
}

void server::impl::cancel_pending_tls_handshakes() {
   for (const auto& pending : pending_tls_handshakes) {
      if (auto stream = pending.lock()) {
         asio::dispatch(stream->get_executor(), [stream = std::move(stream)] {
            auto ignored = boost::system::error_code{};
            beast::get_lowest_layer(*stream).socket().cancel(ignored);
            beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
            beast::get_lowest_layer(*stream).socket().close(ignored);
         });
      }
   }
}

void server::impl::cancel_pending_tls_handshakes_after_runtime_stopped() {
   for (const auto& pending : pending_tls_handshakes) {
      if (auto stream = pending.lock()) {
         auto ignored = boost::system::error_code{};
         beast::get_lowest_layer(*stream).socket().cancel(ignored);
         beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
         beast::get_lowest_layer(*stream).socket().close(ignored);
      }
   }
   pending_tls_handshakes.clear();
   pending_tls_handshake_count = 0;
}

awaitable<void> server::impl::wait_until_tls_handshakes_complete() {
   while (pending_tls_handshake_count != 0U) {
      auto [error] = co_await tls_handshake_completion.async_wait(asio::as_tuple(use_awaitable));
      if (error && error != asio::error::operation_aborted) {
         throw boost::system::system_error{error};
      }
   }
}

void server::impl::cancel_sessions_after_runtime_stopped() {
   for (auto& session : active_sessions()) {
      session->cancel_after_runtime_stopped();
   }
   sessions.clear();
}

void server::impl::begin_cancel_sessions() {
   for (auto& session : active_sessions()) {
      session->cancel();
   }
}

awaitable<void> server::impl::async_cancel_sessions() {
   auto active = active_sessions();
   for (auto& session : active) {
      session->cancel();
   }
   for (auto& session : active) {
      co_await session->async_cancel();
   }
   sessions.clear();
}

awaitable<void> server::impl::wait_until_stop_attempt_finished(std::shared_ptr<std::exception_ptr> pending) {
   while (stop_attempt == pending) {
      auto [error] = co_await stop_completion.async_wait(asio::as_tuple(use_awaitable));
      if (error && error != asio::error::operation_aborted) {
         throw boost::system::system_error{error};
      }
   }
   if (*pending) {
      std::rethrow_exception(*pending);
   }
}

awaitable<void> server::impl::wait_until_accept_loop_completed() {
   while (!accept_loop_completed) {
      auto [error] = co_await accept_loop_completion.async_wait(asio::as_tuple(use_awaitable));
      if (error && error != asio::error::operation_aborted) {
         throw boost::system::system_error{error};
      }
   }
}

void server::impl::complete_accept_loop() noexcept {
   accept_loop_completed = true;
   try {
      accept_loop_completion.cancel();
   } catch (...) {
   }
}

void server::impl::complete_stop() noexcept {
   started = false;
   stopping = false;
   stopped.store(true, std::memory_order_release);
   stop_attempt.reset();
   try {
      stop_completion.cancel();
   } catch (...) {
   }
}

void server::impl::start_on_executor() try {
   if (started) {
      return;
   }
   if (stopping) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP server cannot start while shutdown is in progress");
   }

   const auto address = asio::ip::make_address(config.bind_address);
   auto endpoint = tcp::endpoint{address, config.port};

   if (tls_enabled && config.handshake_timeout <= std::chrono::milliseconds::zero()) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP TLS handshake timeout must be positive");
   }
   if (tls_enabled && config.max_pending_tls_handshakes == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP TLS pending-handshake bound must be positive");
   }
   if (tls_enabled && !config.tls_context_provider->snapshot()) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP TLS context provider has no active snapshot");
   }

   acceptor.open(endpoint.protocol());
   acceptor.set_option(asio::socket_base::reuse_address(true));
   acceptor.bind(endpoint);
   acceptor.listen(asio::socket_base::max_listen_connections);
   accept_loop_completion.expires_at((std::chrono::steady_clock::time_point::max)());
   stop_completion.expires_at((std::chrono::steady_clock::time_point::max)());
   tls_handshake_completion.expires_at((std::chrono::steady_clock::time_point::max)());
   accept_loop_completed = false;
   started = true;
   stopped.store(false, std::memory_order_release);

   auto self = shared_from_this();
   auto operation = self->accept_loop();
   asio::co_spawn(acceptor_executor, std::move(operation), [self](std::exception_ptr error) {
      self->complete_accept_loop();
      if (error) {
         detail::server_log::emit(detail::server_log::event::accept_failed, error);
      }
   });
   auto endpoint_error = boost::system::error_code{};
   const auto listening_endpoint = acceptor.local_endpoint(endpoint_error);
   detail::server_log::emit(detail::server_log::event::listening, {},
                           endpoint_error ? 0 : listening_endpoint.port(), tls_enabled);
} catch (...) {
   detail::server_log::emit(detail::server_log::event::start_failed, std::current_exception());
   throw;
}

void server::impl::stop_on_executor() {
   if (stopped.load(std::memory_order_acquire) || stop_attempt) {
      return;
   }
   auto self = shared_from_this();
   auto operation = self->async_stop_on_executor();
   asio::co_spawn(acceptor_executor, std::move(operation), [self](std::exception_ptr error) {
      static_cast<void>(error);
   });
}

void server::impl::stop_after_runtime_stopped() {
   auto ignored = boost::system::error_code{};
   acceptor.cancel(ignored);
   acceptor.close(ignored);
   cancel_pending_tls_handshakes_after_runtime_stopped();
   cancel_sessions_after_runtime_stopped();
   complete_accept_loop();
   log->flush();
   complete_stop();
   detail::server_log::emit(detail::server_log::event::runtime_stopped);
}

awaitable<void> server::impl::async_stop_on_executor() {
   if (stopped.load(std::memory_order_acquire)) {
      co_return;
   }
   if (stop_attempt) {
      co_await wait_until_stop_attempt_finished(stop_attempt);
      co_return;
   }

   auto pending = std::make_shared<std::exception_ptr>();
   stop_attempt = pending;
   stopping = true;
   auto ignored = boost::system::error_code{};
   acceptor.cancel(ignored);
   acceptor.close(ignored);
   started = false;
   try {
      cancel_pending_tls_handshakes();
      begin_cancel_sessions();
      co_await wait_until_accept_loop_completed();
      co_await wait_until_tls_handshakes_complete();
      co_await async_cancel_sessions();
   } catch (...) {
      // Admission remains closed and sessions remain owned until a retry
      // actually drains them. Publish failure before releasing this attempt.
      *pending = std::current_exception();
      detail::server_log::emit(detail::server_log::event::drain_failed, *pending);
      stop_attempt.reset();
      try {
         stop_completion.cancel();
      } catch (...) {
      }
      throw;
   }
   log->flush();
   complete_stop();
   detail::server_log::emit(detail::server_log::event::drained);
}

} // namespace forge::net::http
