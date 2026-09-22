#pragma once

#include "connection_manager.hxx"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace forge::net::p2p {

class cancellation_latch;

namespace detail {

class lifecycle_tracker;
class lifecycle_wakeup;

[[nodiscard]] std::chrono::system_clock::time_point
saturating_topology_expiry(std::chrono::system_clock::time_point now, std::chrono::milliseconds interval) noexcept;

class topology_manager : public std::enable_shared_from_this<topology_manager> {
 public:
   enum class phase : std::uint8_t {
      idle,
      running,
      stopping,
      stopped,
   };

   struct callbacks {
      struct rendezvous_local_record {
         std::uint64_t generation = 0;
         std::vector<std::uint8_t> signed_peer_record;
      };

      struct rendezvous_register_result {
         bool accepted = false;
         std::chrono::seconds ttl{0};
      };

      struct rendezvous_discover_result {
         enum class status : std::uint8_t {
            ok,
            invalid_cookie,
            rejected,
         };

         status response_status = status::rejected;
         std::vector<discovery::result> results;
         std::vector<std::uint8_t> cookie;
      };

      std::function<boost::asio::awaitable<std::vector<discovery::result>>(std::shared_ptr<cancellation_latch>)>
          discover;
      std::function<boost::asio::awaitable<std::vector<discovery::result>>(std::shared_ptr<cancellation_latch>,
                                                                             std::size_t)>
          peer_exchange;
      std::function<rendezvous_local_record()> local_rendezvous_record;
      std::function<boost::asio::awaitable<rendezvous_register_result>(std::size_t, std::string,
                                                                         std::vector<std::uint8_t>,
                                                                         std::shared_ptr<cancellation_latch>)>
          rendezvous_register;
      std::function<boost::asio::awaitable<rendezvous_discover_result>(std::size_t, std::string, std::size_t,
                                                                         std::vector<std::uint8_t>,
                                                                         std::shared_ptr<cancellation_latch>)>
          rendezvous_discover;
      std::function<boost::asio::awaitable<void>(std::size_t, std::string)> rendezvous_unregister;
      std::function<boost::asio::awaitable<bool>(discovery::result, std::shared_ptr<cancellation_latch>)> dial;
      std::function<void()> refresh_connection_scores;
      std::function<connection_manager::snapshot()> sessions;
      std::function<connection_manager::peer_prune_plan(std::size_t, std::size_t,
                                                         std::chrono::steady_clock::time_point)>
          plan_peer_prune;
      std::function<boost::asio::awaitable<void>(std::vector<std::uint64_t>)> close_sessions;
   };

   struct clocks {
      std::function<std::chrono::steady_clock::time_point()> steady_now;
      std::function<std::chrono::system_clock::time_point()> system_now;
      std::function<void()> before_idle_wait;
      std::function<boost::asio::awaitable<void>(std::chrono::steady_clock::time_point)> idle_wait;
      std::function<void()> before_refresh_completion;
      std::function<void()> before_parent_completion;
      std::function<void()> before_dial_join_wait;
      std::function<void()> before_dial_worker_completion;
   };

   struct status {
      phase lifecycle_phase = phase::idle;
      bool refresh_queued = false;
      bool refresh_in_flight = false;
      std::size_t observations = 0;
      std::size_t mdns_observations = 0;
      std::size_t active_operations = 0;
      std::size_t waiting_refreshes = 0;
      std::uint64_t completed_refreshes = 0;
      std::uint64_t failed_refreshes = 0;
   };

   topology_manager(topology::policy policy, callbacks callbacks_value, clocks clocks_value = {},
                    std::uint64_t periodic_jitter_seed = 0);
   ~topology_manager();

   topology_manager(const topology_manager&) = delete;
   topology_manager& operator=(const topology_manager&) = delete;

   void start(lifecycle_tracker& lifecycle);
   void request_stop() noexcept;
   boost::asio::awaitable<std::vector<discovery::result>> async_refresh();
   boost::asio::awaitable<void> async_join();
   [[nodiscard]] status current() const;

   struct mdns_lease {
      peer_id peer;
      forge::multiformats::multiaddr address;
      std::uint32_t interface_index = 0;
      std::uint64_t generation = 0;
      std::chrono::steady_clock::time_point expires_at;
      std::string address_key{}; // Recomputed at snapshot admission, never trusted from callers.
   };
   // Serialized full-source replacement; callbacks/cancellation run after unlock.
   void replace_mdns_snapshot(std::vector<mdns_lease> leases);

 private:
   struct observation_key {
      peer_id peer;
      discovery::source source = discovery::source::explicit_config;

      [[nodiscard]] bool operator<(const observation_key& other) const noexcept;
   };

   struct observation {
      discovery::result result;
      std::chrono::system_clock::time_point expires_at{};
      std::chrono::steady_clock::time_point retry_after{};
      std::size_t failures = 0;
   };

   struct rendezvous_key {
      peer_id peer;
      std::string namespace_name;

      [[nodiscard]] bool operator<(const rendezvous_key& other) const noexcept;
   };

   struct rendezvous_state {
      std::size_t point_index = 0;
      bool confirmed_registration = false;
      std::uint64_t registered_generation = 0;
      std::chrono::system_clock::time_point expires_at{};
      std::chrono::system_clock::time_point renew_after{};
      std::chrono::steady_clock::time_point retry_after{};
      std::size_t failures = 0;
      std::vector<std::uint8_t> cookie;
   };

   struct rendezvous_work {
      rendezvous_key key;
      std::size_t point_index = 0;
      bool registration_due = false;
      std::vector<std::uint8_t> cookie;
   };

