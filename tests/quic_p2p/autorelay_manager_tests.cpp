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
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

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

p2p::relay::policy cooldown_policy(std::size_t candidates = 16) {
   auto value = policy(1, 1, candidates);
   value.candidate_backoff = 1s;
   return value;
}

void check_refresh_during_cooldown(fixture& value) {
   for (auto round = 0; round < 3; ++round) {
      value.owner()->notify();
      auto refreshed = value.refresh(100ms);
      auto result = fixture::result{};
      BOOST_CHECK_NO_THROW(result = ready(refreshed));
      BOOST_TEST(result.empty());
      BOOST_TEST(value.owner()->stats().attempts == 1U);
      BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   }
}

void check_immediate_completion_cooldown(std::optional<p2p::exceptions::code> error) {
   auto value = fixture{cooldown_policy()};
   auto state = fixture::snapshot({1});
   if (error) { value.immediate_error(*error); }
   else { value.immediate_unowned_success(); }
   value.set_snapshot(state);
   value.start();
   // No notification or manual refresh may rescue the synchronous completion/reap.
   value.wait_state([](const auto& stats) { return stats.failures + stats.invalidated_completions + stats.successes == 1; });
   const auto cooled = value.owner()->stats();
   BOOST_TEST(cooled.attempts == 1U);
   BOOST_TEST(cooled.pending_reservations == 0U);
   BOOST_TEST(cooled.failures == (error ? 1U : 0U));
   BOOST_TEST(cooled.invalidated_completions == (error ? 0U : 1U));
   BOOST_TEST(cooled.successes == 0U);
   BOOST_CHECK(cooled.last_error == error);
   const auto selection = value.owner()->selection();
   BOOST_TEST(std::ranges::count(selection.unavailable, fixture::candidate(1).peer) == 1);
   BOOST_TEST(value.peak_active() == 1U);
   // Ordinary first-failure jitter is 750..1000ms; owner/missing-lease cooldown is 1s.
   value.advance_clock(500ms);
   check_refresh_during_cooldown(value);

   value.advance_clock(500ms);
   value.owner()->notify();
   value.wait_started(2);
   BOOST_TEST(value.owner()->stats().attempts == 2U);
   state.reservations = {fixture::lease(fixture::candidate(1))};
   value.set_snapshot(state);
   value.release(1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 1; });
   BOOST_TEST(value.requests().size() == 2U);
   BOOST_TEST(value.owner()->stats().failures == (error ? 1U : 0U));
   BOOST_CHECK(value.owner()->stats().last_error == error);
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
   BOOST_TEST(!value.owner()->stats().last_error.has_value());
   BOOST_TEST(!value.owner()->stats().running);
   BOOST_TEST(value.requests().size() == 1U);
   BOOST_TEST(value.owner()->selection().unavailable.empty());
}

BOOST_AUTO_TEST_CASE(canceled_reservation_has_scheduling_cooldown_without_network_failure) {
   auto value = fixture{cooldown_policy()};
   auto state = fixture::snapshot({1});
   value.set_snapshot(state);
   value.start();
   value.wait_started(1);
   value.requests().front().cancellation->request_stop();
   value.wait_canceled(1);
   value.wait_state([](const auto& stats) { return stats.invalidated_completions == 1; });
   BOOST_TEST(value.owner()->stats().attempts == 1U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   const auto selection = value.owner()->selection();
   BOOST_TEST(std::ranges::count(selection.unavailable, fixture::candidate(1).peer) == 1);
   check_refresh_during_cooldown(value);
   value.advance_clock(1s);
   value.owner()->notify();
   value.wait_started(2);
   state.reservations = {fixture::lease(fixture::candidate(1))};
   value.set_snapshot(state);
   value.release(1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0; });
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(!value.owner()->stats().last_error.has_value());
   BOOST_TEST(value.requests().size() == 2U);
}

BOOST_AUTO_TEST_CASE(synchronous_canceled_completion_cools_stable_candidate_without_extra_notify) {
   check_immediate_completion_cooldown(p2p::exceptions::code::canceled);
}

BOOST_AUTO_TEST_CASE(synchronous_closed_completion_cools_stable_candidate_without_extra_notify) {
   check_immediate_completion_cooldown(p2p::exceptions::code::closed);
}

BOOST_AUTO_TEST_CASE(synchronous_success_without_owned_lease_cools_stable_candidate_without_extra_notify) {
   check_immediate_completion_cooldown(std::nullopt);
}

