#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;

namespace {

using awaitable_work = forge::asio::task::awaitable;
using forge::asio::task::priority;
using forge::asio::task::task;
using forge::asio::task::context;
using forge::asio::task::handle;
using scheduler_type = forge::asio::task::scheduler;

void wait_task(forge::asio::runtime& runtime, const handle& handle) {
   forge::asio::blocking::run(runtime, handle.wait());
}

bool wait_for_true(const std::atomic_bool& value, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!value.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   return value.load(std::memory_order_acquire);
}

bool wait_for_count(const std::atomic_size_t& value, std::size_t expected,
                    std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (value.load(std::memory_order_acquire) != expected && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   return value.load(std::memory_order_acquire) == expected;
}

void wait_until_true(const std::atomic_bool& value, const char* name = "condition") {
   static_cast<void>(wait_for_true(value));
   BOOST_REQUIRE_MESSAGE(value.load(std::memory_order_acquire), name);
}

#if defined(__APPLE__) || defined(__linux__)
boost::asio::awaitable<std::string> current_thread_name() {
   char name[64] = {};
   if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
      co_return std::string{};
   }
   co_return std::string{name};
}
#endif

} // namespace

BOOST_AUTO_TEST_CASE(task_handle_completion_is_latched_for_late_waiters) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto completed = scheduler.submit(task{.priority = priority{1}, .name = "complete-before-wait", .work = [] {}});

   const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (scheduler.snapshot().completed == 0U && std::chrono::steady_clock::now() < completion_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   BOOST_REQUIRE_EQUAL(scheduler.snapshot().completed, 1U);

   auto waiters = std::vector<std::future<void>>{};
   waiters.reserve(64);
   for (auto index = 0U; index < 64U; ++index) {
      waiters.push_back(boost::asio::co_spawn(runtime.context(), completed.wait(), boost::asio::use_future));
   }
   const auto waiter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   for (auto& waiter : waiters) {
      BOOST_REQUIRE(waiter.wait_until(waiter_deadline) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(waiter.get());
   }
}

BOOST_AUTO_TEST_CASE(task_handle_failure_is_published_before_late_wait_registration) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto failed = scheduler.submit(task{.priority = priority{1},
                                       .name = "failure-before-wait",
                                       .work = [] { throw std::runtime_error{"late task failure"}; }});

   const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (scheduler.snapshot().failed == 0U && std::chrono::steady_clock::now() < failure_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   BOOST_REQUIRE_EQUAL(scheduler.snapshot().failed, 1U);

   auto waiter = boost::asio::co_spawn(runtime.context(), failed.wait(), boost::asio::use_future);
   BOOST_REQUIRE(waiter.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(waiter.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(task_handle_accepted_latches_queue_admission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};

   auto failed = scheduler.submit(task{.priority = priority{1},
                                       .name = "accepted-before-failure",
                                       .work = [] { throw std::runtime_error{"task failure"}; }});
   BOOST_TEST(failed.accepted());
   BOOST_CHECK_THROW(wait_task(runtime, failed), std::runtime_error);
   BOOST_TEST(failed.accepted());

   auto canceled = scheduler.submit_after(task{.priority = priority{1}, .name = "accepted-before-cancel", .work = [] {}},
                                          std::chrono::seconds{1});
   BOOST_TEST(canceled.accepted());
   BOOST_REQUIRE(canceled.cancel());
   BOOST_CHECK_THROW(wait_task(runtime, canceled), forge::asio::exceptions::canceled);
   BOOST_TEST(canceled.accepted());
}

BOOST_AUTO_TEST_CASE(task_handle_accepted_is_false_for_immediate_rejection) {
   auto stopped_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto stopped_scheduler = scheduler_type{stopped_runtime};
   stopped_scheduler.stop();

   auto stopped = stopped_scheduler.submit(task{.priority = priority{1}, .name = "stopped", .work = [] {}});
   BOOST_TEST(!stopped.accepted());
   BOOST_CHECK_THROW(wait_task(stopped_runtime, stopped), forge::asio::exceptions::rejected);

   auto bounded_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto bounded =
       scheduler_type{bounded_runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 1}};
   auto queued = bounded.submit_after(task{.priority = priority{1}, .name = "queued", .work = [] {}},
                                      std::chrono::seconds{1});
   auto full = bounded.submit_after(task{.priority = priority{1}, .name = "full", .work = [] {}},
                                    std::chrono::seconds{1});

   BOOST_TEST(queued.accepted());
   BOOST_TEST(!full.accepted());
   BOOST_CHECK_THROW(wait_task(bounded_runtime, full), forge::asio::exceptions::rejected);
   static_cast<void>(queued.cancel());
   bounded.stop();
}

BOOST_AUTO_TEST_CASE(task_handle_wakes_concurrent_waiters) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto task_started = std::atomic_bool{false};
   auto release_task = std::atomic_bool{false};
   auto waiters_started = std::atomic_size_t{0};
   auto active = scheduler.submit(task{
       .priority = priority{1},
       .name = "concurrent-waiters",
       .work = [&] {
          task_started.store(true, std::memory_order_release);
          while (!release_task.load(std::memory_order_acquire)) {
             std::this_thread::yield();
          }
       },
   });
   wait_until_true(task_started, "concurrent waiter task did not start");

   auto waiters = std::vector<std::future<void>>{};
   waiters.reserve(64);
   for (auto index = 0U; index < 64U; ++index) {
      waiters.push_back(boost::asio::co_spawn(
          runtime.context(),
          [&active, &waiters_started]() -> boost::asio::awaitable<void> {
             waiters_started.fetch_add(1, std::memory_order_release);
             co_await active.wait();
          },
          boost::asio::use_future));
   }
   const auto all_waiters_started = wait_for_count(waiters_started, 64U);
   release_task.store(true, std::memory_order_release);
   BOOST_REQUIRE(all_waiters_started);

   const auto waiter_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   for (auto& waiter : waiters) {
      BOOST_REQUIRE(waiter.wait_until(waiter_deadline) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(waiter.get());
   }
}

BOOST_AUTO_TEST_CASE(runtime_applies_custom_worker_thread_name_when_observable) {
#if defined(__APPLE__) || defined(__linux__)
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1, .thread_name = "forgetest"}};
   const auto name = forge::asio::blocking::run(runtime, current_thread_name());
   BOOST_TEST(name == "forgetest");
#else
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1, .thread_name = "forgetest"}};
   runtime.stop();