   struct mdns_observation {
      std::vector<mdns_lease> leases;
      std::chrono::steady_clock::time_point retry_after{};
      std::size_t failures = 0;
      std::uint64_t revision = 0;
   };
   struct dial_candidate {
      discovery::result result;
      std::uint64_t mdns_revision = 0;
   };
   struct mdns_attempt {
      peer_id peer;
      std::uint64_t revision = 0;
      std::chrono::steady_clock::time_point deadline;
      bool finished = false;
      bool expired = false;
   };
   struct dial_batch {
      mutable std::mutex mutex;
      std::vector<dial_candidate> candidates;
      std::shared_ptr<lifecycle_wakeup> completed;
      std::shared_ptr<cancellation_latch> cancellation;
      std::size_t next = 0;
      std::size_t remaining_workers = 0;
      std::size_t successes = 0;
      std::size_t in_flight = 0;
      std::size_t required = 0;
      bool admission_closed = false;
      bool launches_complete = false;
      bool completion_notified = false;
      std::exception_ptr failure;
   };

   struct completion {
      std::vector<discovery::result> results;
      std::exception_ptr failure;
   };

   struct refresh_waiters {
      std::size_t count = 0;
      std::optional<completion> completed;
   };

   [[nodiscard]] bool stopping() const noexcept;
   [[nodiscard]] std::chrono::milliseconds periodic_refresh_delay(std::uint64_t sequence) const noexcept;
   [[nodiscard]] bool rendezvous_refresh_due_locked(std::chrono::steady_clock::time_point steady_now,
                                                    std::chrono::system_clock::time_point system_now) const noexcept;
   [[nodiscard]] std::chrono::steady_clock::time_point next_autonomous_wakeup() const;
   [[nodiscard]] bool queue_due_refresh_locked(std::chrono::steady_clock::time_point steady_now,
                                               std::chrono::system_clock::time_point system_now);
   [[nodiscard]] std::uint64_t queue_refresh_locked();
   void release_waiter(std::uint64_t generation) noexcept;
   void finish_parent(std::exception_ptr failure) noexcept;
   void finish_refresh(std::uint64_t generation, std::vector<discovery::result> results,
                       std::exception_ptr failure) noexcept;
   void add_cancellation(const std::shared_ptr<cancellation_latch>& cancellation);
   void remove_cancellation(const std::shared_ptr<cancellation_latch>& cancellation) noexcept;
   boost::asio::awaitable<void> async_run();
   boost::asio::awaitable<void> async_refresh_generation(std::uint64_t generation);
   boost::asio::awaitable<std::vector<discovery::result>> async_collect_discovery();
   boost::asio::awaitable<std::vector<discovery::result>>
   async_collect_rendezvous(const std::shared_ptr<cancellation_latch>& cancellation, std::size_t limit);
   boost::asio::awaitable<void> async_unregister_rendezvous();
   void merge_observations(const std::vector<discovery::result>& results);
   [[nodiscard]] std::vector<dial_candidate> candidates_for_dial(const connection_manager::snapshot& sessions);
   [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
   admit_mdns_dial(dial_candidate& candidate, const std::shared_ptr<cancellation_latch>& cancellation);
   void finish_mdns_dial(const dial_candidate& candidate, const std::shared_ptr<cancellation_latch>& cancellation,
                         bool succeeded);
   boost::asio::awaitable<void> async_watch_mdns_expiry(std::shared_ptr<cancellation_latch> cancellation);
   boost::asio::awaitable<bool> async_mdns_connect(boost::asio::awaitable<bool> dial,
                                                 std::shared_ptr<cancellation_latch> cancellation);
   void note_dial_result(const discovery::result& result, bool succeeded) noexcept;
   [[nodiscard]] std::chrono::milliseconds retry_delay(const observation_key& key, std::size_t failures) const;
   [[nodiscard]] std::chrono::milliseconds retry_delay(const rendezvous_key& key, std::size_t failures) const;
   void note_rendezvous_failure(const rendezvous_key& key) noexcept;
   void note_rendezvous_success(const rendezvous_key& key, std::vector<std::uint8_t> cookie) noexcept;
   boost::asio::awaitable<void> async_reconcile_sessions();
   boost::asio::awaitable<void> async_dial_candidates(std::vector<dial_candidate> candidates,
                                                       std::size_t required);
   boost::asio::awaitable<void> async_dial_worker(const std::shared_ptr<dial_batch>& batch);

   topology::policy policy_;
   callbacks callbacks_;
   clocks clocks_;
   std::uint64_t periodic_jitter_seed_ = 0;
   std::shared_ptr<lifecycle_wakeup> changed_;
   lifecycle_tracker* lifecycle_ = nullptr;
   mutable std::mutex mutex_;
   std::map<observation_key, observation> observations_;
   std::map<peer_id, mdns_observation> mdns_;
   std::map<std::shared_ptr<cancellation_latch>, mdns_attempt> mdns_attempts_;
   std::uint64_t mdns_revision_ = 0;
   bool reconcile_pending_ = false;
   std::map<rendezvous_key, rendezvous_state> rendezvous_clients_;
   std::vector<std::shared_ptr<cancellation_latch>> active_cancellations_;
   phase phase_ = phase::idle;
   bool started_ = false;
   bool refresh_queued_ = false;
   bool refresh_running_ = false;
   bool parent_finished_ = false;
   std::uint64_t next_generation_ = 1;
   std::uint64_t queued_generation_ = 0;
   std::uint64_t running_generation_ = 0;
   std::map<std::uint64_t, refresh_waiters> waiters_;
   std::uint64_t completed_refreshes_ = 0;
   std::uint64_t failed_refreshes_ = 0;
   std::chrono::steady_clock::time_point next_periodic_refresh_{};
   std::uint64_t periodic_refresh_sequence_ = 0;
   std::size_t next_source_index_ = 0;
};

} // namespace detail
} // namespace forge::net::p2p
