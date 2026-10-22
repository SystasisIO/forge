#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

import forge.vm.wasm.interpret.watchdog;

namespace {
using forge::vm::wasm::interpret::watchdog;
using interruption = watchdog::interruption;
using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;
} // namespace

BOOST_AUTO_TEST_SUITE(watchdog_absolute_contract)

BOOST_AUTO_TEST_CASE(preexisting_requests_and_expiry_gate_work_with_canonical_priority) {
   BOOST_TEST(static_cast<unsigned>(interruption::deadline) == 0U);
   BOOST_TEST(static_cast<unsigned>(interruption::canceled) == 1U);
   for (unsigned mask = 0; mask != 8; ++mask) {
      auto cancel = std::stop_source{};
      auto external = std::stop_source{};
      if (mask & 1U)
         cancel.request_stop();
      if (mask & 4U)
         external.request_stop();
      const auto deadline = mask & 2U ? clock_type::now() - 1ms : clock_type::time_point::max();
      const auto expected = mask & 1U ? interruption::canceled
                            : mask & 2U ? interruption::deadline
                            : mask & 4U ? interruption::interrupted
                                        : interruption::none;
      auto reason = interruption::none;
      auto calls = 0U;
      auto work = 0U;
      {
         auto guard = watchdog{deadline, {cancel.get_token(), external.get_token()}}.scoped_run(
             [&](interruption value) {
                reason = value;
                ++calls;
             });
         if (guard.enter())
            ++work;
      }
      BOOST_CHECK(reason == expected);
      BOOST_TEST(calls == (expected == interruption::none ? 0U : 1U));
      BOOST_TEST(work == (expected == interruption::none ? 1U : 0U));
   }
}

BOOST_AUTO_TEST_CASE(absolute_watchdog_keeps_original_deadline_across_delayed_and_repeated_use) {
   const auto deadline = clock_type::now() + 10ms;
   auto timer = watchdog{deadline, {}};
   std::this_thread::sleep_until(deadline + 1ms);
   for (auto iteration = 0; iteration != 2; ++iteration) {
      auto reason = interruption::none;
      auto admitted = false;
      {
         auto guard = timer.scoped_run([&](interruption value) { reason = value; });
         admitted = guard.enter();
      }
      BOOST_TEST(!admitted);
      BOOST_CHECK(reason == interruption::deadline);
   }
}

BOOST_AUTO_TEST_CASE(duration_stop_stays_async_when_requester_holds_callback_mutex) {
   const auto requester = std::this_thread::get_id();
   for (const auto prestopped : {false, true}) {
      auto cancel = std::stop_source{};
      if (prestopped)
         cancel.request_stop();
      auto mutex = std::mutex{};
      auto inline_delivery = std::atomic_bool{false};
      auto finished = std::atomic_bool{false};
      auto calls = 0U;
      auto reason = interruption::none;
      auto value = 0U;
      auto observed = 0U;
      auto lock = std::unique_lock{mutex};
      {
         auto guard = watchdog{1h, cancel.get_token()}.scoped_run([&](interruption cause) {
            if (std::this_thread::get_id() == requester) {
               inline_delivery.store(true);
               return;
            }
            auto callback_lock = std::lock_guard{mutex};
            ++calls;
            reason = cause;
            observed = value;
            finished.store(true);
         });
         if (!prestopped)
            cancel.request_stop();
         const auto finished_while_locked = finished.load();
         value = 17U;
         lock.unlock();
         BOOST_TEST(!finished_while_locked);
      }
      BOOST_TEST(!inline_delivery.load());
      BOOST_TEST(finished.load());
      BOOST_TEST(calls == 1U);
      BOOST_TEST(observed == 17U);
      BOOST_CHECK(reason == interruption::canceled);
   }
}

BOOST_AUTO_TEST_CASE(absolute_stop_can_deliver_inline_and_reenter_without_changing_reason) {
   const auto requester = std::this_thread::get_id();
   for (const auto cancel_first : {false, true}) {
      auto cancel = std::stop_source{};
      auto external = std::stop_source{};
      auto calls = 0U;
      auto reason = interruption::none;
      auto inline_delivery = false;
      auto completed_before_return = false;
      {
         auto guard = watchdog{clock_type::time_point::max(), {cancel.get_token(), external.get_token()}}.scoped_run(
             [&](interruption cause) {
                ++calls;
                reason = cause;
                inline_delivery = std::this_thread::get_id() == requester;
                // Reenter the same watchdog through its other stop handler.
                (cancel_first ? external : cancel).request_stop();
             });
         BOOST_TEST(guard.enter());
         (cancel_first ? cancel : external).request_stop();
         completed_before_return = calls == 1U && inline_delivery;
         BOOST_TEST(!guard.enter());
      }
      BOOST_TEST(completed_before_return);
      BOOST_TEST(calls == 1U);
      BOOST_CHECK(reason == (cancel_first ? interruption::canceled : interruption::interrupted));
   }
}

