#pragma once

#include <exception>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/steady_timer.hpp>

#include "path_manager.hxx"

namespace forge::net::p2p::detail {

// One prearmed terminal wait, independent of cancellation/deadline delivery.
class path_manager::dial_batch final : public std::enable_shared_from_this<dial_batch> {
 public:
   explicit dial_batch(boost::asio::any_io_executor executor);
   const std::shared_ptr<cancellation_latch> cancellation;

   void add_worker(bool wait_inbound = false);
   void complete_worker() noexcept;
   void stop_waiting() noexcept;
   void direct_arrived() noexcept;
   void fail(std::exception_ptr error) noexcept;
   [[nodiscard]] std::size_t active() const;
   [[nodiscard]] std::exception_ptr failure() const;

   template <typename Launch, typename CompletionToken>
   auto async_run(Launch launch, CompletionToken&& token) {
      return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
          [self = shared_from_this(), launch = std::move(launch)](auto handler) mutable {
             // Terminal delivery on another executor may destroy this closure
             // before seal() unlocks. Keep ownership on the initiating stack.
             const auto owner = std::move(self);
             // Native wait allocation/registration precedes every worker. Its
             // associated allocator is preserved; a preparation failure cannot
             // publish children or enter an allocating drain/retry loop.
             {
                const auto lock = std::scoped_lock{owner->_mutex};
                owner->_ready.async_wait(boost::asio::bind_cancellation_slot(
                    boost::asio::cancellation_slot{}, std::move(handler)));
             }
             try { launch(); }
             catch (...) { owner->fail(std::current_exception()); }
             owner->seal();
          }, token);
   }

 private:
   void seal() noexcept;
   void signal_ready_locked() noexcept;

   mutable std::mutex _mutex;
   boost::asio::steady_timer _ready;
   std::size_t _workers = 0;
   bool _sealed = false;
   bool _wait_inbound = false;
   bool _stopped = false;
   bool _succeeded = false;
   bool _signaled = false;
   std::exception_ptr _failure;
};

} // namespace forge::net::p2p::detail
