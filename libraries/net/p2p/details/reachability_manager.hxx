#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

#include "lifecycle_tracker.hxx"
#include "reachability_state.hxx"

namespace forge::net::p2p {
class cancellation_latch;
}

namespace forge::net::p2p::detail {

class lifecycle_wakeup;
class worker_terminal_owner;

class reachability_manager final : public std::enable_shared_from_this<reachability_manager> {
   using time_point = std::chrono::steady_clock::time_point;

 public:
   struct observer {
      peer_id peer;
      endpoint remote;
      bool v1 = false;
      bool v2 = false;
      bool ping = false;
      std::uint64_t session_id = 0;
   };

   struct probe_result {
      reachability::result result;
      bool verified_dialback = false;
      bool internet_scope = false;
   };

   struct callbacks {
      std::function<std::vector<observer>()> observers;
      // Refresh observed-address confirmations before returning the current
      // candidates and their generation atomically. The owner updates state at
      // mutation, not when the manager consumes this potentially stale snapshot.
      // Called on every parent tick, even if NAT state is unchanged.
      std::function<reachability_state::candidate_snapshot()> candidates;
      std::function<boost::asio::awaitable<probe_result>(observer, bool, std::vector<endpoint>,
                                                        std::shared_ptr<cancellation_latch>)> exchange;
      std::function<boost::asio::awaitable<void>(peer_id, std::shared_ptr<cancellation_latch>)> ping;
      std::function<void(host_event)> changed;
      std::function<void()> finished;
      std::function<std::chrono::steady_clock::time_point()> now;
   };

   struct statistics {
      std::size_t pending_probes = 0;
      std::size_t pending_pings = 0;
      std::size_t waiters = 0;
      std::uint64_t ping_successes = 0;
      std::uint64_t ping_failures = 0;
      std::uint64_t probe_errors = 0;
      std::exception_ptr last_probe_error;
   };

   reachability_manager(boost::asio::any_io_executor executor, reachability_policy policy, callbacks value);
   ~reachability_manager();
   void start(lifecycle_tracker& tracker);
   void request_stop() noexcept;
   boost::asio::awaitable<void> async_join();
   // Requires start(). Both eligible versions run independently; return the
   // last completed version's actual result. Partial errors remain in stats;
   // if no version completes, rethrow the first error.
   boost::asio::awaitable<reachability::result> async_probe(observer value);
   [[nodiscard]] host_event current() const;
   [[nodiscard]] statistics stats() const;
   void notify_addresses_changed() noexcept;
   [[nodiscard]] std::uint64_t set_addresses(std::span<const endpoint> addresses);
   [[nodiscard]] reachability_state::candidate_snapshot candidates() const;
   void invalidate_addresses() noexcept;
   void close_results() noexcept;

 private:
   struct work {
      observer source;
      bool ping = false;
      bool done = false;
      bool timed_out = false;
      time_point deadline = time_point::max();
      std::shared_ptr<cancellation_latch> cancellation;
      std::shared_ptr<lifecycle_wakeup> completed;
      reachability::result result;
      std::exception_ptr error;
      std::exception_ptr partial_error;
   };

   struct peer_state {
      time_point next_probe{};
      time_point next_ping{};
      std::chrono::milliseconds backoff{0};
      std::uint64_t candidate_generation = 0;
      std::size_t next_candidate = 0;
      std::shared_ptr<work> probe;
      std::shared_ptr<work> ping;
   };

   static boost::asio::awaitable<void> run_owned(std::shared_ptr<reachability_manager> self);
   static boost::asio::awaitable<void> run_lifecycle(std::shared_ptr<reachability_manager> self,
                                                  std::shared_ptr<lifecycle_stop_source> stop);
   static boost::asio::awaitable<void> run_bridged(std::shared_ptr<reachability_manager> self,
                                                 std::shared_ptr<worker_terminal_owner> terminal);
   static boost::asio::awaitable<void> worker_owned(std::shared_ptr<reachability_manager> self,
                                                   std::shared_ptr<work> item);
   static boost::asio::awaitable<void> join_owned(std::shared_ptr<reachability_manager> self);
   static boost::asio::awaitable<reachability::result> probe_owned(std::shared_ptr<reachability_manager> self,
                                                                 observer value);
   [[nodiscard]] std::shared_ptr<work> launch(observer value, bool ping, time_point now, bool waiter);
   void reap_locked(peer_state& peer, time_point now);
   void complete(const std::shared_ptr<work>& item, std::exception_ptr error) noexcept;
   [[nodiscard]] reachability_state::candidate_snapshot reconcile();
   void publish(time_point now);
   [[nodiscard]] bool stopping() const;
   [[nodiscard]] time_point tick(time_point now);

   boost::asio::strand<boost::asio::any_io_executor> strand_;
   reachability_policy policy_;
   callbacks callbacks_;
   reachability_state state_;
   std::shared_ptr<lifecycle_wakeup> wakeup_;
   std::shared_ptr<cancellation_latch> cancellation_;
   mutable std::mutex mutex_;
   std::map<peer_id, peer_state> peers_;
   statistics statistics_;
   bool started_ = false;
   bool stopping_ = false;
   bool finished_ = false;
   std::exception_ptr failure_;
   std::optional<host_event> published_;
};

} // namespace forge::net::p2p::detail