#endif
}

BOOST_AUTO_TEST_CASE(task_orders_by_numeric_priority_then_fifo) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 8}};

   auto gate_mutex = std::mutex{};
   auto gate_cv = std::condition_variable{};
   auto release_gate = false;
   auto order_mutex = std::mutex{};
   auto order = std::vector<int>{};
   auto record = [&](int value) {
      return [&, value] {
         const auto lock = std::scoped_lock{order_mutex};
         order.push_back(value);
      };
   };

   auto gate = scheduler.submit(task{
       .priority = priority{100},
       .name = "gate",
       .work =
           [&] {
              auto lock = std::unique_lock{gate_mutex};
              gate_cv.wait(lock, [&] { return release_gate; });
              lock.unlock();
              record(0)();
           },
   });
   auto low = scheduler.submit(task{.priority = priority{10}, .name = "low", .work = record(4)});
   auto high_a = scheduler.submit(awaitable_work{
      .priority = priority{50},
      .name = "high-a",
      .work =
         [&](context&) -> boost::asio::awaitable<void> {
         record(2)();
         co_return;
      },
   });
   auto high_b = scheduler.submit(task{.priority = priority{50}, .name = "high-b", .work = record(3)});

   {
      const auto lock = std::scoped_lock{gate_mutex};
      release_gate = true;
   }
   gate_cv.notify_all();

   wait_task(runtime, gate);
   wait_task(runtime, high_a);
   wait_task(runtime, high_b);
   wait_task(runtime, low);

   const auto expected = std::vector<int>{0, 2, 3, 4};
   BOOST_REQUIRE_EQUAL(order.size(), expected.size());
   BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
   BOOST_CHECK_EQUAL(scheduler.snapshot().completed, 4U);
}

