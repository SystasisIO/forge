module;

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.asio.runtime;
import forge.net.p2p.diagnostics;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.relay;

#include "autorelay_manager_fixture.hxx"

namespace {
namespace p2p = forge::net::p2p;
using fixture = forge::tests::p2p::autorelay_manager_fixture;
using namespace std::chrono_literals;

template <typename T> T ready(std::future<T>& value) {
   BOOST_REQUIRE(value.wait_for(4s) == std::future_status::ready);
   return value.get();
}

p2p::relay::policy policy(std::size_t target = 1, std::size_t parallel = 1, std::size_t candidates = 16) {
   return {.target_reservations = target, .max_candidates_per_refresh = candidates,
           .max_parallel_reservations = parallel, .candidate_backoff = 1h};
}
} // namespace

BOOST_AUTO_TEST_SUITE(autorelay_manager_tests)

BOOST_AUTO_TEST_CASE(max_parallel_bound_limits_real_blocked_callbacks) {
   auto value = fixture{policy(4, 2)};
   auto state = fixture::snapshot({1, 2, 3, 4, 5, 6});
   value.set_snapshot(state);
   value.start();
   value.wait_started(2);
   BOOST_TEST(value.requests().size() == 2U);
   BOOST_TEST(value.peak_active() == 2U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 2U);
   for (auto index = std::size_t{}; index < 2; ++index) {
      state.reservations.push_back(fixture::lease(value.requests()[index].source));
      value.set_snapshot(state);
      value.release(index);
      value.wait_started(index + 3);
   }
   const auto requests = value.requests();
   state.reservations.push_back(fixture::lease(requests[2].source));
   state.reservations.push_back(fixture::lease(requests[3].source));
   value.set_snapshot(state);
   value.release(2);
   value.release(3);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.reservations == 4; });
   auto refreshed = value.refresh();
   BOOST_TEST(ready(refreshed).size() == 4U);
   BOOST_TEST(value.requests().size() == 4U);
   BOOST_TEST(value.peak_active() == 2U);
}

BOOST_AUTO_TEST_CASE(renewal_and_acquisition_use_separate_target_accounting) {
   auto value = fixture{policy(3, 2)};
   auto state = fixture::snapshot({1, 2, 3, 4});
   state.reservations = {fixture::lease(fixture::candidate(1), true), fixture::lease(fixture::candidate(2), false, false)};
   value.set_snapshot(state);
   value.start();
   value.wait_started(2);
   const auto requests = value.requests();
   BOOST_TEST(std::ranges::count_if(requests, [](const auto& item) { return item.source.peer == fixture::candidate(1).peer; }) == 1);
   BOOST_TEST(std::ranges::none_of(requests, [](const auto& item) { return item.source.peer == fixture::candidate(2).peer; }));
   const auto acquired = std::ranges::find_if(requests, [](const auto& item) {
      return item.source.peer != fixture::candidate(1).peer;
   });
   BOOST_REQUIRE(acquired != requests.end());
   state.reservations = {fixture::lease(fixture::candidate(1)), fixture::lease(fixture::candidate(2), false, false),
                         fixture::lease(acquired->source)};
   value.set_snapshot(state);
   value.release(0);
   value.release(1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 2; });
   auto refreshed = value.refresh();
   BOOST_TEST(ready(refreshed).size() == 3U);
   BOOST_TEST(value.requests().size() == 2U);
   BOOST_TEST(value.owner()->stats().renewals == 1U);
   BOOST_TEST(value.owner()->stats().automatic_reservations == 2U);
}

