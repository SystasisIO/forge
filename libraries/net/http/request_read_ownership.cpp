#include "details/request_read_ownership.hxx"

#include <chrono>
#include <utility>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <forge/exceptions/macros.hpp>

import forge.asio.exceptions;
import forge.net.http.exceptions;

namespace forge::net::http::detail {
namespace asio = boost::asio;
using asio::awaitable;
using asio::use_awaitable;

request_read_ownership::request_read_ownership(asio::any_io_executor executor)
    : changed_(std::move(executor), (std::chrono::steady_clock::time_point::max)()) {}

awaitable<void> request_read_ownership::begin_body_read() {
   body_read_requested_ = true;
   notify();
   monitor_operation_cancellation_.emit(asio::cancellation_type::all);
   try {
      while (monitor_read_active_) {
         co_await wait_for_change();
      }
   } catch (...) {
      body_read_requested_ = false;
      notify();
      throw;
   }
   if (connection_closed_) {
      body_read_requested_ = false;
      notify();
      throw forge::asio::exceptions::canceled{"HTTP client disconnected while reading the request body"};
   }
   body_read_active_ = true;
   body_read_requested_ = false;
   notify();
}

void request_read_ownership::finish_body_read() noexcept {
   body_read_active_ = false;
   notify();
}

awaitable<bool> request_read_ownership::begin_monitor_read() {
   while (body_read_active_ || body_read_requested_) {
      if (monitor_stopping_) {
         co_return false;
      }
      co_await wait_for_change();
   }
   if (monitor_stopping_) {
      co_return false;
   }
   monitor_read_active_ = true;
   co_return true;
}

void request_read_ownership::finish_monitor_read() noexcept {
   monitor_read_active_ = false;
   notify();
}

void request_read_ownership::stop_monitor() {
   monitor_stopping_ = true;
   monitor_operation_cancellation_.emit(asio::cancellation_type::all);
   notify();
}

void request_read_ownership::mark_connection_closed() {
   connection_closed_ = true;
   stop_monitor();
}

void request_read_ownership::mark_peer_read_closed() {
   peer_read_closed_ = true;
   notify();
}

bool request_read_ownership::peer_read_closed() const noexcept {
   return peer_read_closed_;
}

bool request_read_ownership::can_read_next_request(bool has_buffered_input) const noexcept {
   return !peer_read_closed_ || has_buffered_input;
}

bool request_read_ownership::body_read_requested() const noexcept {
   return body_read_requested_;
}

bool request_read_ownership::monitor_stopping() const noexcept {
   return monitor_stopping_;
}

asio::cancellation_slot request_read_ownership::monitor_cancellation_slot() noexcept {
   return monitor_operation_cancellation_.slot();
}

awaitable<bool> request_read_ownership::wait_for_monitor_retry() {
   if (monitor_stopping_) {
      co_return false;
   }
   co_await wait_for_change();
   co_return !monitor_stopping_;
}

awaitable<void> request_read_ownership::wait_for_change() {
   auto [error] = co_await changed_.async_wait(asio::as_tuple(use_awaitable));
   if (error && error != asio::error::operation_aborted) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP request read ownership wait failed",
                            forge::exceptions::ctx("reason", error.message()));
   }
}

void request_read_ownership::notify() noexcept {
   try {
      changed_.cancel();
   } catch (...) {
   }
}

body_read_scope::body_read_scope(std::shared_ptr<request_read_ownership> ownership)
    : ownership_(std::move(ownership)) {}

body_read_scope::~body_read_scope() {
   if (ownership_) {
      ownership_->finish_body_read();
   }
}

monitor_read_scope::monitor_read_scope(std::shared_ptr<request_read_ownership> ownership)
    : ownership_(std::move(ownership)) {}

monitor_read_scope::~monitor_read_scope() {
   ownership_->finish_monitor_read();
}

} // namespace forge::net::http::detail
