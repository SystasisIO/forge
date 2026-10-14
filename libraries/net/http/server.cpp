module;

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl.hpp>
#include <forge/exceptions/macros.hpp>

#include "details/server_log.hxx"
#include "details/server_wait_state.hxx"

module forge.net.http.server;

import forge.asio.runtime;
import forge.net.http.exceptions;
import forge.net.tls.context;

#include "details/server_impl.hxx"

namespace forge::net::http {
namespace asio = boost::asio;
using asio::use_awaitable;

server::server(forge::asio::runtime& runtime, server_config config, server_handler handler)
    : impl_(std::make_shared<impl>(runtime, std::move(config), std::move(handler), nullptr)) {}

server::server(forge::asio::runtime& runtime, server_config config, router router_value)
    : impl_(std::make_shared<impl>(runtime, std::move(config), server_handler{},
                                   std::make_shared<router>(std::move(router_value)))) {}

server::~server() {
   if (impl_ && !impl_->stopped.load(std::memory_order_acquire)) {
      stop();
   }
}

void server::start() {
   auto state = std::make_shared<detail::server_wait_state>();
   auto impl = impl_;
   if (impl->runtime.context().stopped()) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP server cannot start after runtime stop");
   }
   if (impl->acceptor_executor.running_in_this_thread()) {
      impl->start_on_executor();
      return;
   }
   if (impl->runtime.context().get_executor().running_in_this_thread()) {
      FORGE_THROW_EXCEPTION(exceptions::internal,
                            "HTTP server synchronous start cannot run on a runtime worker; use async_start()");
   }

   asio::dispatch(impl->acceptor_executor, [impl, state] {
      auto error = std::exception_ptr{};
      try {
         impl->start_on_executor();
      } catch (...) {
         error = std::current_exception();
      }
      {
         const auto lock = std::scoped_lock{state->mutex};
         state->error = std::move(error);
         state->done = true;
      }
      state->ready.notify_all();
   });

   auto lock = std::unique_lock{state->mutex};
   state->ready.wait(lock, [&] { return state->done; });
   if (state->error) {
      std::rethrow_exception(state->error);
   }
}

void server::stop() {
   if (!impl_) {
      return;
   }

   auto impl = impl_;
   if (impl->runtime.context().stopped()) {
      impl->stop_after_runtime_stopped();
      return;
   }
   if (impl->acceptor_executor.running_in_this_thread()) {
      impl->stop_on_executor();
      return;
   }
   if (impl->runtime.context().get_executor().running_in_this_thread()) {
      asio::post(impl->acceptor_executor, [impl] { impl->stop_on_executor(); });
      return;
   }

   auto state = std::make_shared<detail::server_wait_state>();
   auto operation = impl->async_stop_on_executor();
   asio::co_spawn(impl->acceptor_executor, std::move(operation), [impl, state](std::exception_ptr error) {
      {
         const auto lock = std::scoped_lock{state->mutex};
         state->error = std::move(error);
         state->done = true;
      }
      state->ready.notify_all();
   });

   auto lock = std::unique_lock{state->mutex};
   state->ready.wait(lock, [&] { return state->done; });
   if (state->error) {
      std::rethrow_exception(state->error);
   }
}

boost::asio::awaitable<void> server::async_start() {
   auto impl = impl_;
   co_await asio::dispatch(impl->acceptor_executor, use_awaitable);
   impl->start_on_executor();
}

boost::asio::awaitable<void> server::async_stop() {
   if (!impl_) {
      co_return;
   }
   auto impl = impl_;
   // Keep all attempt ownership/result publication on the acceptor strand,
   // including continuations after draining a session's different executor.
   co_await asio::co_spawn(impl->acceptor_executor, impl->async_stop_on_executor(), use_awaitable);
}

std::uint16_t server::port() const {
   if (!impl_->acceptor.is_open()) {
      return 0;
   }
   return impl_->acceptor.local_endpoint().port();
}

} // namespace forge::net::http