BOOST_AUTO_TEST_CASE(completion_cooldown_survives_candidate_rotation_and_new_session) {
   for (const auto error : std::initializer_list<std::optional<p2p::exceptions::code>>{
            p2p::exceptions::code::canceled, p2p::exceptions::code::closed, std::nullopt}) {
      auto value = fixture{cooldown_policy(1)};
      if (error) { value.immediate_error(*error); }
      value.set_snapshot(fixture::snapshot({1}));
      value.start();
      if (!error) {
         value.wait_started(1);
         value.requests().front().cancellation->request_stop();
         value.wait_canceled(1);
      }
      value.wait_state([](const auto& stats) { return stats.failures + stats.invalidated_completions == 1; });

      value.set_snapshot(fixture::snapshot({}));
      value.owner()->notify();
      value.wait_state([](const auto& stats) { return stats.candidates == 0 && stats.pending_reservations == 0; });
      const auto absent = value.owner()->selection();
      BOOST_TEST(std::ranges::count(absent.unavailable, fixture::candidate(1).peer) == 1);

      value.advance_clock(500ms);
      auto state = fixture::snapshot({1});
      state.candidates.front().session_id = 99;
      value.set_snapshot(state);
      value.owner()->notify();
      value.wait_state([](const auto& stats) { return stats.candidates == 1; });
      BOOST_TEST(value.owner()->stats().attempts == 1U);
      BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
      const auto returned = value.owner()->selection();
      BOOST_TEST(std::ranges::count(returned.unavailable, fixture::candidate(1).peer) == 1);
      check_refresh_during_cooldown(value);

      value.advance_clock(500ms);
      value.owner()->notify();
      value.wait_started(2);
      const auto requests = value.requests();
      BOOST_REQUIRE_EQUAL(requests.size(), 2U);
      BOOST_TEST(requests[1].source.session_id == 99U);
      state.reservations = {fixture::lease(state.candidates.front())};
      value.set_snapshot(state);
      value.release(1);
      value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 1; });
      BOOST_TEST(value.owner()->stats().failures == (error ? 1U : 0U));
      BOOST_CHECK(value.owner()->stats().last_error == error);
   }
}

BOOST_AUTO_TEST_CASE(cancel_rotated_peer_extends_acquisition_cooldown_without_resetting_failure_history) {
   auto limits = policy(1, 1, 1);
   limits.candidate_backoff = 4s;
   auto value = fixture{limits};
   value.use_selection();
   value.immediate_error(p2p::exceptions::code::closed);
   value.set_snapshot(fixture::snapshot({1}));
   value.start();
   value.wait_state([](const auto& stats) {
      return stats.failures == 1 && stats.candidates == 0 && stats.pending_reservations == 0;
   });
   const auto peer = fixture::candidate(1).peer;
   BOOST_TEST(value.owner()->stats().attempts == 1U);
   BOOST_TEST(std::ranges::count(value.owner()->selection().unavailable, peer) == 1);

   // Both cancellations occur while A is retained only in retry history.
   value.owner()->cancel_peer(peer);
   value.advance_clock(500ms);
   value.owner()->cancel_peer(peer);
   value.advance_clock(1500ms);
   // The original 750..1000ms network backoff has expired, but acquisition is still canceled.
   check_refresh_during_cooldown(value);
   BOOST_TEST(value.owner()->stats().failures == 1U);
   BOOST_TEST(value.owner()->stats().invalidated_completions == 0U);
   BOOST_CHECK(value.owner()->stats().last_error == p2p::exceptions::code::closed);

   value.advance_clock(2s);
   // Repeated cancellation extended the fixed cooldown from t=4s to t=4.5s.
   check_refresh_during_cooldown(value);
   value.advance_clock(500ms);
   value.owner()->notify();
   value.wait_started(2);
   const auto requests = value.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_TEST(requests[1].source.peer.value == peer.value);
   value.release(1, true);
   value.wait_state([](const auto& stats) { return stats.failures == 2 && stats.pending_reservations == 0; });

   // Cancellation must retain the second failure's 1500..2000ms tier, not reset it to <=1s.
   value.advance_clock(1s);
   value.owner()->notify();
   auto refreshed = value.refresh(100ms);
   auto result = fixture::result{};
   BOOST_CHECK_NO_THROW(result = ready(refreshed));
   BOOST_TEST(result.empty());
   BOOST_TEST(value.owner()->stats().attempts == 2U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().failures == 2U);
   BOOST_TEST(value.owner()->stats().invalidated_completions == 0U);
   BOOST_CHECK(value.owner()->stats().last_error == p2p::exceptions::code::timeout);
   BOOST_TEST(value.requests().size() == 2U);
   BOOST_TEST(value.peak_active() == 1U);
}

