#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace forge::net::quic::detail {

class listener_shutdown {
 public:
   listener_shutdown(boost::asio::any_io_executor executor, std::size_t connections);
   void admit() noexcept;
   void complete_stop() noexcept;
   void complete_connection(std::size_t index) noexcept;
   void set_operations_idle(bool idle) noexcept;
   void remember(std::exception_ptr error) noexcept;
   void abort_preparation(std::exception_ptr error) noexcept;
   void finish() noexcept;
   [[nodiscard]] bool admitted() const noexcept;
   [[nodiscard]] std::exception_ptr failure() const noexcept;

   template <typename Launch, typename CompletionToken> auto async_wait_ready(Launch launch, CompletionToken&& token) {
      return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
          [this, launch = std::move(launch)](auto handler) mutable {
             {
                auto lock = std::scoped_lock{_mutex};
                _ready.async_wait(boost::asio::bind_cancellation_slot(
                    boost::asio::cancellation_slot{}, std::move(handler)));
                if (_ready_signaled) { cancel(_ready); }
             }
             // Preparation/stop never runs under the completion mutex.
             launch();
          },
          token);
   }

   template <typename CompletionToken> auto async_wait_finished(CompletionToken&& token) {
      return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
          [this](auto handler) {
             auto lock = std::scoped_lock{_mutex};
             _finished.async_wait(boost::asio::bind_cancellation_slot(
                 boost::asio::cancellation_slot{}, std::move(handler)));
             if (_finished_signaled) { cancel(_finished); }
          },
          token);
   }

 private:
   static void cancel(boost::asio::steady_timer& timer) noexcept;
   void signal_ready() noexcept;
   mutable std::mutex _mutex;
   boost::asio::steady_timer _ready;
   boost::asio::steady_timer _finished;
   std::vector<bool> _completed;
   std::size_t _remaining;
   std::exception_ptr _error;
   bool _admitted = false;
   bool _stop_complete = false;
   bool _aborted = false;
   bool _operations_complete = false;
   bool _ready_signaled = false;
   bool _finished_signaled = false;
};

} // namespace forge::net::quic::detail