BOOST_AUTO_TEST_CASE(concurrent_refresh_callers_share_one_reservation_attempt) {
   auto value = fixture{policy()};
   auto state = fixture::snapshot({1});
   value.set_snapshot(state);
   value.start();
   value.wait_started(1);
   auto callers = std::vector<std::future<fixture::result>>{};
   for (auto index = 0; index < 8; ++index) { callers.push_back(value.refresh()); }
   value.wait_state([](const auto& stats) { return stats.waiting_refreshes == 8; });
   BOOST_TEST(value.requests().size() == 1U);
   state.reservations = {fixture::lease(fixture::candidate(1))};
   value.set_snapshot(state);
   value.release(0);
   for (auto& caller : callers) {
      const auto result = ready(caller);
      BOOST_REQUIRE_EQUAL(result.size(), 1U);
      BOOST_TEST(result.front().relay_peer.value == fixture::candidate(1).peer.value);
   }
   BOOST_TEST(value.owner()->stats().waiting_refreshes == 0U);
   BOOST_TEST(value.owner()->stats().attempts == 1U);
}

BOOST_AUTO_TEST_CASE(public_state_suppresses_initial_work_and_cancels_pending_work) {
   auto value = fixture{policy()};
   value.set_snapshot(fixture::snapshot({1}, false));
   value.start();
   auto suppressed = value.refresh();
   BOOST_TEST(ready(suppressed).empty());
   BOOST_TEST(value.requests().empty());
   value.set_snapshot(fixture::snapshot({1}, true, 2));
   value.owner()->notify();
   value.wait_started(1);
   value.set_snapshot(fixture::snapshot({1}, false, 3));
   value.owner()->notify();
   value.wait_canceled(1);
   value.wait_state([](const auto& stats) { return !stats.permitted && stats.pending_reservations == 0; });
   auto refreshed = value.refresh();
   BOOST_TEST(ready(refreshed).empty());
   BOOST_TEST(value.requests().size() == 1U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
}

BOOST_AUTO_TEST_CASE(late_failure_after_stop_cannot_create_retry_pending) {
   auto value = fixture{policy()};
   value.hold_canceled();
   value.set_snapshot(fixture::snapshot({1}));
   value.start();
   value.wait_started(1);
   auto stopped = value.stop(true);
   ready(stopped);
   value.wait_canceled(1);
   auto joined = value.join();
   auto tracked = value.tracked();
   BOOST_CHECK(joined.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(tracked.wait_for(0s) == std::future_status::timeout);
   value.release(0, true);
   ready(joined);
   ready(tracked);
   value.owner()->notify();
   auto rejected = value.refresh();
   BOOST_CHECK_THROW(ready(rejected), p2p::exceptions::closed);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(!value.owner()->stats().running);
   BOOST_TEST(value.requests().size() == 1U);
}

BOOST_AUTO_TEST_CASE(canceled_reservation_has_no_failure_backoff) {
   auto value = fixture{policy()};
   auto state = fixture::snapshot({1});
   value.set_snapshot(state);
   value.start();
   value.wait_started(1);
   value.requests().front().cancellation->request_stop();
   value.wait_canceled(1);
   // The fixed clock never advances: any failure backoff would prevent this retry.
   value.wait_started(2);
   state.reservations = {fixture::lease(fixture::candidate(1))};
   value.set_snapshot(state);
   value.release(1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0; });
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(!value.owner()->stats().last_error.has_value());
   BOOST_TEST(value.requests().size() == 2U);
}

BOOST_AUTO_TEST_CASE(candidate_set_remains_bounded_under_many_duplicate_hints) {
   auto value = fixture{policy(1, 1, 3)};
   auto state = fixture::snapshot({});
   for (auto id = std::uint8_t{1}; id <= 32; ++id) {
      auto source = fixture::candidate(id);
      source.session_id = 0;
      state.candidates.push_back(source);
      state.candidates.push_back(source);
   }
   value.set_snapshot(state);
   value.start();
   value.wait_started(1);
   BOOST_TEST(value.owner()->stats().candidates == 3U);
   BOOST_TEST(value.owner()->stats().max_candidates == 3U);
   BOOST_TEST(value.requests().size() == 1U);
   auto stopped = value.stop(true);
   ready(stopped);
   auto joined = value.join();
   ready(joined);
   BOOST_TEST(value.owner()->stats().candidates <= 3U);
}

BOOST_AUTO_TEST_CASE(all_refresh_waiters_wake_before_stopped_children_finish) {
   auto value = fixture{policy()};
   value.hold_canceled();
   value.set_snapshot(fixture::snapshot({1}));
   value.start();
   value.wait_started(1);
   auto callers = std::vector<std::future<fixture::result>>{};
   for (auto index = 0; index < 6; ++index) { callers.push_back(value.refresh(1min)); }
   value.wait_state([](const auto& stats) { return stats.waiting_refreshes == 6; });
   auto stopped = value.stop(true);
   ready(stopped);
   for (auto& caller : callers) { BOOST_CHECK_THROW(ready(caller), p2p::exceptions::closed); }
   BOOST_TEST(value.owner()->stats().waiting_refreshes == 0U);
   auto joined = value.join();
   BOOST_CHECK(joined.wait_for(0s) == std::future_status::timeout);
   value.release(0);
   ready(joined);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
}

BOOST_AUTO_TEST_CASE(startup_snapshot_error_propagates_and_releases_lifecycle_ticket) {
   auto value = fixture{policy()};
   value.fail_snapshot();
   value.start();
   auto joined = value.join();
   BOOST_CHECK_EXCEPTION(ready(joined), std::logic_error,
                         [](const auto& error) { return std::string{error.what()} == "scripted snapshot failure"; });
   auto stopped = value.stop(true);
   ready(stopped);
   auto tracked = value.tracked();
   ready(tracked);
   BOOST_TEST(!value.owner()->stats().running);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.requests().empty());
}