BOOST_AUTO_TEST_CASE(terminal_error_selection_admits_healthy_spare_before_reap) {
   for (const auto error : std::initializer_list<std::optional<p2p::exceptions::code>>{
            p2p::exceptions::code::canceled, p2p::exceptions::code::closed, std::nullopt}) {
      auto value = fixture{policy(1, 1, 1)};
      value.use_selection();
      if (error) { value.immediate_error(*error); }
      else { value.immediate_unowned_success(); }
      value.immediate_success();
      value.set_snapshot(fixture::snapshot({1, 2}));
      value.start();
      value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 1; });
      const auto requests = value.requests();
      BOOST_REQUIRE_EQUAL(requests.size(), 2U);
      BOOST_TEST(requests[0].source.peer.value == fixture::candidate(1).peer.value);
      BOOST_TEST(requests[1].source.peer.value == fixture::candidate(2).peer.value);
      BOOST_TEST(value.owner()->stats().attempts == 2U);
      BOOST_TEST(value.owner()->stats().candidates == 1U);
      BOOST_TEST(value.owner()->stats().failures == (error ? 1U : 0U));
      BOOST_CHECK(value.owner()->stats().last_error == error);
   }
}

BOOST_AUTO_TEST_CASE(unexpired_retry_history_survives_capacity_pressure_and_refresh) {
   auto value = fixture{policy(1, 1, 1)};
   value.use_selection();
   value.immediate_error(p2p::exceptions::code::closed);
   value.set_snapshot(fixture::snapshot({1, 2}));
   value.start();
   value.wait_started(2);
   const auto requests = value.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_TEST(requests[0].source.peer.value == fixture::candidate(1).peer.value);
   BOOST_TEST(requests[1].source.peer.value == fixture::candidate(2).peer.value);
   value.release(1, true);
   value.wait_state([](const auto& stats) { return stats.failures == 2 && stats.pending_reservations == 0; });
   BOOST_TEST(value.owner()->stats().attempts == 2U);
   BOOST_TEST(value.owner()->stats().candidates <= 1U);
   BOOST_CHECK(value.owner()->stats().last_error == p2p::exceptions::code::timeout);
   const auto selection = value.owner()->selection();
   BOOST_TEST(std::ranges::count(selection.unavailable, fixture::candidate(1).peer) == 1);
   BOOST_TEST(std::ranges::count(selection.unavailable, fixture::candidate(2).peer) == 1);
   BOOST_TEST(selection.unavailable.size() <= 2U);

   // The fake clock never advances: capacity pressure must not evict either live cooldown.
   for (auto round = 0; round < 3; ++round) {
      value.owner()->notify();
      auto refreshed = value.refresh(100ms);
      auto result = fixture::result{};
      BOOST_CHECK_NO_THROW(result = ready(refreshed));
      BOOST_TEST(result.empty());
      BOOST_TEST(value.owner()->stats().attempts == 2U);
      BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
      BOOST_TEST(value.owner()->stats().failures == 2U);
   }
   BOOST_TEST(value.requests().size() == 2U);
   BOOST_TEST(value.peak_active() == 1U);
}

