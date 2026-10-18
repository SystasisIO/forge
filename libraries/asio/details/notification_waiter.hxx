#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>

namespace forge::asio::detail {

class notification_waiter final : public std::enable_shared_from_this<notification_waiter> {
 public:
   explicit notification_waiter(boost::asio::any_io_executor executor);

   boost::asio::awaitable<boost::system::error_code> wait_until(
       std::chrono::steady_clock::time_point deadline);
   boost::asio::awaitable<boost::system::error_code> wait_until(
       std::chrono::steady_clock::time_point deadline, std::stop_token stop);
   void wake() noexcept;

 private:
   boost::asio::awaitable<boost::system::error_code> wait_until_impl(
       std::chrono::steady_clock::time_point deadline);

   boost::asio::steady_timer timer_;
   mutable std::mutex mutex_;
   bool wake_requested_ = false;
};

} // namespace forge::asio::detail