BOOST_AUTO_TEST_CASE(task_handle_wait_owns_state_before_suspension) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime};
   auto ran = std::atomic_bool{false};

   auto submitted = scheduler.submit(task{
       .name = "owned-wait",
       .work = [&] { ran.store(true, std::memory_order_release); },
   });
   auto waiting = submitted.wait();
   submitted = handle{};

   forge::asio::blocking::run(runtime, std::move(waiting));
   BOOST_CHECK(ran.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(task_runs_delayed_tasks_when_due) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto order_mutex = std::mutex{};
   auto order = std::vector<int>{};
   auto record = [&](int value) {
      return [&, value] {
         const auto lock = std::scoped_lock{order_mutex};
         order.push_back(value);
      };
   };

   auto early = scheduler.submit_after(task{.priority = priority{1}, .name = "early", .work = record(1)},
                                       std::chrono::milliseconds{5});
   auto late = scheduler.submit_after(task{.priority = priority{1}, .name = "late", .work = record(2)},
                                      std::chrono::milliseconds{25});

   wait_task(runtime, early);
   wait_task(runtime, late);

   const auto expected = std::vector<int>{1, 2};
   BOOST_REQUIRE_EQUAL(order.size(), expected.size());
   BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(task_cancels_pending_and_rejects_saturated_queue) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 2}};

   auto canceled = scheduler.submit_after(task{.priority = priority{1}, .name = "cancel", .work = [] {}},
                                          std::chrono::seconds{1});
   BOOST_CHECK(canceled.cancel());
   BOOST_CHECK(canceled.cancel_requested());
   BOOST_CHECK_THROW(wait_task(runtime, canceled), forge::asio::exceptions::canceled);

   auto bounded_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto bounded =
       scheduler_type{bounded_runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 1}};
   auto queued = bounded.submit_after(task{.priority = priority{1}, .name = "queued", .work = [] {}},
                                      std::chrono::seconds{1});
   auto rejected = bounded.submit_after(task{.priority = priority{1}, .name = "rejected", .work = [] {}},
                                        std::chrono::seconds{1});

   BOOST_CHECK_THROW(wait_task(bounded_runtime, rejected), forge::asio::exceptions::rejected);
   BOOST_CHECK_EQUAL(bounded.snapshot().rejected, 1U);
   static_cast<void>(queued.cancel());
   bounded.stop();
}

BOOST_AUTO_TEST_CASE(task_shutdown_cancels_pending_work) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto delayed = scheduler.submit_after(task{.priority = priority{1}, .name = "delayed", .work = [] {}},
                                         std::chrono::seconds{10});

   scheduler.stop();

   BOOST_CHECK_THROW(wait_task(runtime, delayed), forge::asio::exceptions::canceled);
   BOOST_CHECK(scheduler.snapshot().stopped);
   BOOST_CHECK_EQUAL(scheduler.snapshot().pending, 0U);
}

BOOST_AUTO_TEST_CASE(cancel_releases_delayed_queue_capacity_before_its_deadline) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_awaitable_tasks = 1, .max_pending_tasks = 1}};
   for (unsigned iteration = 0; iteration < 32; ++iteration) {
      auto delayed = scheduler.submit_after(task{.name = "canceled-deadline", .work = [] {}}, std::chrono::hours{1});
      BOOST_REQUIRE(delayed.accepted());
      BOOST_CHECK_EQUAL(scheduler.pending_count(), 1U);
      BOOST_CHECK(delayed.cancel());
      BOOST_CHECK(!delayed.cancel());
      BOOST_CHECK_EQUAL(scheduler.pending_count(), 0U);
      BOOST_CHECK_EQUAL(scheduler.pending_count(priority{}), 0U);
      BOOST_CHECK_THROW(wait_task(runtime, delayed), forge::asio::exceptions::canceled);
   }
   BOOST_CHECK_EQUAL(scheduler.snapshot().canceled, 32U);
   BOOST_CHECK_EQUAL(scheduler.snapshot().rejected, 0U);
   auto replacement = scheduler.submit(task{.name = "replacement", .work = [] {}});
   BOOST_REQUIRE(replacement.accepted());
   BOOST_CHECK_NO_THROW(wait_task(runtime, replacement));
}