BOOST_AUTO_TEST_CASE(restored_live_owned_lease_renews_when_unknown_to_full_candidate_and_retry_maps) {
   auto value = fixture{policy(2, 1, 2)};
   value.use_selection();
   auto lease = fixture::lease(fixture::candidate(5));
   lease.info.ttl = 800ms;
   lease.expires_at = fixture::now() + 800ms;
   lease.renew_at = fixture::now() + 600ms;
   auto state = fixture::snapshot({5});
   value.set_snapshot(state);
   value.start();
   value.wait_started(1);
   state.reservations = {lease};
   value.set_snapshot(state);
   value.release(0);
   value.wait_state([](const auto& stats) {
      return stats.successes == 1 && stats.reservations == 1 && stats.candidates == 1 &&
          stats.pending_reservations == 0;
   });

   // Loss of HOP removes C from candidates, but does not withdraw its still-live lease.
   state.candidates = fixture::snapshot({1, 2, 3, 4}).candidates;
   value.immediate_error(p2p::exceptions::code::closed);
   value.set_snapshot(state);
   value.owner()->notify();
   for (auto index = std::size_t{2}; index < 4; ++index) {
      value.wait_started(index + 1);
      value.release(index, true);
   }
   value.wait_state([](const auto& stats) {
      return stats.failures == 3 && stats.candidates == 2;
   });
   // Retaining absent C consumes one slot; otherwise one more already-admitted peer completes.
   if (value.owner()->stats().pending_reservations != 0) {
      value.wait_started(5);
      value.release(4, true);
      value.wait_state([](const auto& stats) { return stats.failures == 4 && stats.pending_reservations == 0; });
   }
   const auto failures = static_cast<std::size_t>(value.owner()->stats().failures);
   BOOST_TEST(failures >= 3U);
   BOOST_TEST(failures <= 4U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().attempts == failures + 1);
   BOOST_TEST(value.owner()->stats().reservations == 1U);
   const auto cooling = value.owner()->selection();
   BOOST_TEST(cooling.unavailable.size() == failures);
   for (auto id = std::uint8_t{1}; id <= failures; ++id) {
      BOOST_TEST(std::ranges::count(cooling.unavailable, fixture::candidate(id).peer) == 1);
   }
   BOOST_TEST(std::ranges::count(cooling.unavailable, fixture::candidate(5).peer) == 0);

   // All first-failure backoffs are still future (>=750ms), while C is live and due.
   value.advance_clock(600ms);
   state.candidates = fixture::snapshot({5, 1, 2, 3, 4}).candidates;
   value.set_snapshot(state);
   value.owner()->notify();
   value.wait_started(failures + 2);
   const auto requests = value.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), failures + 2);
   BOOST_TEST(requests[failures + 1].source.peer.value == fixture::candidate(5).peer.value);
   BOOST_TEST(value.owner()->stats().candidates <= 2U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 1U);
   BOOST_TEST(value.owner()->stats().max_candidates == 2U);
   BOOST_TEST(value.peak_active() == 1U);

   lease.expires_at += 600ms;
   lease.renew_at += 600ms;
   state.reservations = {lease};
   value.set_snapshot(state);
   value.release(failures + 1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 2; });
   BOOST_TEST(value.owner()->stats().renewals == 1U);
   BOOST_TEST(value.owner()->stats().failures == failures);
   BOOST_TEST(value.owner()->stats().invalidated_completions == 0U);
   BOOST_TEST(value.owner()->stats().candidates <= 2U);
   BOOST_TEST(value.owner()->selection().unavailable.size() <= 4U);
}

