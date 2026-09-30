#include "details/notification_waiter.hxx"

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <memory>
#include <utility>

namespace forge::asio::detail {

notification_waiter::notification_waiter(boost::asio::any_io_executor executor)
    : timer_{std::move(executor), boost::asio::steady_timer::time_point::max()} {}

boost::asio::awaitable<boost::system::error_code>
notification_waiter::wait_until(std::chrono::steady_clock::time_point deadline) {
   co_return co_await wait_until_impl(deadline, cancellation_mode::disabled);
}

boost::asio::awaitable<boost::system::error_code>
notification_waiter::wait_until_cancellable(std::chrono::steady_clock::time_point deadline) {
   co_return co_await wait_until_impl(deadline, cancellation_mode::associated);
}

boost::asio::awaitable<boost::system::error_code>
notification_waiter::wait_until(std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
   auto cancel = std::stop_callback{stop, [weak = weak_from_this()]() noexcept {
                                       if (const auto waiter = weak.lock()) {
                                          waiter->wake();
                                       }
                                    }};
   co_return co_await wait_until_impl(deadline, cancellation_mode::disabled);
}

boost::asio::awaitable<boost::system::error_code>
notification_waiter::wait_until_impl(std::chrono::steady_clock::time_point deadline,
                                     cancellation_mode mode) {
   auto error = boost::system::error_code{};
   auto initiate = [self = shared_from_this(), deadline](auto handler) mutable {
      // Completion on another worker can destroy the initiating closure before
      // this call returns. Keep ownership on this stack through mutex unlock.
      const auto owner = std::move(self);
      auto lock = std::scoped_lock{owner->mutex_};
      const auto wake_requested = owner->wake_requested_;
      owner->timer_.expires_at(wake_requested ? boost::asio::steady_timer::time_point::max() : deadline);
      owner->timer_.async_wait(std::move(handler));
      if (wake_requested) {
         static_cast<void>(owner->timer_.cancel());
      }
   };
   auto wait_token = boost::asio::redirect_error(boost::asio::use_awaitable, error);
   if (mode == cancellation_mode::associated) {
      co_await boost::asio::async_initiate<decltype(wait_token), void(boost::system::error_code)>(
          std::move(initiate), wait_token);
   } else {
      auto bound_token = boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{},
                                                              std::move(wait_token));
      co_await boost::asio::async_initiate<decltype(bound_token), void(boost::system::error_code)>(
          std::move(initiate), bound_token);
   }
   co_return error;
}

void notification_waiter::wake() noexcept {
   const auto lock = std::scoped_lock{mutex_};
   wake_requested_ = true;
   try {
      static_cast<void>(timer_.cancel());
   } catch (...) {
      // A live wait owns a valid timer; cancellation has no allocation path.
      // Preserve the sticky state if the platform nevertheless reports an
      // unrecoverable timer service error.
   }
}

} // namespace forge::asio::detail