BOOST_AUTO_TEST_CASE(canceled_completion_allows_a_waiter_to_replace_the_queued_task) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_pending_tasks = 1}};
   for (unsigned iteration = 0; iteration < 128; ++iteration) {
      auto delayed = scheduler.submit_after(task{.name = "old-deadline", .work = [] {}}, std::chrono::hours{1});
      BOOST_REQUIRE(delayed.accepted());
      const auto replace_after_completion = [&]() -> boost::asio::awaitable<bool> {
         try {
            co_await delayed.wait();
         } catch (const forge::asio::exceptions::canceled&) {
         }
         auto replacement = scheduler.submit_after(task{.name = "new-deadline", .work = [] {}}, std::chrono::hours{1});
         const auto accepted = replacement.accepted();
         replacement.cancel();
         co_return accepted;
      };
      auto replacement = boost::asio::co_spawn(runtime.context(), replace_after_completion(), boost::asio::use_future);
      delayed.cancel();
      BOOST_REQUIRE(replacement.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
      BOOST_CHECK(replacement.get());
   }
   BOOST_CHECK_EQUAL(scheduler.snapshot().rejected, 0U);
}

BOOST_AUTO_TEST_CASE(task_runs_awaitable_tasks) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto ran = false;

   auto handle = scheduler.submit(awaitable_work{
      .priority = priority{1},
      .name = "awaitable",
      .work =
         [&](context&) -> boost::asio::awaitable<void> {
         ran = true;
         co_return;
      },
   });

   wait_task(runtime, handle);
   BOOST_CHECK(ran);
   BOOST_CHECK_EQUAL(scheduler.snapshot().completed, 1U);
}

BOOST_AUTO_TEST_CASE(task_runs_delayed_awaitable_tasks_when_due) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto order_mutex = std::mutex{};
   auto order = std::vector<int>{};
   auto record = [&](int value) -> boost::asio::awaitable<void> {
      const auto lock = std::scoped_lock{order_mutex};
      order.push_back(value);
      co_return;
   };

   auto early = scheduler.submit_after(
      awaitable_work{
         .priority = priority{1},
         .name = "early",
         .work =
            [&](context&) -> boost::asio::awaitable<void> {
            co_await record(1);
         },
      },
      std::chrono::milliseconds{5});
   auto late = scheduler.submit_after(
      awaitable_work{
         .priority = priority{1},
         .name = "late",
         .work =
            [&](context&) -> boost::asio::awaitable<void> {
            co_await record(2);
         },
      },
      std::chrono::milliseconds{25});

   wait_task(runtime, early);
   wait_task(runtime, late);

   const auto expected = std::vector<int>{1, 2};
   BOOST_REQUIRE_EQUAL(order.size(), expected.size());
   BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(task_cancels_pending_awaitable_tasks) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto ran = false;

   auto handle = scheduler.submit_after(
      awaitable_work{
         .priority = priority{1},
         .name = "pending-cancel",
         .work =
            [&](context&) -> boost::asio::awaitable<void> {
            ran = true;
            co_return;
         },
      },
      std::chrono::seconds{1});

   BOOST_CHECK(handle.cancel());
   BOOST_CHECK_THROW(wait_task(runtime, handle), forge::asio::exceptions::canceled);
   BOOST_CHECK(!ran);
}

BOOST_AUTO_TEST_CASE(task_running_awaitable_tasks_observe_cancel_request) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto started = std::atomic_bool{false};
   auto observed_cancel = false;
   auto stop_possible = false;
   auto token_observed_cancel = false;

   auto handle = scheduler.submit(awaitable_work{
      .priority = priority{1},
      .name = "running-cancel",
      .work =
         [&](context& context) -> boost::asio::awaitable<void> {
         const auto stop_token = context.stop_token();
         stop_possible = stop_token.stop_possible();
         started.store(true, std::memory_order_release);
         while (!context.cancel_requested()) {
            auto executor = co_await boost::asio::this_coro::executor;
            auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{1}};
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
         observed_cancel = true;
         token_observed_cancel = stop_token.stop_requested();
      },
   });

   wait_until_true(started);
   BOOST_CHECK(handle.cancel());
   wait_task(runtime, handle);
   BOOST_CHECK(observed_cancel);
   BOOST_CHECK(stop_possible);
   BOOST_CHECK(token_observed_cancel);
}

