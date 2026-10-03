#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

#include "lifecycle_tracker.hxx"

namespace forge::net::p2p {
class cancellation_latch;
}

namespace forge::net::p2p::detail {

class lifecycle_wakeup;

class autorelay_manager final : public lifecycle_stop_listener,
                               public std::enable_shared_from_this<autorelay_manager> {
 public:
   using time_point = std::chrono::steady_clock::time_point;

   struct candidate {
      peer_id peer;
      std::uint64_t session_id = 0; // Zero is an untrusted, bounded topology/store hint.
   };

   struct reservation {
      relay::reservation::info info;
      time_point expires_at;
      time_point renew_at;
      bool automatic = false;
   };

   struct snapshot {
      bool permitted = false;
      std::uint64_t generation = 0;
      std::vector<candidate> candidates;
      std::vector<reservation> reservations;
      time_point next_expiry = time_point::max();
   };

   struct callbacks {
      std::function<snapshot()> current;
      std::function<boost::asio::awaitable<relay::reservation::info>(candidate, std::uint64_t,
                                                                  std::shared_ptr<cancellation_latch>)> reserve;
      std::function<time_point()> now;
   };

   autorelay_manager(boost::asio::any_io_executor executor, relay::policy policy, callbacks value);
   ~autorelay_manager();
   void start(lifecycle_tracker& tracker);
   void notify() noexcept;
   void cancel_peer(const peer_id& peer) noexcept;
   void request_stop() noexcept;
   void request_lifecycle_stop() noexcept override;
   boost::asio::awaitable<void> async_join();
   boost::asio::awaitable<std::vector<relay::reservation::info>> async_refresh(std::chrono::milliseconds timeout);
   [[nodiscard]] diagnostics::autorelay_state stats() const;
   struct selection_state {
      std::vector<candidate> pending;
      std::vector<peer_id> unavailable;
   };
   [[nodiscard]] selection_state selection() const;

 private:
   struct work {
      candidate source;
      std::uint64_t generation = 0;
      std::shared_ptr<cancellation_latch> cancellation;
      bool done = false;
      bool registered = false;
      bool invalidated = false;
      bool renewal = false;
      std::optional<exceptions::code> error;
      std::optional<std::uint64_t> reservation_id;
   };

   struct candidate_state {
      candidate source;
      time_point retry_after{};
      time_point acquisition_after{};
      std::uint64_t failures = 0;
      std::shared_ptr<work> pending;
   };
   struct retry_state {
      time_point retry_after{};
      time_point acquisition_after{};
      std::uint64_t failures = 0;
   };

   static boost::asio::awaitable<void> run_owned(std::shared_ptr<autorelay_manager> self);
   static boost::asio::awaitable<void> reserve_owned(std::shared_ptr<autorelay_manager> self,
                                                    std::shared_ptr<work> item);
   [[nodiscard]] time_point tick();
   void complete(const std::shared_ptr<work>& item, std::optional<exceptions::code> error) noexcept;
   void parent_complete(std::exception_ptr error) noexcept;
   void finish_if_ready() noexcept;
   void reap_locked(time_point now, const std::vector<reservation>& reservations);
   [[nodiscard]] time_point backoff_locked(candidate_state& value, time_point now);

   boost::asio::strand<boost::asio::any_io_executor> _strand;
   relay::policy _policy;
   callbacks _callbacks;
   std::shared_ptr<lifecycle_wakeup> _wakeup;
   std::shared_ptr<lifecycle_wakeup> _changed;
   mutable std::mutex _mutex;
   std::map<peer_id, candidate_state> _candidates;
   // Bounded retry history survives rotating a candidate out of the active set.
   std::map<peer_id, retry_state> _backoffs;
   std::map<peer_id, std::shared_ptr<work>> _pending_cancellations;
   lifecycle_tracker::operation _operation;
   lifecycle_stop_subscription _subscription;
   diagnostics::autorelay_state _stats;
   std::uint64_t _round = 0;
   std::uint64_t _random = 0;
   std::uint64_t _children = 0;
   bool _started = false;
   bool _stopping = false;
   bool _finished = false;
   bool _parent_done = false;
   std::exception_ptr _failure;
};

} // namespace forge::net::p2p::detail
