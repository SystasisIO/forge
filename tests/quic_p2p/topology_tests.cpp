module;

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.multiformats.multiaddr;
import forge.multiformats.multihash;
import forge.multiformats.types;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.topology;
import forge.net.transport.endpoint;

#include "../../libraries/net/p2p/details/connection_manager.hxx"
#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/lifecycle_tracker.hxx"
#include "../../libraries/net/p2p/details/topology_manager.hxx"

namespace forge::net::p2p {
namespace {

[[nodiscard]] peer_id test_peer(std::uint8_t value) {
   const auto payload = forge::multiformats::bytes{value};
   return peer_id::from_bytes(forge::multiformats::multihash::identity(payload).encode());
}

[[nodiscard]] endpoint configured_rendezvous_endpoint(peer_id peer, std::uint16_t port = 4001) {
   return endpoint{
       .transport =
           forge::net::transport::endpoint{
               .host_type = endpoint::host_kind::ip4,
               .protocol = endpoint::protocol_kind::quic_v1,
               .host = "127.0.0.1",
               .port = port,
           },
       .peer = std::move(peer),
   };
}

[[nodiscard]] forge::multiformats::multiaddr discovered_rendezvous_address(peer_id peer, std::uint16_t port = 4001) {
   return configured_rendezvous_endpoint(std::move(peer), port).to_multiaddr();
}

[[nodiscard]] connection_manager test_connection_manager() {
   return connection_manager{connection_manager::policy{
       .max_sessions = 8,
       .low_watermark = 2,
       .grace_period = std::chrono::milliseconds{0},
       .prune_silence = std::chrono::milliseconds{1},
   }};
}

[[nodiscard]] topology::policy test_topology_policy() {
   auto policy = topology::policy{};
   policy.peers = topology::watermarks{.low = 1, .target = 2, .high = 3};
   policy.refresh_interval = std::chrono::hours{1};
   policy.query_timeout = std::chrono::seconds{1};
   policy.max_candidates = 8;
   policy.max_parallel_dials = 2;
   return policy;
}

[[nodiscard]] detail::topology_manager::callbacks topology_callbacks() {
   return detail::topology_manager::callbacks{
       .discover = [](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
          co_return std::vector<discovery::result>{};
       },
       .peer_exchange = [](std::shared_ptr<cancellation_latch>, std::size_t)
           -> boost::asio::awaitable<std::vector<discovery::result>> { co_return std::vector<discovery::result>{}; },
       .dial = [](discovery::result, std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
          co_return false;
       },
       .refresh_connection_scores = [] {},
       .sessions = [] { return connection_manager::snapshot{}; },
       .plan_peer_prune = [](std::size_t, std::size_t,
                             std::chrono::steady_clock::time_point) { return connection_manager::peer_prune_plan{}; },
       .close_sessions = [](std::vector<std::uint64_t>) -> boost::asio::awaitable<void> { co_return; },
   };
}

boost::asio::awaitable<bool> wait_for_dial_cancellation(std::shared_ptr<forge::asio::notification> canceled,
                                                        std::promise<void>* started,
                                                        std::shared_ptr<cancellation_latch> cancellation) {
   const auto observed = canceled->epoch();
   auto subscription = cancellation_latch::subscribe(cancellation, [canceled] noexcept { canceled->notify(); });
   static_cast<void>(subscription);
   started->set_value();
   static_cast<void>(co_await canceled->async_wait(observed));
   co_return false;
}

boost::asio::awaitable<bool> successful_topology_dial() {
   co_return true;
}

void stop_topology_manager(forge::asio::runtime& runtime, detail::topology_manager& manager,
                           detail::lifecycle_tracker& lifecycle) {
   manager.request_stop();
   forge::asio::blocking::run(runtime, manager.async_join());
   lifecycle.request_stop();
}

void remember(connection_manager& manager, std::uint64_t id, const peer_id& peer, double network_score,
              std::chrono::steady_clock::time_point opened_at, std::chrono::steady_clock::time_point last_used_at) {
   const auto admission = manager.remember(
       connection_manager::session_record{
           .id = id,
           .peer = peer,
           .opened_at = opened_at,
           .last_used_at = last_used_at,
           .network_score = network_score,
       },
       opened_at);
   BOOST_REQUIRE(admission.accepted);
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_topology_tests)

BOOST_AUTO_TEST_CASE(p2p_topology_policy_has_managed_bounded_defaults) {
   const auto policy = topology::policy{};

   BOOST_TEST(static_cast<int>(policy.operating_mode) == static_cast<int>(topology::mode::managed));
   BOOST_TEST(policy.peers.low == 128U);
   BOOST_TEST(policy.peers.target == 160U);
   BOOST_TEST(policy.peers.high == 192U);
   BOOST_TEST(policy.refresh_interval == std::chrono::seconds{600});
   BOOST_TEST(policy.query_timeout == std::chrono::seconds{10});
   BOOST_TEST(policy.max_candidates == 256U);
   BOOST_TEST(policy.max_parallel_queries == 10U);
   BOOST_TEST(policy.max_parallel_dials == 4U);
   BOOST_TEST(policy.max_rendezvous_points == 4U);
   BOOST_TEST(policy.max_rendezvous_namespaces == 16U);
   BOOST_TEST(policy.max_peer_exchange_peers == 4U);
   BOOST_TEST(policy.max_tagged_peers == 1024U);
   BOOST_TEST(policy.max_tags_per_peer == 16U);
   BOOST_TEST(policy.max_tag_size == 128U);
   BOOST_TEST(policy.retry_jitter == 0.20);
   BOOST_TEST(static_cast<std::uint16_t>(discovery::source::peer_exchange) == 4U);
   validate(policy);
}

BOOST_AUTO_TEST_CASE(p2p_topology_policy_rejects_invalid_sources_and_limits) {
   auto policy = topology::policy{};
   policy.peers.target = policy.peers.low - 1;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.max_parallel_dials = 0;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.max_rendezvous_namespaces = 0;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.max_tagged_peers = 0;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.max_tags_per_peer = 0;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.max_tag_size = 0;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.retry_jitter = 1.0;
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(1)),
           .namespaces = {"forge.content"},
       },
   };
   validate(policy);

   policy.rendezvous_points.front().namespaces.push_back("forge.content");
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.max_rendezvous_namespaces = 1;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(1)),
           .namespaces = {"forge.content", "forge.control"},
       },
   };
   BOOST_CHECK_THROW(validate(policy), exceptions::invalid_options);

   policy = topology::policy{};
   policy.operating_mode = topology::mode::static_only;
   validate(policy);
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(2)),
           .namespaces = {"forge.content"},
       },
   };
   validate(policy);
}

