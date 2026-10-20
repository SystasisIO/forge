#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>

namespace forge::api::p2p::detail {

class publication_state final : public std::enable_shared_from_this<publication_state> {
 public:
   using close_handler = std::function<void()>;
   using drain_handler = std::function<boost::asio::awaitable<void>()>;
   using active_handler = std::function<bool()>;

   publication_state(boost::asio::any_io_executor owner_executor, close_handler close, drain_handler drain,
                     active_handler active);
   ~publication_state();

   publication_state(const publication_state&) = delete;
   publication_state& operator=(const publication_state&) = delete;

   [[nodiscard]] bool active() const noexcept;
   [[nodiscard]] std::stop_token stop_token() const noexcept;
   void bind_native(std::stop_token);
   void close() noexcept;
   boost::asio::awaitable<void> async_close();

 private:
   boost::asio::awaitable<void> wait_for_close();
   boost::asio::awaitable<void> wait_for_drain();
   void launch_drain(drain_handler drain) noexcept;
   void finish_close(std::exception_ptr failure) noexcept;
   void finish_drain(std::exception_ptr failure) noexcept;

   mutable std::mutex mutex_;
   boost::asio::any_io_executor owner_executor_;
   close_handler close_;
   drain_handler drain_;
   active_handler active_;
   std::stop_source stop_;
   std::unique_ptr<std::stop_callback<std::function<void()>>> native_stop_;
   forge::asio::notification close_ready_;
   forge::asio::notification drain_ready_;
   std::exception_ptr close_failure_;
   std::exception_ptr drain_failure_;
   bool close_requested_ = false;
   bool close_finished_ = false;
   bool drain_started_ = false;
   bool drain_finished_ = false;
};

} // namespace forge::api::p2p::detail
