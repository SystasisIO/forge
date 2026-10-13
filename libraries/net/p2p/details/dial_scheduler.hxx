#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/compat/move_only_function.hpp>

#include "black_hole_detector.hxx"
#include "dial_ranker.hxx"
#include "direct_attempt.hxx"
#include "dns_address_expander.hxx"

namespace forge::net::p2p {
class cancellation_latch;
}

namespace forge::net::p2p::detail {

struct dial_root_outcome {
   source_root root;
   dialing::outcome outcome = dialing::outcome::neutral;
};

struct dial_result {
   direct_attempt attempt;
   endpoint winner;
   std::vector<source_root> winner_roots;
   std::vector<dial_root_outcome> root_outcomes;
};

// Coordinates one unpublished direct dial. The caller owns winner publication
// through node::impl::commit_direct_attempt after this operation returns.
class dial_scheduler final {
 public:
   using clock = std::chrono::steady_clock;

   struct policy {
      address_resolution::policy resolution{};
      dialing::ranker_policy ranker{};
      dialing::black_hole_policy black_holes{};
      std::size_t max_concurrent_attempts = 4;
   };

   struct request {
      std::vector<forge::multiformats::multiaddr> roots;
      std::optional<peer_id> expected_peer;
      clock::time_point logical_deadline = clock::time_point::max();
      std::chrono::milliseconds attempt_timeout{10'000};
      std::stop_token stop;
      // Launch budget in [1, 100]; unlaunched plan entries remain neutral.
      std::size_t max_attempts = 100;
      bool tcp_only = false;
   };

   using address_lookup = boost::compat::move_only_function<boost::asio::awaitable<forge::net::dns::address_response>(
       std::string, forge::net::dns::address_family, forge::net::dns::query_options, std::stop_token)>;
   using text_lookup = boost::compat::move_only_function<boost::asio::awaitable<forge::net::dns::text_response>(
       std::string, forge::net::dns::query_options, std::stop_token)>;
   using attempt_start = boost::compat::move_only_function<boost::asio::awaitable<direct_attempt>(
       endpoint, std::optional<peer_id>, clock::time_point, std::shared_ptr<cancellation_latch>,
       direct::tcp_transport_progress_handler)>;
   using attempt_discard = boost::compat::move_only_function<boost::asio::awaitable<void>(direct_attempt&)>;
   using owner_stopping = boost::compat::move_only_function<bool() noexcept>;
   using terminal_root_observer =
       boost::compat::move_only_function<void(std::vector<dial_root_outcome>) noexcept>;

   // This bundle is consumed by exactly one async_dial operation. Resolver
   // callbacks are an internal deterministic-test seam; normal operations use
   // the scheduler-owned resolver.
   struct operation_callbacks {
      address_lookup resolve_addresses;
      text_lookup resolve_txt;
      attempt_start start_attempt;
      attempt_discard discard_attempt;
      owner_stopping is_owner_stopping;
      terminal_root_observer observe_terminal_root_outcomes;
      // Optional synchronous admission after identity inference; throwing prevents all attempts.
      boost::compat::move_only_function<void(const std::optional<peer_id>&)> prepare_peer;
   };

   dial_scheduler(boost::asio::any_io_executor executor, policy policy_value,
                  forge::net::dns::resolver_options resolver_options = {});
   ~dial_scheduler();

   dial_scheduler(const dial_scheduler&) = delete;
   dial_scheduler& operator=(const dial_scheduler&) = delete;
   dial_scheduler(dial_scheduler&&) = delete;
   dial_scheduler& operator=(dial_scheduler&&) = delete;

   [[nodiscard]] boost::asio::awaitable<dial_result> async_dial(request value, operation_callbacks callbacks);
   void request_stop() noexcept;
   [[nodiscard]] boost::asio::awaitable<void> async_close();
   [[nodiscard]] dialing::black_hole_status black_hole_status() const;
   static void validate_policy(const policy& value);

 private:
   struct completion {
      std::size_t plan_index = 0;
      endpoint target;
      std::optional<direct_attempt> attempt;
      std::exception_ptr error;
      bool attributable = false;
   };

   struct launch_failure final {
      std::atomic_bool published = false;
      completion value;
   };

   struct state final {
      state();

