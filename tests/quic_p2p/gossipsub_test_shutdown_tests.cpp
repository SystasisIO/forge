module;

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/strand.hpp>
#include <boost/test/unit_test.hpp>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

module forge.net.p2p.node;
import forge.asio.runtime;
import forge.net.p2p.diagnostics;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.tcp.connection;
import forge.net.tcp.listener;

#include "gossipsub_test_shutdown.hxx"
#include "gossipsub_test_shutdown_fixture.hxx"

namespace forge::tests::p2p {

BOOST_AUTO_TEST_SUITE(p2p_gossipsub_shutdown)

BOOST_AUTO_TEST_CASE(failure_unwind_releases_gate_and_joins_pending_native_cold_subscriptions_before_stop) {
   auto fixture = gossipsub_test_shutdown_fixture{};
   auto unwound = false;
   try {
      auto shutdown = gossipsub_test_shutdown{fixture.runtime, fixture.first, fixture.second,
         [&] { fixture.release_workers(); }, [&](auto deadline) { fixture.join_workers(deadline); },
         [&] { fixture.cancel_workers(); }};
      fixture.admit_cold_subscriptions();
      BOOST_TEST(fixture.first.metrics().sessions_opened == 0U);
      throw std::logic_error{"test body unwinds while native cold subscription is pending"};
   } catch (const std::logic_error&) { unwound = true; }
   BOOST_TEST(unwound);
   BOOST_TEST(fixture.barriers == 1U);
   BOOST_TEST(fixture.barrier_before_stop);
   BOOST_TEST(fixture.cancellations == 0U);
   BOOST_TEST(fixture.workers_ready());
   for (auto& worker : fixture.workers) { BOOST_CHECK_NO_THROW(static_cast<void>(worker.get())); }
   BOOST_TEST(fixture.first.diagnostics().persistence.closed);
   BOOST_TEST(fixture.second.diagnostics().persistence.closed);
   BOOST_TEST(fixture.first.metrics().active_sessions == 0U);
}

BOOST_AUTO_TEST_CASE(failed_first_worker_barrier_cancels_and_settles_native_waiters_then_stops_both_owners) {
   auto fixture = gossipsub_test_shutdown_fixture{};
   auto callbacks = 0U;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, fixture.first, fixture.second,
      [&] { fixture.release_workers(); }, [&](auto deadline) {
         if (++callbacks == 1U) { throw std::logic_error{"injected first worker barrier failure"}; }
         fixture.join_workers(deadline);
      }, [&] { fixture.cancel_workers(); }};
   fixture.admit_cold_subscriptions();
   BOOST_CHECK_THROW(shutdown.join(), std::logic_error);
   BOOST_TEST(callbacks == 2U);
   BOOST_TEST(fixture.cancellations == 1U);
   BOOST_TEST(fixture.barrier_before_stop);
   BOOST_TEST(fixture.workers_ready());
   BOOST_TEST(fixture.first.diagnostics().persistence.closed);
   BOOST_TEST(fixture.second.diagnostics().persistence.closed);
   BOOST_CHECK_NO_THROW(shutdown.join());
   BOOST_TEST(callbacks == 2U);
}

BOOST_AUTO_TEST_CASE(failed_owner_stop_remains_unjoined_and_destructor_retries_only_that_owner) {
   auto fixture = gossipsub_test_shutdown_fixture{};
   fixture.first_store->fail_next_close.store(true);
   {
      auto shutdown = gossipsub_test_shutdown{fixture.runtime, fixture.first, fixture.second};
      BOOST_CHECK_THROW(shutdown.join(), std::runtime_error);
      BOOST_TEST(fixture.first_store->close_calls.load() == 1U);
      BOOST_TEST(fixture.second_store->close_calls.load() == 1U);
      BOOST_TEST(!fixture.first.diagnostics().persistence.closed);
      BOOST_TEST(fixture.second.diagnostics().persistence.closed);
   }
   BOOST_TEST(fixture.first_store->close_calls.load() == 2U);
   BOOST_TEST(fixture.second_store->close_calls.load() == 1U);
   BOOST_TEST(fixture.first.diagnostics().persistence.closed);
   BOOST_TEST(fixture.second.diagnostics().persistence.closed);
}