BOOST_AUTO_TEST_CASE(invalidated_generation_completion_cools_without_network_failure) {
   auto value = fixture{cooldown_policy()};
   value.hold_canceled();
   value.set_snapshot(fixture::snapshot({1}));
   value.start();
   value.wait_started(1);
   value.set_snapshot(fixture::snapshot({1}, true, 2));
   value.owner()->notify();
   value.wait_canceled(1);
   value.release(0, true);
   value.wait_state([](const auto& stats) { return stats.invalidated_completions == 1; });
   BOOST_TEST(value.owner()->stats().attempts == 1U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(!value.owner()->stats().last_error.has_value());
   const auto selection = value.owner()->selection();
   BOOST_TEST(std::ranges::count(selection.unavailable, fixture::candidate(1).peer) == 1);
   value.hold_canceled(false);
   auto stopped = value.stop(true);
   ready(stopped);
   auto joined = value.join();
   auto tracked = value.tracked();
   ready(joined);
   ready(tracked);
   BOOST_TEST(value.owner()->stats().failures == 0U);
}

BOOST_AUTO_TEST_CASE(completed_cancellation_observes_latest_owner_policy_before_reap) {
   for (const auto permitted : {true, false}) {
      auto value = fixture{cooldown_policy()};
      value.immediate_error(p2p::exceptions::code::canceled);
      value.set_snapshot(fixture::snapshot({1}));
      // Same-strand synchronous callback: only its terminal notification can drive the next tick.
      value.before_immediate_completion([&] {
         value.set_snapshot(fixture::snapshot({1}, permitted, permitted ? 2 : 1));
      });
      value.start();
      value.wait_state([](const auto& stats) { return stats.failures + stats.invalidated_completions == 1; });
      const auto stats = value.owner()->stats();
      BOOST_TEST(stats.permitted == permitted);
      BOOST_TEST(stats.attempts == 1U);
      BOOST_TEST(stats.pending_reservations == 0U);
      BOOST_TEST(stats.invalidated_completions == 1U);
      BOOST_TEST(stats.failures == 0U);
      BOOST_TEST(!stats.last_error.has_value());
      BOOST_TEST(value.requests().front().completed);
      BOOST_TEST(!value.requests().front().cancellation->stop_requested());
      const auto selection = value.owner()->selection();
      BOOST_TEST(std::ranges::count(selection.unavailable, fixture::candidate(1).peer) == 1);

      value.advance_clock(500ms);
      auto state = fixture::snapshot({1}, true, 2);
      value.set_snapshot(state);
      check_refresh_during_cooldown(value);
      value.advance_clock(500ms);
      value.owner()->notify();
      value.wait_started(2);
      BOOST_TEST(value.requests()[1].generation == 2U);
      state.reservations = {fixture::lease(fixture::candidate(1))};
      value.set_snapshot(state);
      value.release(1);
      value.wait_state([](const auto& current) { return current.pending_reservations == 0 && current.successes == 1; });
      BOOST_TEST(value.owner()->stats().failures == 0U);
   }
}

BOOST_AUTO_TEST_CASE(committed_live_lease_survives_generation_change_and_renews_on_time) {
   auto value = fixture{policy(1, 1, 1)};
   value.set_snapshot(fixture::snapshot({2}));
   value.start();
   value.wait_started(1);

   auto state = fixture::snapshot({2}, true, 2);
   auto lease = fixture::lease(fixture::candidate(2));
   lease.info.ttl = 60s;
   lease.expires_at = fixture::now() + 60s;
   lease.renew_at = fixture::now() + 45s;
   state.reservations = {lease};
   // Publish committed ownership and a new global generation before the terminal callback.
   // No manager notify: completion alone must drive the next snapshot/reap.
   value.set_snapshot(state);
   value.release(0);
   value.wait_state([](const auto& stats) {
      return stats.pending_reservations == 0 && stats.successes + stats.invalidated_completions + stats.failures == 1;
   });
   BOOST_TEST(value.owner()->stats().successes == 1U);
   BOOST_TEST(value.owner()->stats().invalidated_completions == 0U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(!value.owner()->stats().last_error.has_value());

   value.advance_clock(45s);
   value.owner()->notify();
   value.wait_started(2);
   const auto requests = value.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_TEST(requests[1].source.peer.value == fixture::candidate(2).peer.value);
   BOOST_TEST(requests[1].generation == 2U);
   BOOST_TEST(value.owner()->stats().attempts == 2U);

   lease.expires_at += 45s;
   lease.renew_at += 45s;
   state.reservations = {lease};
   value.set_snapshot(state);
   value.release(1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 2; });
   BOOST_TEST(value.owner()->stats().renewals == 1U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
}

BOOST_AUTO_TEST_CASE(invalidated_inflight_renewal_keeps_live_lease_schedule_under_new_generation) {
   auto value = fixture{policy(1, 1, 1)};
   value.use_selection();
   value.hold_canceled();
   auto state = fixture::snapshot({2});
   auto lease = fixture::lease(fixture::candidate(2));
   lease.info.ttl = 60s;
   lease.expires_at = fixture::now() + 60s;
   lease.renew_at = fixture::now() + 45s;
   state.reservations = {lease};
   value.set_snapshot(state);
   value.start();
   value.wait_state([](const auto& stats) {
      return stats.running && stats.reservations == 1 && stats.pending_reservations == 0;
   });
   value.advance_clock(45s);
   value.owner()->notify();
   value.wait_started(1);
   BOOST_TEST(value.requests().front().generation == 1U);

   state.generation = 2;
   value.set_snapshot(state);
   value.owner()->notify();
   value.wait_canceled(1);
   value.wait_cancellation_observed(1);
   value.release(0, true);
   value.wait_state([](const auto& stats) { return stats.invalidated_completions == 1; });
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(!value.owner()->stats().last_error.has_value());
   // The old grant is still live and due; stable generation 2 must restart its renewal.
   value.wait_started(2);
   const auto requests = value.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_TEST(requests[1].source.peer.value == fixture::candidate(2).peer.value);
   BOOST_TEST(requests[1].generation == 2U);
   BOOST_TEST(value.owner()->stats().attempts == 2U);
   BOOST_TEST(value.peak_active() == 1U);

   lease.expires_at += 45s;
   lease.renew_at += 45s;
   state.reservations = {lease};
   value.set_snapshot(state);
   value.release(1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 1; });
   BOOST_TEST(value.owner()->stats().renewals == 1U);
   BOOST_TEST(value.owner()->stats().invalidated_completions == 1U);
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(!value.owner()->stats().last_error.has_value());
}

BOOST_AUTO_TEST_CASE(success_requires_same_live_automatic_lease_identity) {
   for (const auto generation : {1U, 2U}) {
      for (const auto invalid_proof : {"mismatched_id", "manual", "expired"}) {
         BOOST_TEST_CONTEXT("generation=" << generation << " proof=" << invalid_proof) {
            auto value = fixture{policy(1, 1, 1)};
            value.set_snapshot(fixture::snapshot({2}));
            value.start();
            value.wait_started(1);
            auto state = fixture::snapshot({2}, true, generation);
            auto lease = fixture::lease(fixture::candidate(2));
            if (std::string{invalid_proof} == "mismatched_id") { ++lease.info.id; }
            else if (std::string{invalid_proof} == "manual") { lease.automatic = false; }
            else { lease.expires_at = fixture::now(); }
            state.reservations = {lease};
            value.set_snapshot(state);
            value.release(0);
            value.wait_state([](const auto& stats) {
               return stats.pending_reservations == 0 &&
                   stats.successes + stats.invalidated_completions + stats.failures == 1;
            });
            BOOST_TEST(value.owner()->stats().attempts == 1U);
            BOOST_TEST(value.owner()->stats().successes == 0U);
            BOOST_TEST(value.owner()->stats().invalidated_completions == 1U);
            BOOST_TEST(value.owner()->stats().failures == 0U);
            BOOST_TEST(!value.owner()->stats().last_error.has_value());
            const auto selection = value.owner()->selection();
            BOOST_TEST(std::ranges::count(selection.unavailable, fixture::candidate(2).peer) == 1);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(unsolicited_closed_with_live_lease_and_generation_drift_remains_failure) {
   auto value = fixture{policy(1, 1, 1)};
   value.use_selection();
   auto state = fixture::snapshot({2});
   state.reservations = {fixture::lease(fixture::candidate(2), true)};
   value.set_snapshot(state);
   value.immediate_error(p2p::exceptions::code::closed);
   value.before_immediate_completion([&] {
      state.generation = 2;
      value.set_snapshot(state);
   });
   value.start();
   value.wait_state([](const auto& stats) {
      return stats.pending_reservations == 0 && stats.failures + stats.invalidated_completions == 1;
   });
   const auto failed = value.owner()->stats();
   BOOST_TEST(failed.attempts == 1U);
   BOOST_TEST(failed.failures == 1U);
   BOOST_TEST(failed.invalidated_completions == 0U);
   BOOST_CHECK(failed.last_error == p2p::exceptions::code::closed);
   BOOST_TEST(value.requests().front().completed);
   BOOST_TEST(!value.requests().front().cancellation->stop_requested());

   value.advance_clock(500ms);
   value.owner()->notify();
   auto refreshed = value.refresh(100ms);
   BOOST_TEST(ready(refreshed).size() == 1U);
   BOOST_TEST(value.owner()->stats().attempts == 1U);
   value.advance_clock(500ms);
   value.owner()->notify();
   value.wait_started(2);
   BOOST_TEST(value.requests()[1].generation == 2U);
   state.reservations = {fixture::lease(fixture::candidate(2))};
   value.set_snapshot(state);
   value.release(1);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 0 && stats.successes == 1; });
   BOOST_TEST(value.owner()->stats().renewals == 1U);
   BOOST_TEST(value.owner()->stats().failures == 1U);
   BOOST_CHECK(value.owner()->stats().last_error == p2p::exceptions::code::closed);
}

BOOST_AUTO_TEST_CASE(success_without_owned_lease_preserves_prior_failure_history) {
   auto limits = policy();
   limits.candidate_backoff = 4s;
   auto value = fixture{limits};
   value.immediate_error(p2p::exceptions::code::timeout);
   value.set_snapshot(fixture::snapshot({1}));
   value.start();
   value.wait_state([](const auto& stats) { return stats.failures == 1; });
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);

   value.immediate_unowned_success();
   value.advance_clock(1s);
   value.owner()->notify();
   value.wait_state([](const auto& stats) { return stats.invalidated_completions + stats.successes == 1; });
   BOOST_TEST(value.owner()->stats().attempts == 2U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().successes == 0U);
   BOOST_TEST(value.owner()->stats().invalidated_completions == 1U);
   BOOST_TEST(value.owner()->stats().failures == 1U);

   value.advance_clock(4s);
   value.owner()->notify();
   value.wait_started(3);
   value.release(2, true);
   value.wait_state([](const auto& stats) { return stats.failures == 2; });
   // Second genuine failure retains the 1500..2000ms tier, not the reset 750..1000ms tier.
   value.advance_clock(1s);
   value.owner()->notify();
   auto refreshed = value.refresh(100ms);
   auto result = fixture::result{};
   BOOST_CHECK_NO_THROW(result = ready(refreshed));
   BOOST_TEST(result.empty());
   BOOST_TEST(value.owner()->stats().attempts == 3U);
   BOOST_TEST(value.owner()->stats().pending_reservations == 0U);
   BOOST_TEST(value.owner()->stats().failures == 2U);
   BOOST_CHECK(value.owner()->stats().last_error == p2p::exceptions::code::timeout);
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

BOOST_AUTO_TEST_CASE(snapshot_bad_alloc_keeps_canceled_children_owned_until_completion) {
   auto value = fixture{policy(2, 2, 2)};
   value.hold_canceled();
   value.set_snapshot(fixture::snapshot({1, 2}));
   value.start();
   value.wait_started(2);
   auto refreshed = value.refresh(1min);
   value.wait_state([](const auto& stats) { return stats.waiting_refreshes == 1; });
   auto joined = value.join();
   auto tracked = value.tracked_state();

   // Script a callback error, not an allocator fault in the manager's drain waiter.
   value.fail_snapshot(std::make_exception_ptr(std::bad_alloc{}));
   value.owner()->notify();
   value.wait_cancellation_observed(2);
   BOOST_CHECK_THROW(ready(refreshed), p2p::exceptions::closed);
   BOOST_TEST(std::ranges::all_of(value.requests(), [](const auto& item) {
      return item.canceled && item.cancellation_observed && !item.released && !item.completed;
   }));
   auto late_joined = value.join();
   auto late_tracked = value.tracked_state();
   auto rejected = value.refresh();
   BOOST_CHECK_THROW(ready(rejected), p2p::exceptions::closed);
   // Tracker stop is required by wait(); parent failure already canceled the children.
   auto stopped = value.stop(true);
   ready(stopped);

   const auto check_retained = [&] {
      BOOST_CHECK(joined.wait_for(25ms) == std::future_status::timeout);
      BOOST_CHECK(late_joined.wait_for(25ms) == std::future_status::timeout);
      BOOST_CHECK(tracked.wait_for(25ms) == std::future_status::timeout);
      BOOST_CHECK(late_tracked.wait_for(25ms) == std::future_status::timeout);
      const auto stats = value.owner()->stats();
      BOOST_TEST(stats.running);
      BOOST_TEST(stats.stopping);
      BOOST_TEST(!stats.permitted);
   };
   check_retained();
   BOOST_TEST(value.owner()->stats().pending_reservations == 2U);
   value.release(0);
   value.wait_state([](const auto& stats) { return stats.pending_reservations == 1; });
   const auto requests = value.requests();
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_TEST(requests[0].completed);
   BOOST_TEST(!requests[1].completed);
   check_retained();

   value.release(1);
   for (auto* waiter : {&tracked, &late_tracked}) {
      const auto terminal = ready(*waiter);
      BOOST_TEST(!terminal.running);
      BOOST_TEST(terminal.stopping);
      BOOST_TEST(!terminal.permitted);
      BOOST_TEST(terminal.pending_reservations == 0U);
   }
   BOOST_CHECK_THROW(ready(joined), std::bad_alloc);
   BOOST_CHECK_THROW(ready(late_joined), std::bad_alloc);
   BOOST_TEST(value.owner()->stats().failures == 0U);
   BOOST_TEST(value.owner()->stats().waiting_refreshes == 0U);
   BOOST_TEST(value.requests().size() == 2U);
   BOOST_TEST(std::ranges::all_of(value.requests(), [](const auto& item) {
      return item.canceled && item.cancellation_observed && item.released && item.completed;
   }));
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