BOOST_AUTO_TEST_CASE(p2p_node_validates_configured_rendezvous_namespace_against_node_limits) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto options = node::options{
       .explicit_peer_id = test_peer(3),
       .allow_insecure_test_mode = true,
   };
   options.limits.rendezvous.max_namespace_size = 4;
   options.limits.topology.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(4)),
           .namespaces = {"forge"},
       },
   };
   BOOST_CHECK_THROW(static_cast<void>(node{runtime, std::move(options)}), exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_node_normalizes_managed_topology_to_hard_session_capacity_only) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto managed_options = node::options{
       .explicit_peer_id = test_peer(5),
       .allow_insecure_test_mode = true,
   };
   managed_options.limits.max_sessions = 1;
   managed_options.limits.session_low_watermark = 1;
   auto managed = node{runtime, std::move(managed_options)};
   const auto managed_topology = managed.diagnostics().topology;
   BOOST_TEST(managed_topology.low_watermark == 1U);
   BOOST_TEST(managed_topology.target_watermark == 1U);
   BOOST_TEST(managed_topology.high_watermark == topology::watermarks{}.high);

   auto static_options = node::options{
       .explicit_peer_id = test_peer(6),
       .allow_insecure_test_mode = true,
   };
   static_options.limits.topology.operating_mode = topology::mode::static_only;
   static_options.limits.max_sessions = 1;
   static_options.limits.session_low_watermark = 1;
   auto static_node = node{runtime, std::move(static_options)};
   const auto static_topology = static_node.diagnostics().topology;
   BOOST_TEST(static_topology.low_watermark == topology::watermarks{}.low);
   BOOST_TEST(static_topology.target_watermark == topology::watermarks{}.target);
   BOOST_TEST(static_topology.high_watermark == topology::watermarks{}.high);

   forge::asio::blocking::run(runtime, static_node.async_stop());
   forge::asio::blocking::run(runtime, managed.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_coalesces_refresh_waiters) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto started = std::promise<void>{};
   auto calls = std::atomic_size_t{0};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release, &started,
        &calls](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      const auto observed = release->epoch();
      ++calls;
      started.set_value();
      static_cast<void>(co_await release->async_wait(observed));
      co_return std::vector<discovery::result>{};
   };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks));
   manager->start(lifecycle);

   auto first = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   started.get_future().wait();
   auto second = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   const auto waiter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (manager->current().waiting_refreshes < 2 && std::chrono::steady_clock::now() < waiter_deadline) {
      std::this_thread::yield();
   }
   BOOST_REQUIRE(manager->current().waiting_refreshes == 2U);
   release->notify();
   static_cast<void>(first.get());
   static_cast<void>(second.get());
   BOOST_TEST(calls.load() == 1U);

   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_completion_publication_survives_failpoint) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto started = std::promise<void>{};
   auto fail_refresh_completion = std::atomic_bool{true};
   auto fail_parent_completion = std::atomic_bool{true};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release,
        &started](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      const auto observed = release->epoch();
      started.set_value();
      static_cast<void>(co_await release->async_wait(observed));
      co_return std::vector<discovery::result>{};
   };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks),
                                                             detail::topology_manager::clocks{
                                                                 .before_refresh_completion =
                                                                     [&] {
                                                                        if (fail_refresh_completion.exchange(false)) {
                                                                           throw std::bad_alloc{};
                                                                        }
                                                                     },
                                                                 .before_parent_completion =
                                                                     [&] {
                                                                        if (fail_parent_completion.exchange(false)) {
                                                                           throw std::bad_alloc{};
                                                                        }
                                                                     },
                                                             });
   manager->start(lifecycle);
   started.get_future().wait();

   auto first = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   auto second = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   release->notify();
   static_cast<void>(first.get());
   static_cast<void>(second.get());

   manager->request_stop();
   forge::asio::blocking::run(runtime, manager->async_join());
   BOOST_TEST(!fail_refresh_completion.load());
   BOOST_TEST(!fail_parent_completion.load());
   BOOST_TEST(static_cast<int>(manager->current().lifecycle_phase) ==
              static_cast<int>(detail::topology_manager::phase::stopped));
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_coalesces_and_repeats_peer_exchange_batches) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto entered = std::promise<void>{};
   auto calls = std::atomic_size_t{0};
   auto maximum_parallel = std::atomic_size_t{0};
   auto callbacks = topology_callbacks();
   callbacks.peer_exchange = [release, &entered, &calls, &maximum_parallel](
                                 std::shared_ptr<cancellation_latch>,
                                 std::size_t max_parallel) -> boost::asio::awaitable<std::vector<discovery::result>> {
      maximum_parallel.store(max_parallel);
      if (calls.fetch_add(1) == 0) {
         const auto observed = release->epoch();
         entered.set_value();
         static_cast<void>(co_await release->async_wait(observed));
      }
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(71),
                            .endpoints = {discovered_rendezvous_address(test_peer(71), 4071)},
                            .discovered_by = discovery::source::peer_exchange,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                            .score = 1.0},
      };
   };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks));
   manager->start(lifecycle);
   entered.get_future().wait();

   auto first = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   auto second = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   release->notify();
   const auto first_results = first.get();
   const auto second_results = second.get();
   BOOST_TEST(calls.load() == 1U);
   BOOST_TEST(maximum_parallel.load() == 4U);
   BOOST_REQUIRE_EQUAL(first_results.size(), 1U);
   BOOST_REQUIRE_EQUAL(second_results.size(), 1U);
   BOOST_TEST(static_cast<int>(first_results.front().discovered_by) ==
              static_cast<int>(discovery::source::peer_exchange));

   const auto repeated = forge::asio::blocking::run(runtime, manager->async_refresh());
   BOOST_TEST(calls.load() == 2U);
   BOOST_REQUIRE_EQUAL(repeated.size(), 1U);
   BOOST_TEST(static_cast<int>(repeated.front().discovered_by) == static_cast<int>(discovery::source::peer_exchange));
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_static_mode_does_no_autonomous_work) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   auto policy = test_topology_policy();
   policy.operating_mode = topology::mode::static_only;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(31)),
           .namespaces = {"forge.static"},
       },
   };
   auto calls = std::size_t{};
   auto peer_exchange_calls = std::size_t{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [&calls](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      ++calls;
      co_return std::vector<discovery::result>{};
   };
   callbacks.peer_exchange =
       [&peer_exchange_calls](std::shared_ptr<cancellation_latch>,
                              std::size_t) -> boost::asio::awaitable<std::vector<discovery::result>> {
      ++peer_exchange_calls;
      co_return std::vector<discovery::result>{};
   };
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks));
   manager->start(lifecycle);

   const auto results = forge::asio::blocking::run(runtime, manager->async_refresh());
   BOOST_TEST(results.empty());
   BOOST_TEST(calls == 0U);
   BOOST_TEST(peer_exchange_calls == 0U);
   BOOST_TEST(static_cast<int>(manager->current().lifecycle_phase) ==
              static_cast<int>(detail::topology_manager::phase::idle));
   manager->request_stop();
   forge::asio::blocking::run(runtime, manager->async_join());
   BOOST_TEST(static_cast<int>(manager->current().lifecycle_phase) ==
              static_cast<int>(detail::topology_manager::phase::stopped));
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_renews_confirmed_rendezvous_before_periodic_refresh) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto steady_now = std::chrono::steady_clock::now();
   auto system_now = std::chrono::system_clock::time_point{std::chrono::hours{300}};
   auto policy = test_topology_policy();
   policy.dht_enabled = false;
   policy.peer_exchange_enabled = false;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(32)),
           .namespaces = {"forge.renewal"},
       },
   };
   const auto periodic_refresh = policy.refresh_interval;
   auto registrations = std::size_t{};
   auto idle_waits = std::size_t{};
   auto manager_value = static_cast<detail::topology_manager*>(nullptr);
   auto callbacks = topology_callbacks();
   callbacks.local_rendezvous_record = [] {
      return detail::topology_manager::callbacks::rendezvous_local_record{
          .generation = 1,
          .signed_peer_record = {0x01},
      };
   };
   callbacks.rendezvous_register = [&registrations](std::size_t, std::string, std::vector<std::uint8_t>,
                                                    std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_register_result> {
      ++registrations;
      co_return detail::topology_manager::callbacks::rendezvous_register_result{
          .accepted = true,
          .ttl = std::chrono::seconds{3},
      };
   };
   callbacks.rendezvous_discover = [](std::size_t, std::string, std::size_t, std::vector<std::uint8_t>,
                                      std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_discover_result> {
      co_return detail::topology_manager::callbacks::rendezvous_discover_result{
          .response_status = detail::topology_manager::callbacks::rendezvous_discover_result::status::ok,
      };
   };
   auto manager = std::make_shared<detail::topology_manager>(
       std::move(policy), std::move(callbacks),
       detail::topology_manager::clocks{
           .steady_now = [&steady_now] { return steady_now; },
           .system_now = [&system_now] { return system_now; },
           .idle_wait = [&](std::chrono::steady_clock::time_point deadline) -> boost::asio::awaitable<void> {
              ++idle_waits;
              if (idle_waits == 1) {
                 const auto elapsed = deadline - steady_now;
                 BOOST_TEST(elapsed < periodic_refresh);
                 steady_now = deadline;
                 system_now += std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
              } else {
                 manager_value->request_stop();
              }
              co_return;
           },
       });
   manager_value = manager.get();
   manager->start(lifecycle);
   forge::asio::blocking::run(runtime, manager->async_join());

   BOOST_TEST(registrations == 2U);
   BOOST_TEST(idle_waits >= 2U);
   BOOST_TEST(static_cast<int>(manager->current().lifecycle_phase) ==
              static_cast<int>(detail::topology_manager::phase::stopped));
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_keeps_jittered_periodic_deadline_across_early_wakes) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto steady_now = std::chrono::steady_clock::now();
   auto system_now = std::chrono::system_clock::time_point{std::chrono::hours{400}};
   auto policy = test_topology_policy();
   policy.refresh_interval = std::chrono::seconds{10};
   policy.retry_jitter = 0.20;
   policy.peer_exchange_enabled = false;
   policy.rendezvous_enabled = false;
   auto refreshes = std::size_t{};
   auto idle_waits = std::size_t{};
   auto first_deadline = std::chrono::steady_clock::time_point{};
   auto manager_value = static_cast<detail::topology_manager*>(nullptr);
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [&refreshes](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      ++refreshes;
      co_return std::vector<discovery::result>{};
   };
   auto manager = std::make_shared<detail::topology_manager>(
       std::move(policy), std::move(callbacks),
       detail::topology_manager::clocks{
           .steady_now = [&steady_now] { return steady_now; },
           .system_now = [&system_now] { return system_now; },
           .idle_wait = [&](std::chrono::steady_clock::time_point deadline) -> boost::asio::awaitable<void> {
              ++idle_waits;
              if (idle_waits == 1) {
                 first_deadline = deadline;
                 const auto delay = deadline - steady_now;
                 BOOST_CHECK(delay >= std::chrono::seconds{8});
                 BOOST_CHECK(delay <= std::chrono::seconds{12});
              } else if (idle_waits == 2) {
                 BOOST_CHECK(deadline == first_deadline);
                 const auto elapsed = deadline - steady_now;
                 steady_now = deadline;
                 system_now += std::chrono::duration_cast<std::chrono::system_clock::duration>(elapsed);
              } else {
                 manager_value->request_stop();
              }
              co_return;
           },
       });
   manager_value = manager.get();
   manager->start(lifecycle);
   forge::asio::blocking::run(runtime, manager->async_join());

   BOOST_TEST(refreshes == 2U);
   BOOST_TEST(idle_waits >= 3U);
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_jitters_first_periodic_deadline_by_node_seed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto steady_now = std::chrono::steady_clock::time_point{std::chrono::seconds{100}};
   const auto system_now = std::chrono::system_clock::time_point{std::chrono::hours{400}};
   auto policy = test_topology_policy();
   policy.refresh_interval = std::chrono::seconds{10};
   policy.retry_jitter = 0.20;
   policy.peer_exchange_enabled = false;
   policy.rendezvous_enabled = false;

   const auto first_deadline_for = [&](std::uint64_t seed) {
      auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
      BOOST_TEST(lifecycle.begin_start());
      auto deadline = std::chrono::steady_clock::time_point{};
      auto manager_value = static_cast<detail::topology_manager*>(nullptr);
      auto manager = std::make_shared<detail::topology_manager>(
          policy, topology_callbacks(),
          detail::topology_manager::clocks{
              .steady_now = [&] { return steady_now; },
              .system_now = [&] { return system_now; },
              .idle_wait = [&](std::chrono::steady_clock::time_point candidate) -> boost::asio::awaitable<void> {
                 deadline = candidate;
                 manager_value->request_stop();
                 co_return;
              },
          },
          seed);
      manager_value = manager.get();
      manager->start(lifecycle);
      forge::asio::blocking::run(runtime, manager->async_join());
      lifecycle.request_stop();
      BOOST_TEST(deadline.time_since_epoch().count() != 0);
      const auto delay = deadline - steady_now;
      BOOST_CHECK(delay >= std::chrono::seconds{8});
      BOOST_CHECK(delay <= std::chrono::seconds{12});
      return deadline;
   };

   const auto first = first_deadline_for(0);
   const auto repeated = first_deadline_for(0);
   const auto different = first_deadline_for(10'000);

   BOOST_TEST(first.time_since_epoch().count() == repeated.time_since_epoch().count());
   BOOST_TEST(first.time_since_epoch().count() != different.time_since_epoch().count());
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_saturates_maximum_periodic_deadline) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   const auto steady_now = std::chrono::steady_clock::time_point{std::chrono::milliseconds{1}};
   const auto system_now = std::chrono::system_clock::time_point{std::chrono::hours{500}};
   auto policy = test_topology_policy();
   policy.refresh_interval = (std::chrono::milliseconds::max)();
   policy.retry_jitter = 0.0;
   policy.dht_enabled = false;
   policy.peer_exchange_enabled = false;
   policy.rendezvous_enabled = false;
   auto deadline = std::chrono::steady_clock::time_point{};
   auto manager_value = static_cast<detail::topology_manager*>(nullptr);
   auto manager = std::make_shared<detail::topology_manager>(
       std::move(policy), topology_callbacks(),
       detail::topology_manager::clocks{
           .steady_now = [&] { return steady_now; },
           .system_now = [&] { return system_now; },
           .idle_wait = [&](std::chrono::steady_clock::time_point candidate) -> boost::asio::awaitable<void> {
              deadline = candidate;
              manager_value->request_stop();
              co_return;
           },
       });
   manager_value = manager.get();
   manager->start(lifecycle);
   forge::asio::blocking::run(runtime, manager->async_join());

   BOOST_TEST(deadline.time_since_epoch().count() ==
              std::chrono::steady_clock::time_point::max().time_since_epoch().count());
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_saturates_dial_retry_deadline_near_steady_clock_maximum) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   const auto steady_now = std::chrono::steady_clock::time_point::max() - std::chrono::milliseconds{1};
   const auto system_now = std::chrono::system_clock::time_point{std::chrono::hours{500}};
   auto policy = test_topology_policy();
   policy.refresh_interval = (std::chrono::milliseconds::max)();
   policy.retry_jitter = 0.0;
   policy.peer_exchange_enabled = false;
   policy.rendezvous_enabled = false;
   auto dials = std::size_t{};
   auto deadline = std::chrono::steady_clock::time_point{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      co_return std::vector<discovery::result>{
          discovery::result{
              .peer = test_peer(98),
              .endpoints = {discovered_rendezvous_address(test_peer(98), 4098)},
              .discovered_by = discovery::source::dht,
              .score = 1.0,
          },
      };
   };
   callbacks.dial = [&dials](discovery::result, std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
      ++dials;
      co_return false;
   };
   auto manager_value = static_cast<detail::topology_manager*>(nullptr);
   auto manager = std::make_shared<detail::topology_manager>(
       std::move(policy), std::move(callbacks),
       detail::topology_manager::clocks{
           .steady_now = [&] { return steady_now; },
           .system_now = [&] { return system_now; },
           .idle_wait = [&](std::chrono::steady_clock::time_point candidate) -> boost::asio::awaitable<void> {
              deadline = candidate;
              manager_value->request_stop();
              co_return;
           },
       });
   manager_value = manager.get();
   manager->start(lifecycle);
   forge::asio::blocking::run(runtime, manager->async_join());

   BOOST_TEST(dials == 1U);
   BOOST_TEST(deadline.time_since_epoch().count() ==
              std::chrono::steady_clock::time_point::max().time_since_epoch().count());
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_saturates_maximum_discovery_expiry) {
   const auto now = std::chrono::system_clock::time_point{std::chrono::milliseconds{1}};
   const auto expiry = detail::saturating_topology_expiry(now, (std::chrono::milliseconds::max)());

   BOOST_TEST(expiry.time_since_epoch().count() ==
              std::chrono::system_clock::time_point::max().time_since_epoch().count());
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_bounds_sources_and_prevents_peer_exchange_starvation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto entered = std::promise<void>{};
   auto block_initial = std::make_shared<std::atomic_bool>(true);
   auto rendezvous_limit = std::size_t{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release, &entered,
        block_initial](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      if (block_initial->exchange(false)) {
         const auto observed = release->epoch();
         entered.set_value();
         static_cast<void>(co_await release->async_wait(observed));
      }
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(41),
                            .endpoints = {discovered_rendezvous_address(test_peer(41), 4041)},
                            .discovered_by = discovery::source::dht,
                            .score = 1.0},
          discovery::result{.peer = test_peer(42),
                            .endpoints = {discovered_rendezvous_address(test_peer(42), 4042)},
                            .discovered_by = discovery::source::dht,
                            .score = 2.0},
      };
   };
   callbacks.local_rendezvous_record = [] {
      return detail::topology_manager::callbacks::rendezvous_local_record{
          .generation = 1,
          .signed_peer_record = {0x01},
      };
   };
   callbacks.rendezvous_register = [](std::size_t, std::string, std::vector<std::uint8_t>,
                                      std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_register_result> {
      co_return detail::topology_manager::callbacks::rendezvous_register_result{
          .accepted = true,
          .ttl = std::chrono::minutes{1},
      };
   };
   callbacks.rendezvous_discover = [&rendezvous_limit](std::size_t, std::string, std::size_t limit,
                                                       std::vector<std::uint8_t>, std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_discover_result> {
      rendezvous_limit = limit;
      co_return detail::topology_manager::callbacks::rendezvous_discover_result{
          .response_status = detail::topology_manager::callbacks::rendezvous_discover_result::status::ok,
          .results =
              {
                  discovery::result{.peer = test_peer(43),
                                    .endpoints = {discovered_rendezvous_address(test_peer(43), 4043)},
                                    .discovered_by = discovery::source::rendezvous,
                                    .score = 1.0},
                  discovery::result{.peer = test_peer(44),
                                    .endpoints = {discovered_rendezvous_address(test_peer(44), 4044)},
                                    .discovered_by = discovery::source::rendezvous,
                                    .score = 2.0},
              },
      };
   };
   callbacks.peer_exchange = [](std::shared_ptr<cancellation_latch>,
                                std::size_t) -> boost::asio::awaitable<std::vector<discovery::result>> {
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(45),
                            .endpoints = {discovered_rendezvous_address(test_peer(45), 4045)},
                            .discovered_by = discovery::source::peer_exchange,
                            .score = 1.0},
          discovery::result{.peer = test_peer(46),
                            .endpoints = {discovered_rendezvous_address(test_peer(46), 4046)},
                            .discovered_by = discovery::source::peer_exchange,
                            .score = 2.0},
          discovery::result{.peer = test_peer(47),
                            .endpoints = {discovered_rendezvous_address(test_peer(47), 4047)},
                            .discovered_by = discovery::source::peer_exchange,
                            .score = 3.0},
          discovery::result{.peer = test_peer(48),
                            .endpoints = {discovered_rendezvous_address(test_peer(48), 4048)},
                            .discovered_by = discovery::source::peer_exchange,
                            .score = 4.0},
      };
   };
   auto policy = test_topology_policy();
   policy.max_candidates = 1;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(40)),
           .namespaces = {"forge.sources"},
       },
   };
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks));
   manager->start(lifecycle);
   entered.get_future().wait();
   auto refresh = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   release->notify();
   const auto dht_results = refresh.get();
   const auto rendezvous_results = forge::asio::blocking::run(runtime, manager->async_refresh());
   const auto peer_exchange_results = forge::asio::blocking::run(runtime, manager->async_refresh());

   BOOST_REQUIRE_EQUAL(dht_results.size(), 1U);
   BOOST_REQUIRE_EQUAL(rendezvous_results.size(), 1U);
   BOOST_REQUIRE_EQUAL(peer_exchange_results.size(), 1U);
   BOOST_TEST(rendezvous_limit == 1U);
   BOOST_TEST(static_cast<std::uint16_t>(dht_results.front().discovered_by) ==
              static_cast<std::uint16_t>(discovery::source::dht));
   BOOST_TEST(static_cast<std::uint16_t>(rendezvous_results.front().discovered_by) ==
              static_cast<std::uint16_t>(discovery::source::rendezvous));
   BOOST_TEST(static_cast<std::uint16_t>(peer_exchange_results.front().discovered_by) ==
              static_cast<std::uint16_t>(discovery::source::peer_exchange));
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_source_failure_keeps_other_results_and_reconciliation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto entered = std::promise<void>{};
   auto dialed = std::vector<peer_id>{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release,
        &entered](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      const auto observed = release->epoch();
      entered.set_value();
      static_cast<void>(co_await release->async_wait(observed));
      throw std::runtime_error{"DHT source failure"};
   };
   callbacks.local_rendezvous_record = [] {
      return detail::topology_manager::callbacks::rendezvous_local_record{
          .generation = 1,
          .signed_peer_record = {0x01},
      };
   };
   callbacks.rendezvous_register = [](std::size_t, std::string, std::vector<std::uint8_t>,
                                      std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_register_result> {
      co_return detail::topology_manager::callbacks::rendezvous_register_result{
          .accepted = true,
          .ttl = std::chrono::minutes{1},
      };
   };
   callbacks.rendezvous_discover = [](std::size_t, std::string, std::size_t, std::vector<std::uint8_t>,
                                      std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_discover_result> {
      co_return detail::topology_manager::callbacks::rendezvous_discover_result{
          .response_status = detail::topology_manager::callbacks::rendezvous_discover_result::status::ok,
          .results =
              {
                  discovery::result{.peer = test_peer(51),
                                    .endpoints = {discovered_rendezvous_address(test_peer(51), 4051)},
                                    .discovered_by = discovery::source::rendezvous,
                                    .score = 1.0},
              },
      };
   };
   callbacks.peer_exchange = [](std::shared_ptr<cancellation_latch>,
                                std::size_t) -> boost::asio::awaitable<std::vector<discovery::result>> {
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(52),
                            .endpoints = {discovered_rendezvous_address(test_peer(52), 4052)},
                            .discovered_by = discovery::source::peer_exchange,
                            .score = 1.0},
      };
   };
   callbacks.sessions = [] { return connection_manager::snapshot{}; };
   callbacks.dial = [&dialed](discovery::result value,
                              std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
      dialed.push_back(value.peer);
      co_return false;
   };
   auto policy = test_topology_policy();
   policy.max_candidates = 3;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(50)),
           .namespaces = {"forge.partial"},
       },
   };
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks));
   manager->start(lifecycle);
   entered.get_future().wait();
   auto refresh = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   release->notify();
   const auto results = refresh.get();

   BOOST_REQUIRE_EQUAL(results.size(), 2U);
   BOOST_TEST(std::ranges::all_of(
       results, [](const discovery::result& value) { return value.discovered_by != discovery::source::dht; }));
   BOOST_REQUIRE_EQUAL(dialed.size(), 2U);
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_all_enabled_source_failures_fail_manual_refresh) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto started = std::promise<void>{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release,
        &started](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      const auto observed = release->epoch();
      started.set_value();
      static_cast<void>(co_await release->async_wait(observed));
      throw exceptions::timeout{"DHT discovery failed"};
   };
   callbacks.local_rendezvous_record = [] {
      return detail::topology_manager::callbacks::rendezvous_local_record{
          .generation = 1,
          .signed_peer_record = {0x01},
      };
   };
   callbacks.rendezvous_register = [](std::size_t, std::string, std::vector<std::uint8_t>,
                                      std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_register_result> {
      throw exceptions::internal{"Rendezvous registration failed"};
   };
   callbacks.rendezvous_discover = [](std::size_t, std::string, std::size_t, std::vector<std::uint8_t>,
                                      std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_discover_result> {
      throw exceptions::internal{"Rendezvous discovery failed"};
   };
   callbacks.peer_exchange = [](std::shared_ptr<cancellation_latch>,
                                std::size_t) -> boost::asio::awaitable<std::vector<discovery::result>> {
      throw exceptions::internal{"peer exchange discovery failed"};
   };
   auto policy = test_topology_policy();
   policy.peer_exchange_enabled = true;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(53)),
           .namespaces = {"forge.all-failed"},
       },
   };
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks));
   manager->start(lifecycle);
   started.get_future().wait();

   auto first = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   auto second = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   release->notify();

   BOOST_CHECK_THROW(static_cast<void>(first.get()), exceptions::timeout);
   BOOST_CHECK_THROW(static_cast<void>(second.get()), exceptions::timeout);
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_canceled_waiter_does_not_cancel_shared_refresh) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto started = std::promise<void>{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release,
        &started](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      const auto observed = release->epoch();
      started.set_value();
      static_cast<void>(co_await release->async_wait(observed));
      co_return std::vector<discovery::result>{};
   };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks));
   manager->start(lifecycle);
   started.get_future().wait();

   auto cancellation = boost::asio::cancellation_signal{};
   auto canceled =
       boost::asio::co_spawn(runtime.context(), manager->async_refresh(),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   BOOST_REQUIRE(canceled.wait_for(std::chrono::milliseconds{10}) == std::future_status::timeout);
   cancellation.emit(boost::asio::cancellation_type::terminal);
   BOOST_CHECK_THROW(static_cast<void>(canceled.get()), boost::system::system_error);

   auto remaining = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   release->notify();
   static_cast<void>(remaining.get());
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_stop_before_idle_wait_is_not_lost) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto policy = test_topology_policy();
   policy.refresh_interval = std::chrono::hours{10};
   auto reached_idle_wait = std::promise<void>{};
   auto manager_value = static_cast<detail::topology_manager*>(nullptr);
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), topology_callbacks(),
                                                             detail::topology_manager::clocks{
                                                                 .before_idle_wait =
                                                                     [&] {
                                                                        manager_value->request_stop();
                                                                        reached_idle_wait.set_value();
                                                                     },
                                                             });
   manager_value = manager.get();
   manager->start(lifecycle);

   BOOST_REQUIRE(reached_idle_wait.get_future().wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   auto joined = boost::asio::co_spawn(runtime.context(), manager->async_join(), boost::asio::use_future);
   BOOST_REQUIRE(joined.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   joined.get();
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_bounds_observations_and_evicts_lowest_score) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto started = std::promise<void>{};
   auto rounds = std::size_t{};
   auto active_peers = std::size_t{2};
   auto dialed = std::vector<peer_id>{};
   auto policy = test_topology_policy();
   policy.max_candidates = 2;
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release, &started,
        &rounds](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      if (rounds++ == 0) {
         const auto observed = release->epoch();
         started.set_value();
         static_cast<void>(co_await release->async_wait(observed));
         co_return std::vector<discovery::result>{
             discovery::result{.peer = test_peer(81),
                               .endpoints = {discovered_rendezvous_address(test_peer(81), 4081)},
                               .discovered_by = discovery::source::dht,
                               .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                               .score = 1.0},
             discovery::result{.peer = test_peer(82),
                               .endpoints = {discovered_rendezvous_address(test_peer(82), 4082)},
                               .discovered_by = discovery::source::dht,
                               .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                               .score = 2.0},
         };
      }
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(83),
                            .endpoints = {discovered_rendezvous_address(test_peer(83), 4083)},
                            .discovered_by = discovery::source::rendezvous,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                            .score = 100.0},
      };
   };
   callbacks.sessions = [&active_peers] { return connection_manager::snapshot{.active_peers = active_peers}; };
   callbacks.dial = [&dialed](discovery::result result,
                              std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
      dialed.push_back(result.peer);
      co_return false;
   };
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks));
   manager->start(lifecycle);
   started.get_future().wait();
   auto initial = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   release->notify();
   static_cast<void>(initial.get());

   active_peers = 0;
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_REQUIRE_EQUAL(dialed.size(), 2U);
   BOOST_TEST(static_cast<bool>(std::find(dialed.begin(), dialed.end(), test_peer(83)) != dialed.end()));
   BOOST_TEST(static_cast<bool>(std::find(dialed.begin(), dialed.end(), test_peer(82)) != dialed.end()));
   BOOST_TEST(static_cast<bool>(std::find(dialed.begin(), dialed.end(), test_peer(81)) == dialed.end()));
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_dials_to_target_and_prunes_to_target) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto connected = std::size_t{};
   auto dials = std::vector<peer_id>{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(41),
                            .endpoints = {discovered_rendezvous_address(test_peer(41), 4041)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                            .score = 10.0},
          discovery::result{.peer = test_peer(42),
                            .endpoints = {discovered_rendezvous_address(test_peer(42), 4042)},
                            .discovered_by = discovery::source::rendezvous,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                            .score = 5.0},
          discovery::result{.peer = test_peer(43),
                            .endpoints = {discovered_rendezvous_address(test_peer(43), 4043)},
                            .discovered_by = discovery::source::rendezvous,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                            .score = 4.0},
      };
   };
   callbacks.dial = [&connected, &dials](discovery::result result,
                                         std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
      dials.push_back(result.peer);
      if (result.peer == test_peer(41)) {
         co_return false;
      }
      ++connected;
      co_return true;
   };
   callbacks.sessions = [&connected] { return connection_manager::snapshot{.active_peers = connected}; };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks));
   manager->start(lifecycle);
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(connected == 2U);
   BOOST_TEST(dials.size() == 3U);
   stop_topology_manager(runtime, *manager, lifecycle);

   auto prune_lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(prune_lifecycle.begin_start());
   auto pruned = std::vector<std::uint64_t>{};
   auto requested_target = std::size_t{};
   auto requested_maximum = std::size_t{};
   auto prune_callbacks = topology_callbacks();
   prune_callbacks.sessions = [] { return connection_manager::snapshot{.active_peers = 4}; };
   prune_callbacks.plan_peer_prune = [&requested_target, &requested_maximum](std::size_t target, std::size_t maximum,
                                                                             std::chrono::steady_clock::time_point) {
      requested_target = target;
      requested_maximum = maximum;
      return connection_manager::peer_prune_plan{
          .connected_peers = 4,
          .victim_peers = {test_peer(51), test_peer(52)},
          .session_ids = {501, 502, 503},
      };
   };
   prune_callbacks.close_sessions = [&pruned](std::vector<std::uint64_t> session_ids) -> boost::asio::awaitable<void> {
      pruned = std::move(session_ids);
      co_return;
   };
   auto prune_manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(prune_callbacks));
   prune_manager->start(prune_lifecycle);
   static_cast<void>(forge::asio::blocking::run(runtime, prune_manager->async_refresh()));
   BOOST_TEST(requested_target == 2U);
   BOOST_TEST(requested_maximum == 2U);
   BOOST_REQUIRE_EQUAL(pruned.size(), 3U);
   BOOST_TEST(pruned[0] == 501U);
   BOOST_TEST(pruned[1] == 502U);
   BOOST_TEST(pruned[2] == 503U);
   stop_topology_manager(runtime, *prune_manager, prune_lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_reserves_target_slots_across_parallel_dials) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release_slow = std::make_shared<forge::asio::notification>();
   auto slow_started = std::promise<void>{};
   auto fast_worker_completed = std::promise<void>{};
   auto completion_reported = std::atomic_bool{false};
   auto third_started = std::atomic_bool{false};
   auto dial_calls = std::atomic_size_t{0};
   auto connected = std::atomic_size_t{0};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(61),
                            .endpoints = {discovered_rendezvous_address(test_peer(61), 4061)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1}},
          discovery::result{.peer = test_peer(62),
                            .endpoints = {discovered_rendezvous_address(test_peer(62), 4062)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1}},
          discovery::result{.peer = test_peer(63),
                            .endpoints = {discovered_rendezvous_address(test_peer(63), 4063)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1}},
      };
   };
   callbacks.sessions = [&connected] {
      return connection_manager::snapshot{.active_peers = connected.load(std::memory_order_acquire)};
   };
   callbacks.dial = [release_slow, &slow_started, &third_started, &dial_calls, &connected](
                        discovery::result, std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
      const auto call = dial_calls.fetch_add(1, std::memory_order_acq_rel);
      if (call == 0) {
         const auto observed = release_slow->epoch();
         slow_started.set_value();
         static_cast<void>(co_await release_slow->async_wait(observed));
      } else if (call == 2) {
         third_started.store(true, std::memory_order_release);
      }
      connected.fetch_add(1, std::memory_order_acq_rel);
      co_return true;
   };
   auto manager = std::make_shared<detail::topology_manager>(
       test_topology_policy(), std::move(callbacks),
       detail::topology_manager::clocks{
           .before_dial_worker_completion =
               [&] {
                  if (!completion_reported.exchange(true, std::memory_order_acq_rel)) {
                     fast_worker_completed.set_value();
                  }
               },
       });
   manager->start(lifecycle);
   auto refreshed = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   BOOST_REQUIRE(slow_started.get_future().wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_REQUIRE(fast_worker_completed.get_future().wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_TEST(dial_calls.load(std::memory_order_acquire) == 2U);
   BOOST_TEST(!third_started.load(std::memory_order_acquire));
   release_slow->notify();
   BOOST_REQUIRE(refreshed.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   static_cast<void>(refreshed.get());
   BOOST_TEST(dial_calls.load(std::memory_order_acquire) == 2U);
   BOOST_TEST(connected.load(std::memory_order_acquire) == 2U);
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_closes_admission_after_synchronous_dial_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto first_round = std::atomic_bool{true};
   auto first_round_calls = std::atomic_size_t{0};
   auto unexpected_admission = std::atomic_bool{false};
   auto blocked_started = std::promise<void>{};
   auto blocked_started_future = blocked_started.get_future().share();
   auto blocked_canceled = std::make_shared<forge::asio::notification>();
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(64),
                            .endpoints = {discovered_rendezvous_address(test_peer(64), 4064)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1}},
          discovery::result{.peer = test_peer(65),
                            .endpoints = {discovered_rendezvous_address(test_peer(65), 4065)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1}},
          discovery::result{.peer = test_peer(66),
                            .endpoints = {discovered_rendezvous_address(test_peer(66), 4066)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1}},
      };
   };
   callbacks.dial =
       [blocked_canceled, blocked_started_future, &blocked_started, &first_round, &first_round_calls,
        &unexpected_admission](discovery::result,
                               std::shared_ptr<cancellation_latch> cancellation) -> boost::asio::awaitable<bool> {
      if (!first_round.load(std::memory_order_acquire)) {
         return successful_topology_dial();
      }
      const auto call = first_round_calls.fetch_add(1, std::memory_order_acq_rel);
      if (call == 0) {
         return wait_for_dial_cancellation(blocked_canceled, &blocked_started, std::move(cancellation));
      }
      if (call == 1) {
         blocked_started_future.wait();
         throw std::bad_alloc{};
      }
      unexpected_admission.store(true, std::memory_order_release);
      return successful_topology_dial();
   };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks));
   manager->start(lifecycle);

   auto failed = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   BOOST_REQUIRE(failed.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(failed.get()), std::bad_alloc);
   BOOST_TEST(manager->current().active_operations == 0U);
   BOOST_TEST(first_round_calls.load(std::memory_order_acquire) == 2U);
   BOOST_TEST(!unexpected_admission.load(std::memory_order_acquire));

   first_round.store(false, std::memory_order_release);
   BOOST_CHECK_NO_THROW(static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh())));
   BOOST_TEST(manager->current().active_operations == 0U);
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_dial_join_failure_cancels_and_drains_before_next_refresh) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto cancellation_seen = std::make_shared<forge::asio::notification>();
   auto first_worker_finished = std::make_shared<forge::asio::notification>();
   auto dial_started = std::promise<void>{};
   auto dial_started_future = dial_started.get_future().share();
   auto fail_join = std::atomic_bool{true};
   auto dial_calls = std::atomic_size_t{0};
   auto active_dials = std::atomic_size_t{0};
   auto maximum_active_dials = std::atomic_size_t{0};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [](std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(44),
                            .endpoints = {discovered_rendezvous_address(test_peer(44), 4144)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                            .score = 2.0},
          discovery::result{.peer = test_peer(45),
                            .endpoints = {discovered_rendezvous_address(test_peer(45), 4145)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = std::chrono::system_clock::now() + std::chrono::hours{1},
                            .score = 1.0},
      };
   };
   callbacks.dial =
       [release, cancellation_seen, first_worker_finished, &dial_started, &dial_calls, &active_dials,
        &maximum_active_dials](discovery::result,
                               std::shared_ptr<cancellation_latch> cancellation) -> boost::asio::awaitable<bool> {
      const auto call = dial_calls.fetch_add(1, std::memory_order_acq_rel);
      const auto active = active_dials.fetch_add(1, std::memory_order_acq_rel) + 1;
      auto maximum = maximum_active_dials.load(std::memory_order_acquire);
      while (maximum < active && !maximum_active_dials.compare_exchange_weak(maximum, active, std::memory_order_acq_rel,
                                                                             std::memory_order_acquire)) {
      }
      if (call == 0) {
         const auto release_epoch = release->epoch();
         const auto cancellation_epoch = cancellation_seen->epoch();
         auto subscription =
             cancellation_latch::subscribe(cancellation, [cancellation_seen] noexcept { cancellation_seen->notify(); });
         static_cast<void>(subscription);
         dial_started.set_value();
         static_cast<void>(co_await cancellation_seen->async_wait(cancellation_epoch));
         static_cast<void>(co_await release->async_wait(release_epoch));
         active_dials.fetch_sub(1, std::memory_order_acq_rel);
         first_worker_finished->notify();
         co_return false;
      }
      active_dials.fetch_sub(1, std::memory_order_acq_rel);
      co_return true;
   };
   auto policy = test_topology_policy();
   policy.max_parallel_dials = 1;
   policy.peer_exchange_enabled = false;
   auto manager =
       std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks),
                                                  detail::topology_manager::clocks{
                                                      .before_dial_join_wait =
                                                          [&] {
                                                             if (fail_join.exchange(false, std::memory_order_acq_rel)) {
                                                                dial_started_future.wait();
                                                                throw std::bad_alloc{};
                                                             }
                                                          },
                                                  });
   const auto cancellation_epoch = cancellation_seen->epoch();
   auto canceled = boost::asio::co_spawn(runtime.context(), cancellation_seen->async_wait(cancellation_epoch),
                                         boost::asio::use_future);
   const auto finished_epoch = first_worker_finished->epoch();
   auto finished = boost::asio::co_spawn(runtime.context(), first_worker_finished->async_wait(finished_epoch),
                                         boost::asio::use_future);
   manager->start(lifecycle);
   auto first = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);

   BOOST_REQUIRE(canceled.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   static_cast<void>(canceled.get());
   BOOST_TEST(active_dials.load(std::memory_order_acquire) == 1U);
   const auto parent_waited_for_worker = first.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout;
   if (!parent_waited_for_worker) {
      BOOST_CHECK_THROW(static_cast<void>(first.get()), std::bad_alloc);
      BOOST_CHECK_NO_THROW(static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh())));
   }
   BOOST_TEST(parent_waited_for_worker);
   release->notify();
   if (parent_waited_for_worker) {
      BOOST_REQUIRE(first.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
      BOOST_CHECK_THROW(static_cast<void>(first.get()), std::bad_alloc);
   }
   BOOST_REQUIRE(finished.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   static_cast<void>(finished.get());
   BOOST_TEST(active_dials.load(std::memory_order_acquire) == 0U);

   if (parent_waited_for_worker) {
      BOOST_CHECK_NO_THROW(static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh())));
   }
   BOOST_TEST(dial_calls.load(std::memory_order_acquire) == 2U);
   BOOST_TEST(maximum_active_dials.load(std::memory_order_acquire) == 1U);
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_deduplicates_sources_and_retries_after_backoff) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   const auto steady_anchor = std::chrono::steady_clock::now();
   const auto system_anchor = std::chrono::system_clock::now();
   auto steady_offset = std::atomic<std::chrono::steady_clock::duration>{};
   auto system_offset = std::atomic<std::chrono::system_clock::duration>{};
   auto dials = std::atomic_size_t{};
   auto discovery_calls = std::atomic_size_t{};
   auto first_discovery_release = std::make_shared<forge::asio::notification>();
   auto first_discovery_entered = std::make_shared<std::promise<void>>();
   auto first_discovery_entered_future = first_discovery_entered->get_future();
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [system_anchor, &system_offset, &discovery_calls, first_discovery_release, first_discovery_entered](
           std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<std::vector<discovery::result>> {
      if (discovery_calls.fetch_add(1, std::memory_order_acq_rel) != 0) {
         co_return std::vector<discovery::result>{};
      }
      const auto release_epoch = first_discovery_release->epoch();
      first_discovery_entered->set_value();
      static_cast<void>(co_await first_discovery_release->async_wait(release_epoch));
      const auto system_now = system_anchor + system_offset.load(std::memory_order_acquire);
      co_return std::vector<discovery::result>{
          discovery::result{.peer = test_peer(61),
                            .endpoints = {discovered_rendezvous_address(test_peer(61), 4061)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = system_now + std::chrono::hours{1},
                            .score = 1.0},
          discovery::result{.peer = test_peer(61),
                            .endpoints = {discovered_rendezvous_address(test_peer(61), 4062)},
                            .discovered_by = discovery::source::dht,
                            .expires_at = system_now + std::chrono::hours{1},
                            .score = 2.0},
          discovery::result{.peer = test_peer(61),
                            .endpoints = {discovered_rendezvous_address(test_peer(61), 4063)},
                            .discovered_by = discovery::source::rendezvous,
                            .expires_at = system_now + std::chrono::hours{1},
                            .score = 1.0},
      };
   };
   callbacks.dial = [&dials](discovery::result, std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
      dials.fetch_add(1, std::memory_order_acq_rel);
      co_return false;
   };
   auto policy = test_topology_policy();
   policy.refresh_interval = std::chrono::hours{24};
   auto manager = std::make_shared<detail::topology_manager>(
       std::move(policy), std::move(callbacks), detail::topology_manager::clocks{
                                                   .steady_now = [steady_anchor, &steady_offset] {
                                                      return steady_anchor + steady_offset.load(std::memory_order_acquire);
                                                   },
                                                   .system_now = [system_anchor, &system_offset] {
                                                      return system_anchor + system_offset.load(std::memory_order_acquire);
                                                   },
                                               });
   manager->start(lifecycle);
   auto stopped = false;
   const auto cleanup = boost::scope::scope_exit{[&] {
      first_discovery_release->notify();
      if (!stopped) {
         try {
            stop_topology_manager(runtime, *manager, lifecycle);
         } catch (...) {
         }
      }
   }};

   BOOST_REQUIRE(first_discovery_entered_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   first_discovery_entered_future.get();
   auto strand = boost::asio::make_strand(runtime.context());
   auto first = boost::asio::co_spawn(strand, manager->async_refresh(), boost::asio::use_future);
   boost::asio::post(strand, [first_discovery_release] {
      first_discovery_release->notify();
   });
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   static_cast<void>(first.get());
   BOOST_TEST(dials.load(std::memory_order_acquire) == 1U);
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(dials.load(std::memory_order_acquire) == 2U);
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(dials.load(std::memory_order_acquire) == 2U);
   steady_offset.store(std::chrono::hours{3}, std::memory_order_release);
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(dials.load(std::memory_order_acquire) == 3U);
   system_offset.store(std::chrono::hours{2}, std::memory_order_release);
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(dials.load(std::memory_order_acquire) == 3U);
   stop_topology_manager(runtime, *manager, lifecycle);
   stopped = true;
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_stop_cancels_a_running_source) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto entered = std::promise<void>{};
   auto callbacks = topology_callbacks();
   callbacks.discover =
       [release, &entered](
           std::shared_ptr<cancellation_latch> cancellation) -> boost::asio::awaitable<std::vector<discovery::result>> {
      const auto observed = release->epoch();
      cancellation->arm([release] noexcept { release->notify(); });
      entered.set_value();
      static_cast<void>(co_await release->async_wait(observed));
      co_return std::vector<discovery::result>{};
   };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks));
   manager->start(lifecycle);
   entered.get_future().wait();
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_child_cancellation_honors_stop_before_arm) {
   auto parent = std::make_shared<cancellation_latch>();
   auto child = std::make_shared<cancellation_latch>();
   parent->request_stop();
   BOOST_TEST(parent->stop_requested());

   auto subscription = cancellation_latch::subscribe(parent, [child] noexcept { child->request_stop(); });
   BOOST_TEST(child->stop_requested());
   auto canceled = false;
   child->arm([&canceled] noexcept { canceled = true; });

   BOOST_TEST(canceled);
   BOOST_TEST(!child->finish());
   BOOST_TEST(!parent->finish());
   static_cast<void>(subscription);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_stop_cancels_a_running_peer_exchange_batch) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto release = std::make_shared<forge::asio::notification>();
   auto entered = std::promise<void>{};
   auto callbacks = topology_callbacks();
   callbacks.peer_exchange = [release,
                              &entered](std::shared_ptr<cancellation_latch> cancellation,
                                        std::size_t) -> boost::asio::awaitable<std::vector<discovery::result>> {
      const auto observed = release->epoch();
      cancellation->arm([release] noexcept { release->notify(); });
      entered.set_value();
      static_cast<void>(co_await release->async_wait(observed));
      co_return std::vector<discovery::result>{};
   };
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), std::move(callbacks));
   manager->start(lifecycle);
   entered.get_future().wait();
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_rendezvous_tracks_cookies_ttl_generation_and_unregister) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto steady_now = std::chrono::steady_clock::time_point{std::chrono::hours{200}};
   auto system_now = std::chrono::system_clock::time_point{std::chrono::hours{200}};
   auto policy = test_topology_policy();
   policy.dht_enabled = false;
   policy.peer_exchange_enabled = false;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(81), 4081),
           .namespaces = {"forge.first", "forge.second"},
       },
   };

   auto generation = std::uint64_t{1};
   auto invalidate_first_cookie = false;
   auto registrations = std::map<std::string, std::size_t>{};
   auto discoveries = std::map<std::string, std::vector<std::vector<std::uint8_t>>>{};
   auto unregisters = std::vector<std::string>{};
   auto callbacks = topology_callbacks();
   callbacks.local_rendezvous_record = [&generation] {
      return detail::topology_manager::callbacks::rendezvous_local_record{
          .generation = generation,
          .signed_peer_record = {0x01, 0x02},
      };
   };
   callbacks.rendezvous_register = [&registrations](std::size_t, std::string namespace_name,
                                                    std::vector<std::uint8_t> signed_peer_record,
                                                    std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_register_result> {
      BOOST_TEST(!signed_peer_record.empty());
      ++registrations[namespace_name];
      co_return detail::topology_manager::callbacks::rendezvous_register_result{
          .accepted = true,
          .ttl = std::chrono::minutes{10},
      };
   };
   callbacks.rendezvous_discover =
       [&discoveries, &invalidate_first_cookie](std::size_t, std::string namespace_name, std::size_t,
                                                std::vector<std::uint8_t> cookie, std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_discover_result> {
      discoveries[namespace_name].push_back(cookie);
      if (namespace_name == "forge.first" && invalidate_first_cookie && !cookie.empty()) {
         co_return detail::topology_manager::callbacks::rendezvous_discover_result{
             .response_status = detail::topology_manager::callbacks::rendezvous_discover_result::status::invalid_cookie,
         };
      }
      co_return detail::topology_manager::callbacks::rendezvous_discover_result{
          .response_status = detail::topology_manager::callbacks::rendezvous_discover_result::status::ok,
          .cookie = namespace_name == "forge.first" ? std::vector<std::uint8_t>{0x11} : std::vector<std::uint8_t>{0x22},
      };
   };
   callbacks.rendezvous_unregister = [&unregisters](std::size_t,
                                                    std::string namespace_name) -> boost::asio::awaitable<void> {
      unregisters.push_back(std::move(namespace_name));
      co_return;
   };
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks),
                                                             detail::topology_manager::clocks{
                                                                 .steady_now = [&steady_now] { return steady_now; },
                                                                 .system_now = [&system_now] { return system_now; },
                                                             });
   manager->start(lifecycle);
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   const auto registered_after_first_refresh = registrations;

   invalidate_first_cookie = true;
   discoveries.clear();
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(registrations["forge.first"] == registered_after_first_refresh.at("forge.first"));
   BOOST_TEST(registrations["forge.second"] == registered_after_first_refresh.at("forge.second"));
   BOOST_REQUIRE_EQUAL(discoveries["forge.first"].size(), 2U);
   BOOST_TEST(discoveries["forge.first"][0] == (std::vector<std::uint8_t>{0x11}), boost::test_tools::per_element());
   BOOST_TEST(discoveries["forge.first"][1].empty());
   BOOST_REQUIRE_EQUAL(discoveries["forge.second"].size(), 1U);
   BOOST_TEST(discoveries["forge.second"].front() == (std::vector<std::uint8_t>{0x22}),
              boost::test_tools::per_element());

   invalidate_first_cookie = false;
   system_now += std::chrono::minutes{10};
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(registrations["forge.first"] == registered_after_first_refresh.at("forge.first") + 1U);
   BOOST_TEST(registrations["forge.second"] == registered_after_first_refresh.at("forge.second") + 1U);

   ++generation;
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   BOOST_TEST(registrations["forge.first"] == registered_after_first_refresh.at("forge.first") + 2U);
   BOOST_TEST(registrations["forge.second"] == registered_after_first_refresh.at("forge.second") + 2U);

   stop_topology_manager(runtime, *manager, lifecycle);
   BOOST_REQUIRE_EQUAL(unregisters.size(), 2U);
   BOOST_TEST(unregisters[0] == "forge.first");
   BOOST_TEST(unregisters[1] == "forge.second");
}