BOOST_AUTO_TEST_CASE(snapshot_error_with_pending_children_drains_before_join_and_tracker_release) {
   auto value = fixture{policy(3, 3, 3)};
   value.hold_canceled();
   value.set_snapshot(fixture::snapshot({1, 2, 3}));
   value.start();
   value.wait_started(3);
   auto joined = value.join();
   auto tracked = value.tracked_state();
   value.fail_snapshot();
   value.owner()->notify();
   value.wait_canceled(3);
   auto stopped = value.stop(true);
   ready(stopped);
   BOOST_CHECK(joined.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(tracked.wait_for(0s) == std::future_status::timeout);
   BOOST_TEST(value.owner()->stats().running);
   BOOST_TEST(value.owner()->stats().stopping);
   for (auto index = std::size_t{}; index < 2; ++index) {
      value.release(index);
      value.wait_state([index](const auto& stats) { return stats.pending_reservations == 2 - index; });
      BOOST_CHECK(joined.wait_for(0s) == std::future_status::timeout);
      BOOST_CHECK(tracked.wait_for(0s) == std::future_status::timeout);
   }
   value.release(2);
   const auto terminal = ready(tracked);
   BOOST_TEST(!terminal.running);
   BOOST_TEST(terminal.stopping);
   BOOST_TEST(!terminal.permitted);
   BOOST_TEST(terminal.pending_reservations == 0U);
   BOOST_CHECK_EXCEPTION(ready(joined), std::logic_error,
                         [](const auto& error) { return std::string{error.what()} == "scripted snapshot failure"; });
   BOOST_TEST(!value.owner()->stats().running);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(value.requests().size() == 3U);
   BOOST_TEST(std::ranges::all_of(value.requests(), [](const auto& item) { return item.canceled && item.completed; }));
}

BOOST_AUTO_TEST_CASE(tracker_wait_observes_terminal_state_before_join_is_consumed) {
   auto value = fixture{policy(2, 2, 2)};
   value.hold_canceled();
   value.set_snapshot(fixture::snapshot({1, 2}));
   value.start();
   value.wait_started(2);
   auto tracked = value.tracked_state();
   auto joined = value.join();
   auto stopped = value.stop(true);
   ready(stopped);
   value.wait_canceled(2);
   BOOST_CHECK(tracked.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(joined.wait_for(0s) == std::future_status::timeout);
   value.release(0);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 1; });
   BOOST_CHECK(tracked.wait_for(0s) == std::future_status::timeout);
   value.release(1);
   const auto terminal = ready(tracked);
   BOOST_TEST(!terminal.running);
   BOOST_TEST(terminal.stopping);
   BOOST_TEST(!terminal.permitted);
   BOOST_TEST(terminal.pending_reservations == 0U);
   ready(joined);
   BOOST_TEST(!value.owner()->stats().running);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
}