BOOST_AUTO_TEST_CASE(task_throw_if_cancel_requested_marks_awaitable_canceled) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto started = std::atomic_bool{false};

   auto handle = scheduler.submit(awaitable_work{
      .priority = priority{1},
      .name = "throw-cancel",
      .work =
         [&](context& context) -> boost::asio::awaitable<void> {
         started.store(true, std::memory_order_release);
         while (!context.cancel_requested()) {
            auto executor = co_await boost::asio::this_coro::executor;
            auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{1}};
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
         context.throw_if_cancel_requested();
      },
   });

   wait_until_true(started);
   BOOST_CHECK(handle.cancel());
   BOOST_CHECK_THROW(wait_task(runtime, handle), forge::asio::exceptions::canceled);
   BOOST_CHECK_EQUAL(scheduler.snapshot().canceled, 1U);
}

BOOST_AUTO_TEST_CASE(task_reports_awaitable_exceptions) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};

   auto handle = scheduler.submit(awaitable_work{
      .priority = priority{1},
      .name = "throw",
      .work =
         [](context&) -> boost::asio::awaitable<void> {
         throw std::runtime_error{"awaitable failed"};
      },
   });

   BOOST_CHECK_THROW(wait_task(runtime, handle), std::runtime_error);
   BOOST_CHECK_EQUAL(scheduler.snapshot().failed, 1U);
}

BOOST_AUTO_TEST_CASE(task_rejects_saturated_awaitable_queue) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 1}};

   auto queued = scheduler.submit_after(
      awaitable_work{
         .priority = priority{1},
         .name = "queued",
         .work = [](context&) -> boost::asio::awaitable<void> {
            co_return;
         },
      },
      std::chrono::seconds{1});
   auto rejected = scheduler.submit_after(
      awaitable_work{
         .priority = priority{1},
         .name = "rejected",
         .work = [](context&) -> boost::asio::awaitable<void> {
            co_return;
         },
      },
      std::chrono::seconds{1});

   BOOST_CHECK_THROW(wait_task(runtime, rejected), forge::asio::exceptions::rejected);
   BOOST_CHECK_EQUAL(scheduler.snapshot().rejected, 1U);
   static_cast<void>(queued.cancel());
   scheduler.stop();
}

BOOST_AUTO_TEST_CASE(task_stop_cancels_pending_and_waits_for_active_awaitable_tasks) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto started = std::atomic_bool{false};
   auto allow_finish = std::atomic_bool{false};

   auto active = scheduler.submit(awaitable_work{
      .priority = priority{1},
      .name = "active",
      .work =
         [&](context&) -> boost::asio::awaitable<void> {
         started.store(true, std::memory_order_release);
         while (!allow_finish.load(std::memory_order_acquire)) {
            auto executor = co_await boost::asio::this_coro::executor;
            auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{1}};
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      },
   });
   auto pending = scheduler.submit_after(
      awaitable_work{
         .priority = priority{1},
         .name = "pending",
         .work = [](context&) -> boost::asio::awaitable<void> {
            co_return;
         },
      },
      std::chrono::seconds{1});

   wait_until_true(started);
   auto stopper = std::thread{[&] { scheduler.stop(); }};
   std::this_thread::sleep_for(std::chrono::milliseconds{10});
   allow_finish.store(true, std::memory_order_release);
   stopper.join();

   wait_task(runtime, active);
   BOOST_CHECK_THROW(wait_task(runtime, pending), forge::asio::exceptions::canceled);
   BOOST_CHECK(scheduler.snapshot().stopped);
}

BOOST_AUTO_TEST_CASE(task_async_shutdown_does_not_block_single_runtime_worker) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{
      .max_blocking_tasks = 1,
      .max_awaitable_tasks = 1,
      .max_pending_tasks = 4,
   }};
   auto started = std::atomic_bool{false};

   auto active = scheduler.submit(awaitable_work{
      .priority = priority{1},
      .name = "active",
      .work = [&](context&) -> boost::asio::awaitable<void> {
         started.store(true, std::memory_order_release);
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         timer.expires_after(std::chrono::milliseconds{10});
         co_await timer.async_wait(boost::asio::use_awaitable);
      },
   });

   wait_until_true(started);
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   wait_task(runtime, active);

   BOOST_CHECK(scheduler.snapshot().stopped);
   BOOST_CHECK_EQUAL(scheduler.snapshot().running_awaitable, 0U);
}