      void prepare(std::size_t capacity);
      void launch() noexcept;
      void publish(completion value) noexcept;
      [[nodiscard]] std::optional<completion> take_completion();
      [[nodiscard]] std::size_t active() const noexcept;
      [[nodiscard]] bool has_completion() const noexcept;
      [[nodiscard]] clock::time_point tcp_handshake_hold_until() const noexcept;
      [[nodiscard]] std::shared_ptr<cancellation_latch> cancellation() const noexcept;
      [[nodiscard]] std::stop_token stop_token() const noexcept;
      [[nodiscard]] std::shared_ptr<forge::asio::notification> wakeup() const noexcept;
      [[nodiscard]] bool stop_requested() const noexcept;
      void report_tcp_handshake_progress(std::size_t plan_index, std::chrono::milliseconds hold) noexcept;
      void request_stop() noexcept;

    private:
      mutable std::mutex mutex_;
      std::shared_ptr<cancellation_latch> cancellation_;
      std::shared_ptr<forge::asio::notification> wakeup_;
      std::stop_source stop_source_;
      std::vector<std::optional<completion>> completions_;
      std::vector<std::size_t> completion_order_;
      std::vector<clock::time_point> tcp_handshake_holds_;
      std::vector<bool> completed_;
      std::size_t next_completion_ = 0;
      std::size_t active_ = 0;
      bool stopped_ = false;
   };

   struct operation_registry final {
      operation_registry();

      [[nodiscard]] bool admit(const std::shared_ptr<state>& operation);
      void retire(const std::shared_ptr<state>& operation) noexcept;
      void request_stop() noexcept;
      [[nodiscard]] boost::asio::awaitable<void> async_wait_empty();

    private:
      mutable std::mutex mutex_;
      std::set<std::shared_ptr<state>, std::owner_less<std::shared_ptr<state>>> operations_;
      std::shared_ptr<forge::asio::notification> changed_;
      bool sealed_ = false;
   };

   class operation_ticket final {
    public:
      operation_ticket(std::shared_ptr<operation_registry> registry, std::shared_ptr<state> operation) noexcept;
      operation_ticket(const operation_ticket&) = delete;
      operation_ticket& operator=(const operation_ticket&) = delete;
      operation_ticket(operation_ticket&&) = delete;
      operation_ticket& operator=(operation_ticket&&) = delete;
      ~operation_ticket();

    private:
      std::shared_ptr<operation_registry> registry_;
      std::shared_ptr<state> operation_;
   };

   struct owner final {
      owner(boost::asio::any_io_executor executor, policy policy_value,
            forge::net::dns::resolver_options resolver_options);

      policy policy_;
      forge::net::dns::resolver resolver_;
      dns_address_expander expander_;
      dial_ranker ranker_;
      black_hole_detector black_holes_;
      std::shared_ptr<operation_registry> operations_;
   };

   [[nodiscard]] static boost::asio::awaitable<dial_result>
   async_dial_owned(std::shared_ptr<owner> owner, request value, operation_callbacks callbacks);
   [[nodiscard]] static boost::asio::awaitable<void> async_close_owned(std::shared_ptr<owner> owner);
   [[nodiscard]] static boost::asio::awaitable<void>
   async_attempt_worker(std::shared_ptr<operation_callbacks> callbacks, std::shared_ptr<state> operation,
                        std::optional<peer_id> expected_peer, clock::time_point deadline, dial_plan_item item,
                        std::size_t index);
   [[nodiscard]] static boost::asio::awaitable<void>
   async_discard_preserving(std::shared_ptr<operation_callbacks> callbacks, std::vector<direct_attempt>& pending,
                            direct_attempt attempt);
   [[nodiscard]] static boost::asio::awaitable<std::exception_ptr>
   async_drain_pending_discards(std::shared_ptr<operation_callbacks> callbacks, std::vector<direct_attempt>& pending);

   [[nodiscard]] static bool attributable_failure(const std::exception_ptr& error, bool owner_is_stopping) noexcept;
   [[nodiscard]] static bool is_canceled(const std::stop_token& stop) noexcept;
   [[nodiscard]] static clock::time_point candidate_deadline(clock::time_point logical_deadline,
                                                              std::chrono::milliseconds timeout,
                                                              clock::time_point launched) noexcept;
   static void validate_request(const request& value);

   std::shared_ptr<owner> owner_;
};

[[nodiscard]] dial_scheduler::policy dial_scheduler_policy(const address_resolution::policy& resolution,
                                                          const dialing::policy& direct, bool private_network);

} // namespace forge::net::p2p::detail
