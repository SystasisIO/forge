#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <future>

// Include after forge.asio.runtime and forge.net.p2p.node.
namespace forge::tests::p2p {

class gossipsub_test_shutdown {
 public:
   gossipsub_test_shutdown(forge::asio::runtime&, forge::net::p2p::node&, forge::net::p2p::node&,
                            std::function<void()> release_workers = {},
                            std::function<void(std::chrono::steady_clock::time_point)> join_workers = {},
                            std::function<void()> cancel_workers = {},
                            std::chrono::milliseconds join_timeout = std::chrono::seconds{10});
   ~gossipsub_test_shutdown() noexcept;
   gossipsub_test_shutdown(const gossipsub_test_shutdown&) = delete;
   gossipsub_test_shutdown& operator=(const gossipsub_test_shutdown&) = delete;
   void join();
   static constexpr int incomplete_join_exit_code = 86;
   [[noreturn]] static void fail_closed() noexcept;

 private:
   [[nodiscard]] bool joined() const noexcept;
   forge::asio::runtime& _runtime;
   forge::net::p2p::node& _first;
   forge::net::p2p::node& _second;
   std::function<void()> _release_workers;
   std::function<void(std::chrono::steady_clock::time_point)> _join_workers;
   std::function<void()> _cancel_workers;
   std::chrono::milliseconds _join_timeout;
   std::array<std::future<void>, 2> _stops;
   std::array<bool, 2> _owners_joined{};
   bool _workers_released = false;
   bool _workers_joined = false;
};

} // namespace forge::tests::p2p