BOOST_AUTO_TEST_CASE(task_request_stop_cancels_pending_work_before_async_drain) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{
      .max_blocking_tasks = 1,
      .max_awaitable_tasks = 1,
      .max_pending_tasks = 4,
   }};
   auto active_started = std::atomic_bool{false};
   auto release_active = std::atomic_bool{false};
   auto pending_ran = std::atomic_bool{false};

   auto active = scheduler.submit(awaitable_work{
      .priority = priority{1},
      .name = "active",
      .work = [&](context&) -> boost::asio::awaitable<void> {
         active_started.store(true, std::memory_order_release);
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         while (!release_active.load(std::memory_order_acquire)) {
            timer.expires_after(std::chrono::milliseconds{1});
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      },
   });
   wait_until_true(active_started);

   auto pending = scheduler.submit_after(
      task{
         .priority = priority{1},
         .name = "pending",
         .work = [&] { pending_ran.store(true, std::memory_order_release); },
      },
      std::chrono::seconds{1});

   scheduler.request_stop();
   BOOST_CHECK(scheduler.snapshot().stopped);
   BOOST_CHECK_EQUAL(scheduler.snapshot().pending, 0U);
   BOOST_CHECK_THROW(wait_task(runtime, pending), forge::asio::exceptions::canceled);
   BOOST_CHECK(!pending_ran.load(std::memory_order_acquire));

   release_active.store(true, std::memory_order_release);
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   wait_task(runtime, active);
}

BOOST_AUTO_TEST_CASE(task_async_shutdown_serializes_drain_waiter_on_multi_worker_runtime) {
   for (auto iteration = 0; iteration < 64; ++iteration) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
      auto scheduler = scheduler_type{runtime, scheduler_type::options{
         .max_blocking_tasks = 1,
         .max_awaitable_tasks = 1,
         .max_pending_tasks = 4,
      }};
      auto started = std::atomic_bool{false};
      auto release = std::atomic_bool{false};

      auto active = scheduler.submit(awaitable_work{
         .priority = priority{1},
         .name = "racing-active",
         .work = [&](context&) -> boost::asio::awaitable<void> {
            started.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
               co_await boost::asio::post(boost::asio::use_awaitable);
            }
         },
      });
      wait_until_true(started);

      auto shutdown = boost::asio::co_spawn(runtime.context(), scheduler.shutdown(), boost::asio::use_future);
      release.store(true, std::memory_order_release);
      shutdown.get();
      wait_task(runtime, active);
      BOOST_CHECK_EQUAL(scheduler.snapshot().running_awaitable, 0U);
   }
}

BOOST_AUTO_TEST_CASE(task_supports_host_owned_awaitable_reschedule_loop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 4}};
   auto passes = 0;

   while (passes < 3) {
      auto handle = scheduler.submit_after(
         awaitable_work{
            .priority = priority{-50},
            .name = "scrub-pass",
            .work =
               [&](context&) -> boost::asio::awaitable<void> {
               ++passes;
               co_return;
            },
         },
         std::chrono::milliseconds{1});
      wait_task(runtime, handle);
   }

   BOOST_CHECK_EQUAL(passes, 3);
   BOOST_CHECK_EQUAL(scheduler.snapshot().completed, 3U);
}

BOOST_AUTO_TEST_CASE(task_awaitable_can_wait_for_nested_blocking_task_with_one_blocking_slot) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = scheduler_type{runtime,
                                   scheduler_type::options{
                                      .max_blocking_tasks = 1,
                                      .max_awaitable_tasks = 1,
                                      .max_pending_tasks = 4,
                                   }};
   auto outer_started = std::atomic_bool{false};
   auto nested_ran = std::atomic_bool{false};

   auto outer = scheduler.submit(awaitable_work{
      .priority = priority{100},
      .name = "outer-awaitable",
      .work =
         [&](context& context) -> boost::asio::awaitable<void> {
         outer_started.store(true, std::memory_order_release);
         static_cast<void>(scheduler.submit(task{
            .priority = priority{100},
            .name = "nested-blocking",
            .work =
               [&] {
                  nested_ran.store(true, std::memory_order_release);
               },
         }));

         while (!nested_ran.load(std::memory_order_acquire) && !context.cancel_requested()) {
            auto executor = co_await boost::asio::this_coro::executor;
            auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{1}};
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      },
   });

   wait_until_true(outer_started);
   if (!wait_for_true(nested_ran, std::chrono::milliseconds{250})) {
      static_cast<void>(outer.cancel());
      wait_task(runtime, outer);
   }
   BOOST_REQUIRE(nested_ran.load(std::memory_order_acquire));
   wait_task(runtime, outer);
   BOOST_CHECK_EQUAL(scheduler.snapshot().completed, 2U);
}