BOOST_AUTO_TEST_CASE(failed_worker_barrier_without_caller_cancel_still_stops_and_settles_native_singleflight) {
   auto fixture = gossipsub_test_shutdown_fixture{};
   auto callbacks = 0U;
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, fixture.first, fixture.second,
      [&] { fixture.release_workers(); }, [&](auto deadline) {
         if (++callbacks == 1U) { throw std::logic_error{"injected worker barrier failure before node stop"}; }
         fixture.join_workers(deadline);
      }};
   fixture.admit_cold_subscriptions();
   BOOST_CHECK_THROW(shutdown.join(), std::logic_error);
   BOOST_TEST(callbacks == 2U);
   BOOST_TEST(fixture.cancellations == 0U);
   BOOST_TEST(!fixture.barrier_before_stop);
   BOOST_TEST(fixture.workers_ready());
   BOOST_TEST(fixture.first.diagnostics().persistence.closed);
   BOOST_TEST(fixture.second.diagnostics().persistence.closed);
   BOOST_CHECK_NO_THROW(shutdown.join());
   BOOST_TEST(callbacks == 2U);
}

#if defined(__unix__) || defined(__APPLE__)
BOOST_AUTO_TEST_CASE(exhausted_join_exits_child_before_local_native_owners_can_be_destroyed) {
   const auto child = gossipsub_test_shutdown_fixture::run_exhaustion_child(FORGE_GOSSIPSUB_TEST_EXECUTABLE);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(child.reaped);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == gossipsub_test_shutdown::incomplete_join_exit_code);
}

BOOST_AUTO_TEST_CASE(child_deadline_kills_and_reaps_without_accepting_timeout_as_fatal_join_receipt) {
   const auto child = gossipsub_test_shutdown_fixture::run_exhaustion_child(FORGE_GOSSIPSUB_TEST_EXECUTABLE,
      gossipsub_test_shutdown_fixture::child_mode::wait_until_killed, std::chrono::milliseconds{100});
   BOOST_REQUIRE(child.timed_out);
   BOOST_REQUIRE(child.reaped);
   BOOST_REQUIRE(WIFSIGNALED(child.status));
   BOOST_TEST(WTERMSIG(child.status) == SIGKILL);
}

BOOST_AUTO_TEST_CASE(child_launcher_exception_preserves_primary_error_after_actual_bounded_reap) {
   auto cleanup = gossipsub_test_shutdown_fixture::child_result{};
   BOOST_CHECK_THROW(static_cast<void>(gossipsub_test_shutdown_fixture::run_exhaustion_child(
      FORGE_GOSSIPSUB_TEST_EXECUTABLE, gossipsub_test_shutdown_fixture::child_mode::fail_after_spawn,
      std::chrono::seconds{10}, &cleanup)), std::logic_error);
   BOOST_REQUIRE(cleanup.reaped);
   BOOST_TEST(!cleanup.timed_out);
   BOOST_REQUIRE(WIFSIGNALED(cleanup.status));
   BOOST_TEST(WTERMSIG(cleanup.status) == SIGKILL);
}
#endif

BOOST_AUTO_TEST_SUITE_END()

#if defined(__unix__) || defined(__APPLE__)
BOOST_AUTO_TEST_SUITE(p2p_gossipsub_shutdown_child, *boost::unit_test::disabled())

BOOST_AUTO_TEST_CASE(unfinished_join_exhaustion) {
   auto fixture = gossipsub_test_shutdown_fixture{};
   // A setup failure must not masquerade as the expected cleanup fatal exit.
   try { fixture.admit_cold_subscriptions(); }
   catch (...) { std::_Exit(87); }
   if (fixture.workers_ready()) { std::_Exit(87); }
   try {
      auto shutdown = gossipsub_test_shutdown{fixture.runtime, fixture.first, fixture.second,
         [] {}, [&](auto deadline) { fixture.join_workers(deadline); }, {}, std::chrono::milliseconds{20}};
      // Deliberately leave the follower start gate closed. These are real
      // pending native subscription coroutines retaining references to fixture.
   } catch (...) { std::_Exit(87); }
   // Reaching here would reproduce the unsafe return; exit before actually
   // destroying the referenced owners so the parent gets an unambiguous failure.
   std::_Exit(88);
}

BOOST_AUTO_TEST_CASE(wait_for_parent_cleanup) {
   auto context = boost::asio::io_context{};
   auto signal = boost::asio::signal_set{context, SIGUSR1};
   signal.async_wait([](boost::system::error_code, int) {});
   context.run();
   std::_Exit(88);
}

BOOST_AUTO_TEST_SUITE_END()
#endif

} // namespace forge::tests::p2p
