#pragma once

#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/autorelay_manager.hxx"

namespace forge::tests::p2p {

class autorelay_manager_fixture {
 public:
   using manager = forge::net::p2p::detail::autorelay_manager;
   using result = std::vector<forge::net::p2p::relay::reservation::info>;

   struct request {
      manager::candidate source;
      std::uint64_t generation = 0;
      std::shared_ptr<forge::net::p2p::cancellation_latch> cancellation;
      bool released = false;
      bool fail = false;
      bool canceled = false;
      bool cancellation_observed = false;
      bool completed = false;
   };

   explicit autorelay_manager_fixture(forge::net::p2p::relay::policy policy = {});
   ~autorelay_manager_fixture();
   autorelay_manager_fixture(const autorelay_manager_fixture&) = delete;
   autorelay_manager_fixture& operator=(const autorelay_manager_fixture&) = delete;

   [[nodiscard]] static manager::candidate candidate(std::uint8_t id);
   [[nodiscard]] static manager::snapshot snapshot(std::initializer_list<std::uint8_t> ids,
                                                   bool permitted = true, std::uint64_t generation = 1);
   [[nodiscard]] static manager::reservation lease(const manager::candidate& source, bool due = false,
                                                   bool automatic = true);
   [[nodiscard]] static manager::time_point now();
   [[nodiscard]] const std::shared_ptr<manager>& owner() const;
   [[nodiscard]] std::vector<request> requests() const;
   [[nodiscard]] std::size_t peak_active() const;

   void set_snapshot(manager::snapshot value);
   void after_snapshot(std::function<void()> callback);
   void on_cancel(std::function<void()> callback);
   void hold_canceled(bool value = true);
   void immediate_success(bool value = true);
   void fail_snapshot(bool value = true);
   void fail_snapshot(std::exception_ptr error);
   void start();
   void release(std::size_t index, bool fail = false);
   void wait_started(std::size_t count);
   void wait_canceled(std::size_t count);
   void wait_cancellation_observed(std::size_t count);
   void wait_state(std::function<bool(const forge::net::p2p::diagnostics::autorelay_state&)> predicate);
   std::future<result> refresh(std::chrono::milliseconds timeout = std::chrono::seconds{2});
   std::future<void> join();
   std::future<void> stop(bool through_lifecycle = false);
   std::future<void> tracked();
   std::future<forge::net::p2p::diagnostics::autorelay_state> tracked_state();

 private:
   manager::snapshot current();
   boost::asio::awaitable<forge::net::p2p::diagnostics::autorelay_state> observe_tracked_state();
   boost::asio::awaitable<forge::net::p2p::relay::reservation::info>
   reserve(manager::candidate source, std::uint64_t generation,
           std::shared_ptr<forge::net::p2p::cancellation_latch> cancellation);
   void release_all();

   forge::asio::runtime _runtime;
   forge::net::p2p::detail::lifecycle_tracker _tracker;
   std::shared_ptr<manager> _owner;
   mutable std::mutex _mutex;
   std::condition_variable _progress;
   forge::asio::notification _changed;
   manager::snapshot _snapshot;
   std::vector<std::shared_ptr<request>> _requests;
   std::function<void()> _after_snapshot;
   std::function<void()> _on_cancel;
   std::size_t _active = 0;
   std::size_t _peak_active = 0;
   bool _hold_canceled = false;
   bool _immediate_success = false;
   std::exception_ptr _snapshot_error;
};

} // namespace forge::tests::p2p
