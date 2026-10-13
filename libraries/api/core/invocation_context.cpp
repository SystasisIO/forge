module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <utility>

module forge.api.core.invocation_context;

namespace forge::api::core {
namespace {

std::chrono::steady_clock::time_point checked_deadline(
   std::chrono::steady_clock::time_point now, std::chrono::duration<long double> delay) {
   const auto capacity = std::chrono::duration<long double>{
      std::chrono::steady_clock::time_point::max().time_since_epoch()}.count() -
      std::chrono::duration<long double>{now.time_since_epoch()}.count();
   const auto maximum = std::chrono::duration<long double>{std::chrono::steady_clock::duration::max()}.count();
   if (!std::isfinite(delay.count()) || delay.count() <= 0 || delay.count() >= capacity || delay.count() >= maximum) {
      throw exceptions::protocol_error{"invocation lifetime duration is out of range"};
   }
   return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
}

boost::asio::awaitable<bool> wait_recheck_deadline(std::chrono::steady_clock::time_point deadline) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   timer.expires_at(deadline);
   co_await timer.async_wait(boost::asio::use_awaitable);
   co_return false;
}

boost::asio::awaitable<bool> safe_recheck(
   std::function<boost::asio::awaitable<bool>(trusted_invocation)> recheck, trusted_invocation trusted) {
   try {
      co_return co_await recheck(std::move(trusted));
   } catch (...) {
      // Fail closed without retaining or exposing backend diagnostic text.
      co_return false;
   }
}

} // namespace

bool invocation_lifetime::active() const noexcept {
   return expires_at.has_value() || expires_by_.has_value() || static_cast<bool>(recheck);
}

void invocation_lifetime::bind_expiry() {
   check();
   if (expires_at && (!bound_expiry_ || *expires_at < *bound_expiry_)) {
      const auto remaining = std::chrono::duration<long double>{expires_at->time_since_epoch()} -
                             std::chrono::duration<long double>{std::chrono::system_clock::now().time_since_epoch()};
      if (remaining.count() <= 0) {
         throw exceptions::invocation_denied{"invocation authority expired"};
      }
      const auto deadline = checked_deadline(std::chrono::steady_clock::now(), remaining);
      expires_by_ = expires_by_ ? std::min(*expires_by_, deadline) : deadline;
      bound_expiry_ = expires_at;
   }
}

void invocation_lifetime::limit_for(std::chrono::milliseconds duration) {
   const auto deadline = checked_deadline(std::chrono::steady_clock::now(), duration);
   expires_by_ = expires_by_ ? std::min(*expires_by_, deadline) : deadline;
}

void invocation_lifetime::check() const {
   if ((recheck && (recheck_interval.count() <= 0 || recheck_timeout.count() <= 0)) ||
       (!recheck && (recheck_interval.count() != 0 || recheck_timeout.count() != 0))) {
      throw exceptions::protocol_error{"invalid invocation lifetime policy"};
   }
   if (recheck) {
      const auto now = std::chrono::steady_clock::now();
      static_cast<void>(checked_deadline(now, recheck_interval));
      static_cast<void>(checked_deadline(now, recheck_timeout));
   }
   if ((expires_by_ && std::chrono::steady_clock::now() >= *expires_by_) ||
       (expires_at && std::chrono::system_clock::now() >= *expires_at)) {
      throw exceptions::invocation_denied{"invocation authority expired"};
   }
}

boost::asio::awaitable<std::exception_ptr> invocation_lifetime::async_wait(
   trusted_invocation trusted, std::function<void(std::exception_ptr)> on_denied) const {
   auto failure = std::exception_ptr{};
   try {
      auto bound = *this;
      bound.bind_expiry();
      if (!bound.active()) {
         throw exceptions::protocol_error{"inactive invocation lifetime cannot be watched"};
      }
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      for (;;) {
         auto deadline = bound.recheck ? checked_deadline(std::chrono::steady_clock::now(), bound.recheck_interval)
                                       : *bound.expires_by_;
         if (bound.expires_by_) {
            deadline = std::min(deadline, *bound.expires_by_);
         }
         timer.expires_at(deadline);
         co_await timer.async_wait(boost::asio::use_awaitable);
         bound.check();
         if (bound.recheck) {
            auto timeout = checked_deadline(std::chrono::steady_clock::now(), bound.recheck_timeout);
            if (bound.expires_by_) {
               timeout = std::min(timeout, *bound.expires_by_);
            }
            using namespace boost::asio::experimental::awaitable_operators;
            auto result = co_await (safe_recheck(bound.recheck, trusted) || wait_recheck_deadline(timeout));
            const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
            if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
               throw exceptions::cancelled{"invocation lifetime check was cancelled"};
            }
            if (!std::visit([](bool valid) { return valid; }, result)) {
               throw exceptions::invocation_denied{"invocation authority is no longer current"};
            }
            bound.check();
         }
      }
   } catch (const exceptions::invocation_denied&) {
      failure = std::current_exception();
   } catch (const exceptions::protocol_error&) {
      failure = std::current_exception();
   } catch (...) {
      failure = std::make_exception_ptr(exceptions::invocation_denied{"invocation lifetime check failed"});
   }
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      throw exceptions::cancelled{"invocation lifetime check was cancelled"};
   }
   if (failure && on_denied) {
      try {
         on_denied(failure);
      } catch (...) {
         // Keep the original safe denial authoritative during terminal cleanup.
      }
   }
   co_return failure;
}

} // namespace forge::api::core
