module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <utility>

module forge.api.http.binding;

import forge.api.core.invocation_context;
import forge.net.http.body;
import forge.net.http.stream;

#include "details/invocation_body_source.hxx"

namespace forge::api::http::detail {

invocation_body_source::invocation_body_source(forge::net::http::stream_response::body_source body,
                                              forge::api::core::invocation_context context,
                                              std::function<void()> cancel)
    : body_{std::move(body)}, context_{std::move(context)}, cancel_{std::move(cancel)},
      next_recheck_{std::chrono::steady_clock::now() + context_.lifetime.recheck_interval} {}

boost::asio::awaitable<std::optional<forge::net::http::body_chunk>> invocation_body_source::async_read() {
   context_.lifetime.check();
   if (sending_) {
      auto chunk = co_await body_();
      context_.lifetime.check();
      co_return chunk;
   }
   auto current = context_;
   if (current.lifetime.recheck) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_recheck_) {
         auto bounded = current;
         bounded.lifetime.limit_for(current.lifetime.recheck_timeout);
         bounded.lifetime.recheck = {};
         bounded.lifetime.recheck_interval = {};
         bounded.lifetime.recheck_timeout = {};
         auto valid = false;
         auto failed = false;
         try {
            valid = co_await forge::api::core::enforce_invocation(
               current.lifetime.recheck(current.trusted), std::move(bounded),
               [cancel = cancel_](std::exception_ptr) { cancel(); });
         } catch (...) {
            failed = true;
         }
         const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
         if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
            cancel_();
            throw forge::api::core::exceptions::cancelled{"HTTP stream authority check was cancelled"};
         }
         if (failed || !valid) {
            cancel_();
            throw forge::api::core::exceptions::invocation_denied{"HTTP stream authority is no longer current"};
         }
         next_recheck_ = std::chrono::steady_clock::now() + current.lifetime.recheck_interval;
      }
      // Keep one connection-level schedule; fast chunks must not postpone revoke checks.
      current.lifetime.recheck_interval = std::max(
         std::chrono::milliseconds{1},
         std::chrono::ceil<std::chrono::milliseconds>(next_recheck_ - std::chrono::steady_clock::now()));
   }
   co_return co_await forge::api::core::enforce_invocation(
      body_(), std::move(current), [cancel = cancel_](std::exception_ptr) { cancel(); });
}

boost::asio::awaitable<void> invocation_body_source::async_send(boost::asio::awaitable<void> operation,
                                                               std::function<void()> abort) {
   sending_ = true;
   auto error = std::exception_ptr{};
   try {
      // One canonical guard covers producer waits and all socket writes, without per-chunk watchdogs.
      co_await forge::api::core::enforce_invocation(std::move(operation), context_,
         [cancel = cancel_, abort](std::exception_ptr) {
            abort();
            cancel();
         });
   } catch (...) {
      error = std::current_exception();
   }
   sending_ = false;
   if (error) {
      abort();
      cancel_();
      std::rethrow_exception(error);
   }
}

} // namespace forge::api::http::detail