BOOST_AUTO_TEST_CASE(deadline_delivery_is_async_even_when_admission_observes_expiry) {
   const auto requester = std::this_thread::get_id();
   auto calls = 0U;
   auto inline_delivery = false;
   auto reason = interruption::none;
   {
      auto guard = watchdog{clock_type::now() - 1ms, {}}.scoped_run([&](interruption cause) {
         ++calls;
         reason = cause;
         inline_delivery = std::this_thread::get_id() == requester;
      });
      BOOST_TEST(!guard.enter());
   }
   BOOST_TEST(calls == 1U);
   BOOST_TEST(!inline_delivery);
   BOOST_CHECK(reason == interruption::deadline);
}

BOOST_AUTO_TEST_CASE(teardown_joins_cancel_external_and_timer_callbacks_before_returning) {
   for (const auto cause : {interruption::canceled, interruption::interrupted, interruption::deadline}) {
      auto cancel = std::stop_source{};
      auto external = std::stop_source{};
      auto mutex = std::mutex{};
      auto condition = std::condition_variable{};
      auto ready = false;
      auto callback_entered = false;
      auto leave_work = false;
      auto closing = false;
      auto release_callback = false;
      auto returned = false;
      auto reason = interruption::none;
      auto owner = std::jthread{[&] {
         const auto deadline = cause == interruption::deadline ? clock_type::now() : clock_type::time_point::max();
         {
            auto guard = watchdog{deadline, {cancel.get_token(), external.get_token()}}.scoped_run(
                [&](interruption value) {
                   auto lock = std::unique_lock{mutex};
                   reason = value;
                   callback_entered = true;
                   condition.notify_all();
                   condition.wait_for(lock, 2s, [&] { return release_callback; });
                });
            auto lock = std::unique_lock{mutex};
            ready = true;
            condition.notify_all();
            condition.wait_for(lock, 2s, [&] { return leave_work; });
            closing = true;
            condition.notify_all();
         }
         auto lock = std::lock_guard{mutex};
         returned = true;
         condition.notify_all();
      }};
      auto requester = std::jthread{[&] {
         {
            auto lock = std::unique_lock{mutex};
            condition.wait_for(lock, 2s, [&] { return ready; });
         }
         if (cause == interruption::canceled)
            cancel.request_stop();
         if (cause == interruption::interrupted)
            external.request_stop();
      }};
      auto entered = false;
      auto began_closing = false;
      auto returned_early = false;
      {
         auto lock = std::unique_lock{mutex};
         entered = condition.wait_for(lock, 1s, [&] { return callback_entered; });
         leave_work = true;
         condition.notify_all();
         began_closing = condition.wait_for(lock, 1s, [&] { return closing; });
         returned_early = condition.wait_for(lock, 20ms, [&] { return returned; });
         release_callback = true;
         condition.notify_all();
      }
      requester.join();
      owner.join();
      BOOST_TEST(entered);
      BOOST_TEST(began_closing);
      BOOST_TEST(!returned_early);
      BOOST_TEST(returned);
      BOOST_CHECK(reason == cause);
   }
}

BOOST_AUTO_TEST_CASE(completed_guards_unregister_both_tokens_and_saturate_long_durations) {
   auto cancel = std::stop_source{};
   auto external = std::stop_source{};
   auto calls = std::atomic_uint{0};
   {
      auto guard = watchdog{clock_type::time_point::max(), {cancel.get_token(), external.get_token()}}.scoped_run(
          [&] { calls.fetch_add(1); });
      BOOST_TEST(guard.enter());
   }
   cancel.request_stop();
   external.request_stop();
   {
      auto guard = watchdog{clock_type::duration::max()}.scoped_run([&] { calls.fetch_add(1); });
   }
   BOOST_TEST(calls.load() == 0U);
}

BOOST_AUTO_TEST_SUITE_END()
