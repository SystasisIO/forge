#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

// Include after the node, peer_store, runtime and native TCP modules.
namespace forge::tests::p2p {

class gossipsub_test_shutdown_fixture {
 public:
   class close_store final : public forge::net::p2p::peer_store::persistence {
    public:
      close_store();
      boost::asio::awaitable<forge::net::p2p::peer_store::hydration_page> async_hydrate(
          forge::net::p2p::peer_store::hydration_request) override;
      boost::asio::awaitable<forge::net::p2p::peer_store::apply_result> async_apply(
          forge::net::p2p::peer_store::mutation_batch) override;
      boost::asio::awaitable<forge::net::p2p::peer_store::prune_result> async_prune_expired(
          std::chrono::system_clock::time_point, std::size_t) override;
      boost::asio::awaitable<void> async_flush() override;
      boost::asio::awaitable<void> async_close() override;
      std::atomic_bool fail_next_close{false};
      std::atomic_size_t close_calls{0};

    private:
      std::shared_ptr<forge::net::p2p::peer_store::persistence> _delegate;
   };

   gossipsub_test_shutdown_fixture();
   ~gossipsub_test_shutdown_fixture() noexcept;
   void admit_cold_subscriptions();
   void release_workers() noexcept;
   void cancel_workers();
   void join_workers(std::chrono::steady_clock::time_point);
   [[nodiscard]] bool workers_ready() const;
#if defined(__unix__) || defined(__APPLE__)
   enum class child_mode { exhaustion, wait_until_killed, fail_after_spawn };
   struct child_result {
      int status = 0;
      bool timed_out = false;
      bool reaped = false;
   };
   [[nodiscard]] static child_result run_exhaustion_child(const std::string& executable,
       child_mode mode = child_mode::exhaustion, std::chrono::milliseconds wait_budget = std::chrono::seconds{10},
       child_result* exception_cleanup = nullptr);
#endif

   forge::asio::runtime runtime;
   std::shared_ptr<close_store> first_store;
   std::shared_ptr<close_store> second_store;
   forge::net::p2p::node first;
   forge::net::p2p::node second;
   std::vector<std::future<forge::net::p2p::pubsub::subscription>> workers;
   std::size_t barriers = 0;
   std::size_t cancellations = 0;
   bool barrier_before_stop = true;

 private:
   forge::net::tcp::listener _listener;
   forge::net::tcp::connection _accepted;
   std::future<forge::net::tcp::connection> _accepting;
   boost::asio::strand<boost::asio::io_context::executor_type> _worker_executor;
   std::shared_ptr<std::atomic_bool> _start;
   std::vector<std::shared_ptr<boost::asio::cancellation_signal>> _cancellations;
};

} // namespace forge::tests::p2p