BOOST_AUTO_TEST_CASE(task_skips_saturated_blocking_head_for_runnable_awaitable) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = scheduler_type{runtime,
                                   scheduler_type::options{
                                      .max_blocking_tasks = 1,
                                      .max_awaitable_tasks = 1,
                                      .max_pending_tasks = 8,
                                   }};
   auto gate_mutex = std::mutex{};
   auto gate_cv = std::condition_variable{};
   auto gate_started = std::atomic_bool{false};
   auto release_gate = false;
   auto blocked_head_ran = std::atomic_bool{false};
   auto awaitable_ran = std::atomic_bool{false};

   auto gate = scheduler.submit(task{
      .priority = priority{100},
      .name = "blocking-gate",
      .work =
         [&] {
            gate_started.store(true, std::memory_order_release);
            auto lock = std::unique_lock{gate_mutex};
            gate_cv.wait(lock, [&] { return release_gate; });
         },
   });
   wait_until_true(gate_started);

   auto blocked_head = scheduler.submit(task{
      .priority = priority{90},
      .name = "blocked-head",
      .work =
         [&] {
            blocked_head_ran.store(true, std::memory_order_release);
         },
   });
   auto runnable = scheduler.submit(awaitable_work{
      .priority = priority{80},
      .name = "runnable-awaitable",
      .work =
         [&](context&) -> boost::asio::awaitable<void> {
         awaitable_ran.store(true, std::memory_order_release);
         co_return;
      },
   });

   BOOST_REQUIRE(wait_for_true(awaitable_ran, std::chrono::milliseconds{250}));
   BOOST_CHECK(!blocked_head_ran.load(std::memory_order_acquire));

   {
      const auto lock = std::scoped_lock{gate_mutex};
      release_gate = true;
   }
   gate_cv.notify_all();

   wait_task(runtime, gate);
   wait_task(runtime, runnable);
   wait_task(runtime, blocked_head);
   BOOST_CHECK(blocked_head_ran.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(task_snapshot_reports_separate_blocking_and_awaitable_counts) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = scheduler_type{runtime,
                                   scheduler_type::options{
                                      .max_blocking_tasks = 1,
                                      .max_awaitable_tasks = 1,
                                      .max_pending_tasks = 4,
                                   }};
   auto gate_mutex = std::mutex{};
   auto gate_cv = std::condition_variable{};
   auto release_gate = false;
   auto blocking_started = std::atomic_bool{false};
   auto awaitable_started = std::atomic_bool{false};
   auto release_awaitable = std::atomic_bool{false};

   auto blocking = scheduler.submit(task{
      .priority = priority{100},
      .name = "blocking",
      .work =
         [&] {
            blocking_started.store(true, std::memory_order_release);
            auto lock = std::unique_lock{gate_mutex};
            gate_cv.wait(lock, [&] { return release_gate; });
         },
   });
   wait_until_true(blocking_started, "blocking task started");

   auto awaitable = scheduler.submit(awaitable_work{
      .priority = priority{90},
      .name = "awaitable",
      .work =
         [&](context&) -> boost::asio::awaitable<void> {
         awaitable_started.store(true, std::memory_order_release);
         while (!release_awaitable.load(std::memory_order_acquire)) {
            auto executor = co_await boost::asio::this_coro::executor;
            auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{1}};
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
         co_return;
      },
   });

   if (!wait_for_true(awaitable_started)) {
      release_awaitable.store(true, std::memory_order_release);
      {
         const auto lock = std::scoped_lock{gate_mutex};
         release_gate = true;
      }
      gate_cv.notify_all();
      static_cast<void>(awaitable.cancel());
      wait_task(runtime, blocking);
      BOOST_REQUIRE_MESSAGE(false, "awaitable task started");
   }
   auto snapshot = scheduler.snapshot();
   BOOST_CHECK_EQUAL(snapshot.running_blocking, 1U);
   BOOST_CHECK_EQUAL(snapshot.running_awaitable, 1U);

   release_awaitable.store(true, std::memory_order_release);
   {
      const auto lock = std::scoped_lock{gate_mutex};
      release_gate = true;
   }
   gate_cv.notify_all();

   wait_task(runtime, awaitable);
   wait_task(runtime, blocking);
   snapshot = scheduler.snapshot();
   BOOST_CHECK_EQUAL(snapshot.running_blocking, 0U);
   BOOST_CHECK_EQUAL(snapshot.running_awaitable, 0U);
}

