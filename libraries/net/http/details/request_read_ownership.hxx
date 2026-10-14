#pragma once

#include <memory>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>

namespace forge::net::http::detail {

class request_read_ownership {
 public:
   explicit request_read_ownership(boost::asio::any_io_executor executor);
   boost::asio::awaitable<void> begin_body_read();
   void finish_body_read() noexcept;
   boost::asio::awaitable<bool> begin_monitor_read();
   void finish_monitor_read() noexcept;
   void stop_monitor();
   void mark_connection_closed();
   void mark_peer_read_closed();
   [[nodiscard]] bool peer_read_closed() const noexcept;
   [[nodiscard]] bool can_read_next_request(bool has_buffered_input) const noexcept;
   [[nodiscard]] bool body_read_requested() const noexcept;
   [[nodiscard]] bool monitor_stopping() const noexcept;
   [[nodiscard]] boost::asio::cancellation_slot monitor_cancellation_slot() noexcept;
   boost::asio::awaitable<bool> wait_for_monitor_retry();

 private:
   boost::asio::awaitable<void> wait_for_change();
   void notify() noexcept;
   boost::asio::steady_timer changed_;
   boost::asio::cancellation_signal monitor_operation_cancellation_;
   bool body_read_requested_ = false;
   bool body_read_active_ = false;
   bool monitor_read_active_ = false;
   bool monitor_stopping_ = false;
   bool connection_closed_ = false;
   bool peer_read_closed_ = false;
};

class body_read_scope {
 public:
   explicit body_read_scope(std::shared_ptr<request_read_ownership> ownership);
   ~body_read_scope();
   body_read_scope(const body_read_scope&) = delete;
   body_read_scope& operator=(const body_read_scope&) = delete;

 private:
   std::shared_ptr<request_read_ownership> ownership_;
};

class monitor_read_scope {
 public:
   explicit monitor_read_scope(std::shared_ptr<request_read_ownership> ownership);
   ~monitor_read_scope();
   monitor_read_scope(const monitor_read_scope&) = delete;
   monitor_read_scope& operator=(const monitor_read_scope&) = delete;

 private:
   std::shared_ptr<request_read_ownership> ownership_;
};

} // namespace forge::net::http::detail