BOOST_AUTO_TEST_CASE(immediate_completion_wakes_manual_refresh_and_stop_without_extra_notify) {
   auto value = fixture{policy()};
   value.immediate_success();
   value.set_snapshot(fixture::snapshot({1}, false));
   value.start();
   value.wait_state([](const auto& stats) { return stats.running && !stats.permitted; });
   value.set_snapshot(fixture::snapshot({1}, true, 2));
   auto refreshed = value.refresh();
   BOOST_TEST(ready(refreshed).size() == 1U);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 1; });
   BOOST_TEST(value.requests().size() == 1U);
   auto stopped = value.stop(true);
   ready(stopped);
   auto joined = value.join();
   auto tracked = value.tracked();
   ready(joined);
   ready(tracked);
}

BOOST_AUTO_TEST_CASE(public_transition_notified_inside_current_is_not_lost) {
   auto value = fixture{policy()};
   value.set_snapshot(fixture::snapshot({1}));
   value.start();
   value.wait_started(1);
   value.after_snapshot([&] {
      value.set_snapshot(fixture::snapshot({1}, false, 2));
      value.owner()->notify();
   });
   value.owner()->notify();
   // No further manager trigger is allowed to rescue the in-tick notification.
   value.wait_canceled(1);
   value.wait_state([](const auto& stats) { return !stats.permitted && stats.pending_reservations == 0; });
   BOOST_TEST(value.requests().size() == 1U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
}

BOOST_AUTO_TEST_CASE(refresh_callers_keep_independent_deadline_budgets) {
   auto value = fixture{policy()};
   value.set_snapshot(fixture::snapshot({1}));
   value.start();
   value.wait_started(1);
   auto long_waiter = value.refresh(1min);
   value.wait_state([](const auto& stats) { return stats.waiting_refreshes == 1; });
   auto short_waiter = value.refresh(30ms);
   BOOST_CHECK_THROW(ready(short_waiter), p2p::exceptions::timeout);
   BOOST_CHECK(long_waiter.wait_for(0s) == std::future_status::timeout);
   BOOST_TEST(value.owner()->stats().waiting_refreshes == 1U);
   BOOST_TEST(value.requests().size() == 1U);
   BOOST_TEST(!value.requests().front().cancellation->stop_requested());
   auto stopped = value.stop();
   ready(stopped);
   BOOST_CHECK_THROW(ready(long_waiter), p2p::exceptions::closed);
}

BOOST_AUTO_TEST_CASE(reentrant_stop_from_cancel_callback_reaches_every_pending_child) {
   auto value = fixture{policy(4, 4, 4)};
   value.set_snapshot(fixture::snapshot({1, 2, 3, 4}));
   value.start();
   value.wait_started(4);
   value.on_cancel([owner = value.owner()] { owner->request_stop(); });
   value.set_snapshot(fixture::snapshot({}, false, 2));
   auto stopped = value.stop();
   ready(stopped);
   value.wait_canceled(4);
   auto joined = value.join();
   ready(joined);
   const auto requests = value.requests();
   BOOST_TEST(std::ranges::all_of(requests, [](const auto& item) { return item.canceled && item.completed; }));
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
}

BOOST_AUTO_TEST_SUITE_END()