BOOST_AUTO_TEST_CASE(task_delayed_awaitable_runs_while_blocking_budget_is_saturated) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = scheduler_type{runtime,
                                   scheduler_type::options{
                                      .max_blocking_tasks = 1,
                                      .max_awaitable_tasks = 1,
                                      .max_pending_tasks = 4,
                                   }};
   auto gate_mutex = std::mutex{};
   auto gate_cv = std::condition_variable{};
   auto release_gate = false;
   auto blocking_started = std::atomic_bool{false};
   auto awaitable_started = std::atomic_bool{false};

   auto blocking = scheduler.submit_after(
      task{
         .priority = priority{100},
         .name = "delayed-blocking",
         .work =
            [&] {
               blocking_started.store(true, std::memory_order_release);
               auto lock = std::unique_lock{gate_mutex};
               gate_cv.wait(lock, [&] { return release_gate; });
            },
      },
      std::chrono::milliseconds{10});
   auto awaitable = scheduler.submit_after(
      awaitable_work{
         .priority = priority{90},
         .name = "delayed-awaitable",
         .work =
            [&](context&) -> boost::asio::awaitable<void> {
            awaitable_started.store(true, std::memory_order_release);
            co_return;
         },
      },
      std::chrono::milliseconds{30});

   wait_until_true(blocking_started, "blocking task started");
   if (!wait_for_true(awaitable_started, std::chrono::seconds{1})) {
      const auto snapshot = scheduler.snapshot();
      {
         const auto lock = std::scoped_lock{gate_mutex};
         release_gate = true;
      }
      gate_cv.notify_all();
      static_cast<void>(awaitable.cancel());
      wait_task(runtime, blocking);
      BOOST_REQUIRE_MESSAGE(false,
                            "delayed awaitable started while blocking budget is saturated"
                               << " pending=" << snapshot.pending
                               << " running_blocking=" << snapshot.running_blocking
                               << " running_awaitable=" << snapshot.running_awaitable);
   }

   {
      const auto lock = std::scoped_lock{gate_mutex};
      release_gate = true;
   }
   gate_cv.notify_all();

   wait_task(runtime, awaitable);
   wait_task(runtime, blocking);
}

BOOST_AUTO_TEST_CASE(task_awaitable_limit_does_not_consume_blocking_budget) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto scheduler = scheduler_type{runtime,
                                   scheduler_type::options{
                                      .max_blocking_tasks = 1,
                                      .max_awaitable_tasks = 1,
                                      .max_pending_tasks = 8,
                                   }};
   auto first_started = std::atomic_bool{false};
   auto release_first = std::atomic_bool{false};
   auto second_started = std::atomic_bool{false};
   auto blocking_ran = std::atomic_bool{false};

   auto first = scheduler.submit(awaitable_work{
      .priority = priority{100},
      .name = "first-awaitable",
      .work =
         [&](context&) -> boost::asio::awaitable<void> {
         first_started.store(true, std::memory_order_release);
         while (!release_first.load(std::memory_order_acquire)) {
            auto executor = co_await boost::asio::this_coro::executor;
            auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{1}};
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
         co_return;
      },
   });
   wait_until_true(first_started);

   auto second = scheduler.submit(awaitable_work{
      .priority = priority{90},
      .name = "second-awaitable",
      .work =
         [&](context&) -> boost::asio::awaitable<void> {
         second_started.store(true, std::memory_order_release);
         co_return;
      },
   });
   auto blocking = scheduler.submit(task{
      .priority = priority{80},
      .name = "blocking",
      .work =
         [&] {
            blocking_ran.store(true, std::memory_order_release);
         },
   });

   BOOST_REQUIRE(wait_for_true(blocking_ran, std::chrono::milliseconds{250}));
   BOOST_CHECK(!second_started.load(std::memory_order_acquire));

   release_first.store(true, std::memory_order_release);
   wait_task(runtime, first);
   wait_until_true(second_started);
   wait_task(runtime, second);
   wait_task(runtime, blocking);
}