BOOST_AUTO_TEST_CASE(p2p_topology_manager_rendezvous_isolates_point_failure_and_respects_backoff) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto steady_now = std::chrono::steady_clock::time_point{std::chrono::hours{210}};
   auto system_now = std::chrono::system_clock::time_point{std::chrono::hours{210}};
   auto policy = test_topology_policy();
   policy.dht_enabled = false;
   policy.peer_exchange_enabled = false;
   policy.rendezvous_points = {
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(82), 4082),
           .namespaces = {"forge.bad"},
       },
       topology::rendezvous_point{
           .endpoint = configured_rendezvous_endpoint(test_peer(83), 4083),
           .namespaces = {"forge.good"},
       },
   };
   auto registrations = std::map<std::string, std::size_t>{};
   auto discoveries = std::map<std::string, std::size_t>{};
   auto callbacks = topology_callbacks();
   callbacks.local_rendezvous_record = [] {
      return detail::topology_manager::callbacks::rendezvous_local_record{
          .generation = 1,
          .signed_peer_record = {0x01},
      };
   };
   callbacks.rendezvous_register = [&registrations](std::size_t, std::string namespace_name, std::vector<std::uint8_t>,
                                                    std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_register_result> {
      ++registrations[namespace_name];
      co_return detail::topology_manager::callbacks::rendezvous_register_result{
          .accepted = namespace_name != "forge.bad",
          .ttl = std::chrono::minutes{10},
      };
   };
   callbacks.rendezvous_discover = [&discoveries](std::size_t, std::string namespace_name, std::size_t,
                                                  std::vector<std::uint8_t>, std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_discover_result> {
      ++discoveries[namespace_name];
      co_return detail::topology_manager::callbacks::rendezvous_discover_result{
          .response_status = detail::topology_manager::callbacks::rendezvous_discover_result::status::ok,
          .results =
              {
                  discovery::result{
                      .peer = test_peer(84),
                      .endpoints = {discovered_rendezvous_address(test_peer(84), 4084)},
                  },
              },
      };
   };
   auto manager = std::make_shared<detail::topology_manager>(std::move(policy), std::move(callbacks),
                                                             detail::topology_manager::clocks{
                                                                 .steady_now = [&steady_now] { return steady_now; },
                                                                 .system_now = [&system_now] { return system_now; },
                                                             });
   manager->start(lifecycle);
   static_cast<void>(forge::asio::blocking::run(runtime, manager->async_refresh()));
   const auto bad_attempts = registrations["forge.bad"];
   const auto good_attempts = registrations["forge.good"];
   const auto results = forge::asio::blocking::run(runtime, manager->async_refresh());
   BOOST_TEST(registrations["forge.bad"] == bad_attempts);
   BOOST_TEST(registrations["forge.good"] == good_attempts);
   BOOST_TEST(discoveries["forge.good"] >= 1U);
   BOOST_REQUIRE(!results.empty());
   BOOST_TEST(static_cast<std::uint16_t>(results.front().discovered_by) ==
              static_cast<std::uint16_t>(discovery::source::rendezvous));
   stop_topology_manager(runtime, *manager, lifecycle);
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_soft_tags_replace_remove_and_check_overflow) {
   auto manager = test_connection_manager();
   const auto peer = test_peer(10);

   manager.tag(peer, "bootstrap", 40);
   manager.tag(peer, "gossip", -15);
   BOOST_TEST(manager.aggregate_tag_value(peer) == 25);

   manager.tag(peer, "bootstrap", 20);
   BOOST_TEST(manager.aggregate_tag_value(peer) == 5);
   BOOST_TEST(manager.untag(peer, "gossip"));
   BOOST_TEST(manager.aggregate_tag_value(peer) == 20);
   BOOST_TEST(manager.untag(peer, "bootstrap"));
   BOOST_TEST(manager.aggregate_tag_value(peer) == 0);
   BOOST_TEST(!manager.untag(peer, "bootstrap"));

   manager.tag(peer, "maximum", (std::numeric_limits<std::int64_t>::max)());
   BOOST_CHECK_THROW(manager.tag(peer, "overflow", 1), exceptions::invalid_options);
   BOOST_TEST(manager.aggregate_tag_value(peer) == (std::numeric_limits<std::int64_t>::max)());
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_soft_tag_limits_are_failure_atomic) {
   auto manager = connection_manager{connection_manager::policy{
       .max_sessions = 8,
       .low_watermark = 2,
       .max_tagged_peers = 1,
       .max_tags_per_peer = 1,
       .max_tag_size = 3,
       .grace_period = std::chrono::milliseconds{0},
       .prune_silence = std::chrono::milliseconds{1},
   }};
   const auto first = test_peer(11);
   const auto second = test_peer(12);

   manager.tag(first, "one", 10);
   manager.tag(first, "one", 20);
   BOOST_TEST(manager.aggregate_tag_value(first) == 20);

   BOOST_CHECK_THROW(manager.tag(first, "two", 1), exceptions::backpressure_rejected);
   BOOST_TEST(manager.aggregate_tag_value(first) == 20);
   BOOST_CHECK_THROW(manager.tag(second, "one", 1), exceptions::backpressure_rejected);
   BOOST_TEST(manager.aggregate_tag_value(second) == 0);
   BOOST_CHECK_THROW(manager.tag(first, "long", 1), exceptions::invalid_options);
   BOOST_TEST(manager.aggregate_tag_value(first) == 20);
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_peer_prune_is_deterministic_and_preserves_protected_peers) {
   auto manager = test_connection_manager();
   const auto first = test_peer(20);
   const auto second = test_peer(21);
   const auto third = test_peer(22);
   const auto now = std::chrono::steady_clock::now();

   remember(manager, 1, first, 100.0, now - std::chrono::seconds{4}, now - std::chrono::seconds{3});
   remember(manager, 2, first, 100.0, now - std::chrono::seconds{3}, now - std::chrono::seconds{2});
   remember(manager, 3, second, 1.0, now - std::chrono::seconds{2}, now - std::chrono::seconds{2});
   remember(manager, 4, third, 1.0, now - std::chrono::seconds{1}, now - std::chrono::seconds{1});
   manager.tag(first, "low-priority", -10);

   const auto snapshot = manager.current(8);
   BOOST_TEST(snapshot.active_sessions == 4U);
   BOOST_TEST(snapshot.active_peers == 3U);

   const auto tagged_plan = manager.plan_peer_prune(2, 1, now);
   BOOST_TEST(tagged_plan.connected_peers == 3U);
   BOOST_REQUIRE_EQUAL(tagged_plan.victim_peers.size(), 1U);
   BOOST_TEST(tagged_plan.victim_peers.front().to_string() == first.to_string());
   BOOST_REQUIRE_EQUAL(tagged_plan.session_ids.size(), 2U);
   BOOST_TEST(tagged_plan.session_ids[0] == 1U);
   BOOST_TEST(tagged_plan.session_ids[1] == 2U);

   manager.protect(first, "bootstrap");
   const auto protected_plan = manager.plan_peer_prune(2, 1, now);
   BOOST_REQUIRE_EQUAL(protected_plan.victim_peers.size(), 1U);
   BOOST_TEST(protected_plan.victim_peers.front().to_string() == second.to_string());
   BOOST_REQUIRE_EQUAL(protected_plan.session_ids.size(), 1U);
   BOOST_TEST(protected_plan.session_ids.front() == 3U);
}

BOOST_AUTO_TEST_CASE(p2p_connection_manager_hard_admission_uses_tag_score_grace_and_recency_order) {
   auto manager = connection_manager{connection_manager::policy{
       .max_sessions = 2,
       .low_watermark = 1,
       .grace_period = std::chrono::seconds{10},
       .prune_silence = std::chrono::milliseconds{1},
   }};
   const auto tagged = test_peer(71);
   const auto lower_score = test_peer(72);
   const auto incoming = test_peer(73);
   const auto now = std::chrono::steady_clock::now();

   remember(manager, 1, tagged, 100.0, now - std::chrono::seconds{30}, now - std::chrono::seconds{20});
   remember(manager, 2, lower_score, -100.0, now - std::chrono::seconds{30}, now - std::chrono::seconds{25});
   manager.tag(tagged, "topology-low", -1);
   const auto admission = manager.remember(
       connection_manager::session_record{
           .id = 3,
           .peer = incoming,
           .opened_at = now,
           .last_used_at = now,
       },
       now);
   BOOST_REQUIRE(admission.accepted);
   BOOST_REQUIRE_EQUAL(admission.pruned.size(), 1U);
   BOOST_TEST(admission.pruned.front() == 1U);

   auto grace_manager = connection_manager{connection_manager::policy{
       .max_sessions = 4,
       .low_watermark = 1,
       .grace_period = std::chrono::seconds{10},
       .prune_silence = std::chrono::milliseconds{1},
   }};
   const auto old_peer = test_peer(74);
   const auto fresh_peer = test_peer(75);
   remember(grace_manager, 10, old_peer, 0.0, now - std::chrono::seconds{20}, now - std::chrono::seconds{20});
   remember(grace_manager, 11, fresh_peer, -100.0, now, now);
   const auto plan = grace_manager.plan_peer_prune(0, 2, now);
   BOOST_REQUIRE_EQUAL(plan.victim_peers.size(), 1U);
   BOOST_TEST(plan.victim_peers.front().to_string() == old_peer.to_string());
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_replacement_cancels_old_generation_without_rediscovery) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto peers = std::vector{test_peer(101), test_peer(102)};
   std::ranges::sort(peers, {}, [](const auto& peer) { return peer.to_string(); });
   auto discoveries = std::atomic_size_t{};
   auto dials = std::atomic_size_t{};
   auto closed = std::atomic_size_t{};
   auto connected = std::atomic_bool{};
   auto idle_once = std::atomic_bool{};
   auto idle = std::promise<void>{};
   auto idle_future = idle.get_future();
   auto started = std::promise<void>{};
   auto started_future = started.get_future();
   auto completed = std::promise<discovery::result>{};
   auto completed_future = completed.get_future();
   auto canceled = std::make_shared<forge::asio::notification>();
   auto callbacks = topology_callbacks();
   callbacks.discover = [&](std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<std::vector<discovery::result>> {
      ++discoveries;
      co_return std::vector<discovery::result>{};
   };
   callbacks.dial = [&](discovery::result candidate, std::shared_ptr<cancellation_latch> cancellation)
       -> boost::asio::awaitable<bool> {
      if (dials.fetch_add(1) == 0) {
         co_return co_await wait_for_dial_cancellation(canceled, &started, cancellation);
      }
      connected = true;
      completed.set_value(std::move(candidate));
      co_return true;
   };
   callbacks.sessions = [&] {
      return connection_manager::snapshot{.active_sessions = connected ? 1U : 0U,
                                           .active_peers = connected ? 1U : 0U};
   };
   callbacks.close_sessions = [&](std::vector<std::uint64_t>) -> boost::asio::awaitable<void> {
      ++closed;
      co_return;
   };
   auto policy = test_topology_policy();
   policy.max_parallel_dials = 1;
   policy.refresh_interval = std::chrono::hours{24};
   auto manager = std::make_shared<detail::topology_manager>(policy, std::move(callbacks),
       detail::topology_manager::clocks{.before_idle_wait = [&] {
          if (!idle_once.exchange(true)) { idle.set_value(); }
       }});
   const auto cleanup = boost::scope::scope_exit{[&] { stop_topology_manager(runtime, *manager, lifecycle); }};
   manager->start(lifecycle);
   BOOST_REQUIRE(idle_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours{1};
   manager->replace_mdns_snapshot({
       {peers[0], discovered_rendezvous_address(peers[0], 4101), 7, 1, deadline},
       {peers[1], discovered_rendezvous_address(peers[1], 4102), 7, 1, deadline}});
   BOOST_REQUIRE(started_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   // One active and one queued old-generation candidate must both be withdrawn.
   const auto replacement = discovered_rendezvous_address(peers[1], 4202);
   manager->replace_mdns_snapshot({{peers[1], replacement, 7, 2, deadline}});
   BOOST_REQUIRE(completed_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   const auto result = completed_future.get();
   BOOST_TEST(static_cast<unsigned>(result.discovered_by) == 5U);
   BOOST_REQUIRE_EQUAL(result.endpoints.size(), 1U);
   BOOST_TEST(result.endpoints.front().to_string() == replacement.to_string());
   manager->replace_mdns_snapshot({});
   manager->request_stop();
   auto joined = boost::asio::co_spawn(runtime.context(), manager->async_join(), boost::asio::use_future);
   BOOST_REQUIRE(joined.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   joined.get();
   BOOST_TEST(discoveries.load() == 1U);
   BOOST_TEST(dials.load() == 2U);
   BOOST_TEST(closed.load() == 0U);
   BOOST_TEST(connected.load());
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_steady_expiry_cancels_inflight_without_discovery) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto discoveries = std::atomic_size_t{};
   auto dials = std::atomic_size_t{};
   auto started = std::promise<void>{};
   auto started_future = started.get_future();
   auto expired = std::promise<void>{};
   auto expired_future = expired.get_future();
   auto canceled = std::make_shared<forge::asio::notification>();
   auto callbacks = topology_callbacks();
   callbacks.discover = [&](std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<std::vector<discovery::result>> {
      ++discoveries;
      co_return std::vector<discovery::result>{};
   };
   callbacks.dial = [&](discovery::result, std::shared_ptr<cancellation_latch> cancellation)
       -> boost::asio::awaitable<bool> {
      ++dials;
      co_await wait_for_dial_cancellation(canceled, &started, cancellation);
      expired.set_value();
      co_return false;
   };
   auto policy = test_topology_policy();
   policy.refresh_interval = std::chrono::hours{24};
   auto manager = std::make_shared<detail::topology_manager>(policy, std::move(callbacks));
   const auto cleanup = boost::scope::scope_exit{[&] { stop_topology_manager(runtime, *manager, lifecycle); }};
   const auto peer = test_peer(103);
   manager->replace_mdns_snapshot({{peer, discovered_rendezvous_address(peer), 9, 1,
                                   std::chrono::steady_clock::now() + std::chrono::seconds{1}}});
   manager->start(lifecycle);
   BOOST_REQUIRE(started_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_REQUIRE(expired_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   manager->request_stop();
   auto joined = boost::asio::co_spawn(runtime.context(), manager->async_join(), boost::asio::use_future);
   BOOST_REQUIRE(joined.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   joined.get();
   BOOST_TEST(discoveries.load() == 1U);
   BOOST_TEST(dials.load() == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_renewal_preserves_inflight_past_old_expiry) {
   using namespace std::chrono_literals;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto started = std::promise<void>{};
   auto started_future = started.get_future();
   auto first_canceled = std::promise<void>{};
   auto canceled_future = first_canceled.get_future();
   auto dials = std::atomic_size_t{};
   auto completions = std::atomic_size_t{};
   auto callbacks = topology_callbacks();
   callbacks.dial = [&](discovery::result, std::shared_ptr<cancellation_latch> cancellation)
       -> boost::asio::awaitable<bool> {
      const auto first = dials.fetch_add(1) == 0;
      auto wakeup = std::make_shared<forge::asio::notification>();
      const auto epoch = wakeup->epoch();
      auto subscription = cancellation_latch::subscribe(cancellation, [wakeup]() noexcept { wakeup->notify(); });
      if (first) { started.set_value(); }
      if (!cancellation->stop_requested()) { static_cast<void>(co_await wakeup->async_wait(epoch)); }
      ++completions;
      if (first) { first_canceled.set_value(); }
      co_return false;
   };
   auto policy = test_topology_policy();
   policy.max_parallel_dials = 1;
   policy.refresh_interval = 24h;
   auto manager = std::make_shared<detail::topology_manager>(policy, std::move(callbacks));
   const auto cleanup = boost::scope::scope_exit{[&] { stop_topology_manager(runtime, *manager, lifecycle); }};
   const auto peer = test_peer(104);
   const auto address = discovered_rendezvous_address(peer);
   const auto old_expiry = std::chrono::steady_clock::now() + 2s;
   manager->replace_mdns_snapshot({{peer, address, 9, 1, old_expiry}});
   manager->start(lifecycle);
   BOOST_REQUIRE(started_future.wait_for(1s) == std::future_status::ready);
   BOOST_REQUIRE(std::chrono::steady_clock::now() < old_expiry);
   manager->replace_mdns_snapshot({{peer, address, 9, 1, old_expiry + 30s}});
   // This observation window crosses the old timer deadline, but is independent
   // of the renewed lease. No successful connection or retry can mask cancellation.
   BOOST_CHECK_MESSAGE(canceled_future.wait_until(old_expiry + 250ms) == std::future_status::timeout,
                       "continuous mDNS lease renewal canceled the original in-flight dial");
   BOOST_TEST(dials.load() == 1U);
   BOOST_TEST(completions.load() == 0U);
   manager->request_stop();
   lifecycle.request_stop();
   auto joined = boost::asio::co_spawn(runtime.context(), manager->async_join(), boost::asio::use_future);
   BOOST_REQUIRE(joined.wait_for(2s) == std::future_status::ready);
   joined.get();
   auto tracked = boost::asio::co_spawn(runtime.context(), lifecycle.wait(), boost::asio::use_future);
   BOOST_REQUIRE(tracked.wait_for(2s) == std::future_status::ready);
   tracked.get();
   BOOST_TEST(completions.load() == dials.load());
   BOOST_CHECK(canceled_future.wait_for(0ms) == std::future_status::ready);
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_snapshot_order_and_duplicates_preserve_inflight) {
   using namespace std::chrono_literals;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto started = std::promise<void>{};
   auto started_future = started.get_future();
   auto first_canceled = std::promise<void>{};
   auto canceled_future = first_canceled.get_future();
   auto dials = std::atomic_size_t{};
   auto completions = std::atomic_size_t{};
   auto callbacks = topology_callbacks();
   callbacks.dial = [&](discovery::result candidate, std::shared_ptr<cancellation_latch> cancellation)
       -> boost::asio::awaitable<bool> {
      const auto first = dials.fetch_add(1) == 0;
      BOOST_TEST(candidate.endpoints.size() == 2U);
      auto wakeup = std::make_shared<forge::asio::notification>();
      const auto epoch = wakeup->epoch();
      auto subscription = cancellation_latch::subscribe(cancellation, [wakeup]() noexcept { wakeup->notify(); });
      if (first) { started.set_value(); }
      if (!cancellation->stop_requested()) { static_cast<void>(co_await wakeup->async_wait(epoch)); }
      ++completions;
      if (first) { first_canceled.set_value(); }
      co_return false;
   };
   auto policy = test_topology_policy();
   policy.max_parallel_dials = 1;
   policy.refresh_interval = 24h;
   auto manager = std::make_shared<detail::topology_manager>(policy, std::move(callbacks));
   const auto cleanup = boost::scope::scope_exit{[&] { stop_topology_manager(runtime, *manager, lifecycle); }};
   const auto peer = test_peer(105);
   const auto expiry = std::chrono::steady_clock::now() + 1h;
   const auto first = detail::topology_manager::mdns_lease{peer, discovered_rendezvous_address(peer, 4101), 7, 1, expiry};
   const auto second = detail::topology_manager::mdns_lease{peer, discovered_rendezvous_address(peer, 4102), 8, 3, expiry};
   manager->replace_mdns_snapshot({first, second});
   manager->start(lifecycle);
   BOOST_REQUIRE(started_future.wait_for(2s) == std::future_status::ready);
   manager->replace_mdns_snapshot({second, first});
   BOOST_CHECK_MESSAGE(canceled_future.wait_for(100ms) == std::future_status::timeout,
                       "snapshot permutation changed the active lease identity");
   manager->replace_mdns_snapshot({second, first, second, first});
   BOOST_CHECK_MESSAGE(canceled_future.wait_for(250ms) == std::future_status::timeout,
                       "duplicate lease evidence changed revision and canceled the original dial");
   BOOST_TEST(dials.load() == 1U);
   BOOST_TEST(completions.load() == 0U);
   manager->request_stop();
   lifecycle.request_stop();
   auto joined = boost::asio::co_spawn(runtime.context(), manager->async_join(), boost::asio::use_future);
   BOOST_REQUIRE(joined.wait_for(2s) == std::future_status::ready);
   joined.get();
   auto tracked = boost::asio::co_spawn(runtime.context(), lifecycle.wait(), boost::asio::use_future);
   BOOST_REQUIRE(tracked.wait_for(2s) == std::future_status::ready);
   tracked.get();
   BOOST_TEST(completions.load() == dials.load());
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_observations_count_live_peers_not_duplicate_leases) {
   using namespace std::chrono_literals;
   auto now = std::chrono::steady_clock::now();
   auto manager = std::make_shared<detail::topology_manager>(test_topology_policy(), topology_callbacks(),
       detail::topology_manager::clocks{.steady_now = [&] { return now; }});
   const auto first = detail::topology_manager::mdns_lease{
       test_peer(106), discovered_rendezvous_address(test_peer(106)), 7, 1, now + 1s};
   auto other_interface = first;
   other_interface.interface_index = 8;
   const auto second = detail::topology_manager::mdns_lease{
       test_peer(107), discovered_rendezvous_address(test_peer(107)), 7, 1, now + 1s};
   BOOST_TEST(manager->current().mdns_observations == 0U);
   manager->replace_mdns_snapshot({first, first, other_interface, second});
   BOOST_TEST(manager->current().mdns_observations == 2U);
   BOOST_TEST(manager->current().observations == 0U);
   manager->replace_mdns_snapshot({first, other_interface});
   BOOST_TEST(manager->current().mdns_observations == 1U);
   now += 1s;
   // Diagnostics must not report stale evidence even before autonomous pruning.
   BOOST_TEST(manager->current().mdns_observations == 0U);
   auto renewed = first;
   renewed.expires_at = now + 1s;
   manager->replace_mdns_snapshot({renewed});
   BOOST_TEST(manager->current().mdns_observations == 1U);
   manager->replace_mdns_snapshot({});
   BOOST_TEST(manager->current().mdns_observations == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_synchronous_dial_construction_failure_closes_batch) {
   using namespace std::chrono_literals;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto constructing = std::promise<void>{};
   auto constructing_future = constructing.get_future();
   auto release = std::promise<void>{};
   auto released = release.get_future().share();
   auto calls = std::atomic_size_t{};
   auto callbacks = topology_callbacks();
   // Deliberately not a coroutine: failure happens while obtaining awaitable.
   callbacks.dial = [&](discovery::result, std::shared_ptr<cancellation_latch>) -> boost::asio::awaitable<bool> {
      if (calls.fetch_add(1) == 0) {
         constructing.set_value();
         if (released.wait_for(3s) != std::future_status::ready) {
            throw std::runtime_error{"mDNS construction test release deadline"};
         }
         throw std::bad_alloc{};
      }
      return successful_topology_dial();
   };
   auto policy = test_topology_policy();
   policy.max_parallel_dials = 1;
   policy.refresh_interval = 24h;
   auto manager = std::make_shared<detail::topology_manager>(policy, std::move(callbacks));
   const auto cleanup = boost::scope::scope_exit{[&] {
      try { release.set_value(); } catch (const std::future_error&) {}
      stop_topology_manager(runtime, *manager, lifecycle);
   }};
   const auto expiry = std::chrono::steady_clock::now() + 1h;
   manager->replace_mdns_snapshot({
       {test_peer(108), discovered_rendezvous_address(test_peer(108)), 7, 1, expiry},
       {test_peer(109), discovered_rendezvous_address(test_peer(109)), 7, 1, expiry}});
   manager->start(lifecycle);
   BOOST_REQUIRE(constructing_future.wait_for(2s) == std::future_status::ready);
   auto failed = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   const auto waiter_deadline = std::chrono::steady_clock::now() + 1s;
   while (manager->current().waiting_refreshes == 0 && std::chrono::steady_clock::now() < waiter_deadline) {
      std::this_thread::sleep_for(1ms);
   }
   const auto waiter_registered = manager->current().waiting_refreshes != 0;
   release.set_value();
   BOOST_REQUIRE(waiter_registered);
   BOOST_REQUIRE(failed.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(failed.get()), std::bad_alloc);
   BOOST_TEST(calls.load() == 1U);
   BOOST_TEST(manager->current().active_operations == 0U);
   // A local construction failure must not impose peer backoff on either
   // candidate. Both are immediately eligible in the next explicit refresh.
   auto retry = boost::asio::co_spawn(runtime.context(), manager->async_refresh(), boost::asio::use_future);
   BOOST_REQUIRE(retry.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(static_cast<void>(retry.get()));
   BOOST_TEST(calls.load() == 3U);
   manager->request_stop();
   lifecycle.request_stop();
   auto joined = boost::asio::co_spawn(runtime.context(), manager->async_join(), boost::asio::use_future);
   BOOST_REQUIRE(joined.wait_for(2s) == std::future_status::ready);
   joined.get();
   auto tracked = boost::asio::co_spawn(runtime.context(), lifecycle.wait(), boost::asio::use_future);
   BOOST_REQUIRE(tracked.wait_for(2s) == std::future_status::ready);
   tracked.get();
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_shortened_live_lease_waits_until_new_expiry) {
   using namespace std::chrono_literals;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lifecycle = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(lifecycle.begin_start());
   auto started = std::promise<void>{};
   auto started_future = started.get_future();
   auto canceled = std::promise<void>{};
   auto canceled_future = canceled.get_future();
   auto calls = std::atomic_size_t{};
   auto wakeup = std::make_shared<forge::asio::notification>();
   auto callbacks = topology_callbacks();
   callbacks.dial = [&](discovery::result, std::shared_ptr<cancellation_latch> stop)
       -> boost::asio::awaitable<bool> {
      const auto first = calls.fetch_add(1) == 0;
      const auto epoch = wakeup->epoch();
      auto subscription = cancellation_latch::subscribe(stop, [wakeup]() noexcept { wakeup->notify(); });
      if (first) { started.set_value(); }
      if (!stop->stop_requested()) { static_cast<void>(co_await wakeup->async_wait(epoch)); }
      if (first) { canceled.set_value(); }
      co_return false;
   };
   auto policy = test_topology_policy();
   policy.refresh_interval = 24h;
   policy.max_parallel_dials = 1;
   auto manager = std::make_shared<detail::topology_manager>(policy, std::move(callbacks));
   const auto cleanup = boost::scope::scope_exit{[&] { stop_topology_manager(runtime, *manager, lifecycle); }};
   const auto peer = test_peer(110);
   const auto address = discovered_rendezvous_address(peer);
   manager->replace_mdns_snapshot({{peer, address, 7, 1, std::chrono::steady_clock::now() + 1h}});
   manager->start(lifecycle);
   BOOST_REQUIRE(started_future.wait_for(2s) == std::future_status::ready);
   const auto expiry = std::chrono::steady_clock::now() + 1s;
   manager->replace_mdns_snapshot({{peer, address, 7, 1, expiry}});
   BOOST_CHECK_MESSAGE(canceled_future.wait_until(expiry - 200ms) == std::future_status::timeout,
                       "shortened but live lease canceled dial before its new expiry");
   BOOST_REQUIRE(canceled_future.wait_until(expiry + 1s) == std::future_status::ready);
   BOOST_TEST(calls.load() == 1U);
   manager->request_stop();
   lifecycle.request_stop();
   auto joined = boost::asio::co_spawn(runtime.context(), manager->async_join(), boost::asio::use_future);
   BOOST_REQUIRE(joined.wait_for(2s) == std::future_status::ready);
   joined.get();
}

BOOST_AUTO_TEST_CASE(p2p_topology_mdns_snapshot_bounds_cancel_storage_independent_of_parallel_policy) {
   auto policy = test_topology_policy();
   policy.max_parallel_dials = std::numeric_limits<std::size_t>::max();
   auto manager = std::make_shared<detail::topology_manager>(policy, topology_callbacks());
   const auto peer = test_peer(111);
   BOOST_CHECK_NO_THROW(manager->replace_mdns_snapshot({{peer, discovered_rendezvous_address(peer), 7, 1,
       std::chrono::steady_clock::now() + std::chrono::hours{1}}}));
   BOOST_TEST(manager->current().mdns_observations == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
