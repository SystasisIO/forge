#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "relay_budget.hxx"
#include "lifecycle_tracker.hxx"

namespace forge::net::p2p::detail {

class relay_pair {
 private:
   // Released last, after the task closures, logical admission and stream scopes.
   lifecycle_tracker::operation _operation;

 public:
   class worker {
    public:
      enum class kind { pump, deadline };
      worker(std::shared_ptr<relay_pair> pair, kind value) noexcept;
      ~worker();
      worker(const worker&) = delete;
      worker& operator=(const worker&) = delete;
      void enter() noexcept;

    private:
      std::shared_ptr<relay_pair> _pair;
      kind _kind;
      bool _entered = false;
   };

   relay_pair(peer_id owner_value, forge::net::p2p::stream left_value, forge::net::p2p::stream right_value,
              resource_manager::relay_reservation circuit_value, boost::asio::any_io_executor executor,
              std::chrono::milliseconds duration, std::uint64_t byte_limit,
              std::function<void()> release = {}, lifecycle_tracker::operation operation = {});
   ~relay_pair();

   void take_ownership() noexcept;
   [[nodiscard]] bool mark_finished() noexcept;
   boost::asio::awaitable<bool> async_wait_deadline();
   void cancel_streams() noexcept;

   peer_id owner;
   forge::net::p2p::stream left;
   forge::net::p2p::stream right;
   // HOP and STOP retain their own stream scopes; this owns the circuit span.
   resource_manager::relay_reservation circuit;
   relay_budget left_to_right;
   relay_budget right_to_left;

 private:
   void cancel_deadline() noexcept;

   std::shared_ptr<boost::asio::steady_timer> deadline_;
   std::mutex mutex_;
   std::uint32_t finished_ = 0;
   bool deadline_cancelled_ = false;
   std::function<void()> _release;
   bool _owns_circuit = false;
};

} // namespace forge::net::p2p::detail
