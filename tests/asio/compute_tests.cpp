#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#include <boost/test/unit_test.hpp>
#include <boost/scope/scope_exit.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.asio.task;

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   return predicate();
}

bool wait_for_true(const std::atomic_bool& value, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
   return wait_until([&] { return value.load(std::memory_order_acquire); }, timeout);
}

std::string current_thread_name() {
#if defined(__APPLE__) || defined(__linux__)
   auto buffer = std::array<char, 64>{};
   if (pthread_getname_np(pthread_self(), buffer.data(), buffer.size()) == 0) {
      return buffer.data();
   }
#endif
   return {};
}

struct awaitable_work {
   boost::asio::awaitable<void> operator()() const {
      co_return;
   }
};

struct cancel_parent_on_copy {
   cancel_parent_on_copy(std::function<void()> on_copy_value, std::atomic_bool& ran_value)
       : on_copy{std::move(on_copy_value)}, ran{&ran_value} {}

   cancel_parent_on_copy(const cancel_parent_on_copy& other) : on_copy{other.on_copy}, ran{other.ran} {
      on_copy();
   }

   cancel_parent_on_copy(cancel_parent_on_copy&&) noexcept = default;

   void operator()() const {
      ran->store(true, std::memory_order_release);
   }

   std::function<void()> on_copy;
   std::atomic_bool* ran = nullptr;
};

struct callable_lifetime {
   std::atomic_size_t live{0};
   std::atomic_bool ran{false};
};

struct move_only_work {
   move_only_work(std::shared_ptr<callable_lifetime> lifetime_value, int result_value)
       : lifetime{std::move(lifetime_value)}, result{std::make_unique<int>(result_value)} {
      lifetime->live.fetch_add(1, std::memory_order_relaxed);
   }

   move_only_work(const move_only_work&) = delete;
   move_only_work& operator=(const move_only_work&) = delete;

   move_only_work(move_only_work&& other) noexcept
       : lifetime{std::move(other.lifetime)}, result{std::move(other.result)} {}
   move_only_work& operator=(move_only_work&&) = delete;

   ~move_only_work() {
      if (lifetime != nullptr) {
         lifetime->live.fetch_sub(1, std::memory_order_relaxed);
      }
   }

   std::unique_ptr<int> operator()() {
      lifetime->ran.store(true, std::memory_order_release);
      return std::move(result);
   }

   std::shared_ptr<callable_lifetime> lifetime;
   std::unique_ptr<int> result;
};

template <typename Work>
concept compute_work =
    requires(forge::asio::compute::executor executor, Work work) { executor.try_submit({}, std::move(work)); };

static_assert(!compute_work<awaitable_work>);

} // namespace

BOOST_AUTO_TEST_CASE(compute_execute_returns_values_and_propagates_exceptions) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 2}};
   auto executor = pool.get_executor();

   const auto value = forge::asio::blocking::run(
       runtime, executor.execute({.name = "value"}, [] { return std::make_unique<int>(42); }));
   BOOST_REQUIRE(value != nullptr);
   BOOST_CHECK_EQUAL(*value, 42);

   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, executor.execute({.name = "failure"}, []() -> int { throw std::runtime_error{"compute failed"}; })),
       std::runtime_error);
   BOOST_CHECK_EQUAL(pool.snapshot().completed, 1U);
   BOOST_CHECK_EQUAL(pool.snapshot().failed, 1U);

   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_awaitables_own_temporary_executor_and_move_only_work) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};

   auto submit_lifetime = std::make_shared<callable_lifetime>();
   auto submission = pool.get_executor().submit({.name = "owned-submit"}, move_only_work{submit_lifetime, 41});
   BOOST_REQUIRE_EQUAL(submit_lifetime->live.load(std::memory_order_relaxed), 1U);

   auto operation = forge::asio::blocking::run(runtime, std::move(submission));
   auto submitted_result = forge::asio::blocking::run(runtime, std::move(operation).wait());
   BOOST_REQUIRE(submitted_result != nullptr);
   BOOST_CHECK_EQUAL(*submitted_result, 41);
   BOOST_CHECK(submit_lifetime->ran.load(std::memory_order_acquire));
   BOOST_CHECK_EQUAL(submit_lifetime->live.load(std::memory_order_relaxed), 0U);

   auto execute_lifetime = std::make_shared<callable_lifetime>();
   auto execution = pool.get_executor().execute({.name = "owned-execute"}, move_only_work{execute_lifetime, 42});
   BOOST_REQUIRE_EQUAL(execute_lifetime->live.load(std::memory_order_relaxed), 1U);

   auto executed_result = forge::asio::blocking::run(runtime, std::move(execution));
   BOOST_REQUIRE(executed_result != nullptr);
   BOOST_CHECK_EQUAL(*executed_result, 42);
   BOOST_CHECK(execute_lifetime->ran.load(std::memory_order_acquire));
   BOOST_CHECK_EQUAL(execute_lifetime->live.load(std::memory_order_relaxed), 0U);

   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_operation_wait_owns_state_before_suspension) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};

   auto operation = forge::asio::blocking::run(
       runtime, pool.get_executor().submit({.name = "owned-wait"}, [] { return 42; }));
   auto waiting = std::move(operation).wait();
   operation = {};

   BOOST_CHECK_EQUAL(forge::asio::blocking::run(runtime, std::move(waiting)), 42);
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_releases_single_runtime_worker_while_cpu_work_runs) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
   auto timer_fired = std::atomic_bool{false};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto runtime_thread = std::this_thread::get_id();
      auto asio_executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          asio_executor,
          [&]() -> boost::asio::awaitable<void> {
             auto timer =
                 boost::asio::steady_timer{co_await boost::asio::this_coro::executor, std::chrono::milliseconds{10}};
             co_await timer.async_wait(boost::asio::use_awaitable);
             timer_fired.store(true, std::memory_order_release);
          },
          boost::asio::detached);

      const auto compute_thread = co_await pool.get_executor().execute({.name = "slow-cpu"}, [] {
         std::this_thread::sleep_for(std::chrono::milliseconds{75});
         return std::this_thread::get_id();
      });

      BOOST_CHECK(compute_thread != runtime_thread);
      BOOST_CHECK(std::this_thread::get_id() == runtime_thread);
      BOOST_CHECK(timer_fired.load(std::memory_order_acquire));
   }());

   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_runs_work_in_parallel_up_to_worker_count) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 2,
       .max_pending_tasks = 0,
   }};
   auto active = std::atomic_size_t{0};
   auto maximum = std::atomic_size_t{0};
   auto release = std::atomic_bool{false};

   const auto work = [&] {
      const auto current = active.fetch_add(1, std::memory_order_acq_rel) + 1;
      auto observed = maximum.load(std::memory_order_relaxed);
      while (observed < current && !maximum.compare_exchange_weak(observed, current, std::memory_order_relaxed)) {
      }
      while (!release.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      active.fetch_sub(1, std::memory_order_acq_rel);
   };

   auto first = forge::asio::blocking::run(runtime, pool.get_executor().submit({.name = "parallel-1"}, work));
   auto second = forge::asio::blocking::run(runtime, pool.get_executor().submit({.name = "parallel-2"}, work));
   BOOST_REQUIRE(wait_until([&] { return maximum.load(std::memory_order_acquire) == 2; }));
   release.store(true, std::memory_order_release);

   forge::asio::blocking::run(runtime, std::move(first).wait());
   forge::asio::blocking::run(runtime, std::move(second).wait());
   BOOST_CHECK_EQUAL(maximum.load(std::memory_order_relaxed), 2U);
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_try_submit_obeys_bounded_capacity) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .max_pending_tasks = 0,
       .max_waiting_submissions = 1,
   }};
   auto executor = pool.get_executor();
   auto gate_mutex = std::mutex{};
   auto gate_cv = std::condition_variable{};
   auto started = std::atomic_bool{false};
   auto release = false;

   auto active = forge::asio::blocking::run(runtime, executor.submit({.name = "active"}, [&] {
      started.store(true, std::memory_order_release);
      auto lock = std::unique_lock{gate_mutex};
      gate_cv.wait(lock, [&] { return release; });
   }));
   BOOST_REQUIRE(wait_for_true(started));

   auto rejected = executor.try_submit({.name = "rejected"}, [] {});
   BOOST_CHECK(!rejected.has_value());
   BOOST_CHECK_EQUAL(pool.snapshot().rejected, 1U);

   {
      const auto lock = std::scoped_lock{gate_mutex};
      release = true;
   }
   gate_cv.notify_all();
   forge::asio::blocking::run(runtime, std::move(active).wait());
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_submit_waits_for_capacity_without_blocking_runtime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .max_pending_tasks = 0,
       .max_waiting_submissions = 1,
   }};
   auto executor = pool.get_executor();
   auto first_started = std::atomic_bool{false};
   auto release_first = std::atomic_bool{false};
   auto timer_fired = std::atomic_bool{false};

   auto first = forge::asio::blocking::run(runtime, executor.submit({.name = "first"}, [&] {
      first_started.store(true, std::memory_order_release);
      while (!release_first.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
   }));
   BOOST_REQUIRE(wait_for_true(first_started));

   auto releaser = std::thread{[&] {
      std::this_thread::sleep_for(std::chrono::milliseconds{30});
      release_first.store(true, std::memory_order_release);
   }};
   const auto second = forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<int> {
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor, std::chrono::milliseconds{10}};
      boost::asio::co_spawn(
          co_await boost::asio::this_coro::executor,
          [&]() -> boost::asio::awaitable<void> {
             co_await timer.async_wait(boost::asio::use_awaitable);
             timer_fired.store(true, std::memory_order_release);
          },
          boost::asio::detached);
      co_return co_await executor.execute({.name = "second"}, [] { return 7; });
   }());
   releaser.join();

   BOOST_CHECK_EQUAL(second, 7);
   BOOST_CHECK(timer_fired.load(std::memory_order_acquire));
   forge::asio::blocking::run(runtime, std::move(first).wait());
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_admits_waiting_submissions_in_fifo_order_and_bounds_the_wait_queue) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .max_pending_tasks = 0,
       .max_waiting_submissions = 2,
   }};
   auto executor = pool.get_executor();
   auto first_started = std::atomic_bool{false};
   auto release_first = std::atomic_bool{false};
   auto order_mutex = std::mutex{};
   auto order = std::vector<int>{};

   auto first = forge::asio::blocking::run(runtime, executor.submit({.name = "active"}, [&] {
      first_started.store(true, std::memory_order_release);
      while (!release_first.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
   }));
   BOOST_REQUIRE(wait_for_true(first_started));

   auto second = boost::asio::co_spawn(runtime.context(),
                                       executor.execute({.name = "waiting-2"},
                                                        [&] {
                                                           const auto lock = std::scoped_lock{order_mutex};
                                                           order.push_back(2);
                                                        }),
                                       boost::asio::use_future);
   BOOST_REQUIRE(wait_until([&] { return pool.snapshot().waiting == 1; }));
   auto third = boost::asio::co_spawn(runtime.context(),
                                      executor.execute({.name = "waiting-3"},
                                                       [&] {
                                                          const auto lock = std::scoped_lock{order_mutex};
                                                          order.push_back(3);
                                                       }),
                                      boost::asio::use_future);
   BOOST_REQUIRE(wait_until([&] { return pool.snapshot().waiting == 2; }));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, executor.submit({.name = "overflow"}, [] {})),
                     forge::asio::exceptions::rejected);
   release_first.store(true, std::memory_order_release);
   second.get();
   third.get();
   forge::asio::blocking::run(runtime, std::move(first).wait());

   const auto expected = std::vector<int>{2, 3};
   BOOST_TEST(order == expected, boost::test_tools::per_element());
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_hands_off_a_canceled_granted_reservation_to_the_next_waiter) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .max_pending_tasks = 0,
       .max_waiting_submissions = 2,
   }};
   auto executor = pool.get_executor();
   auto active_started = std::atomic_bool{false};
   auto release_active = std::atomic_bool{false};
   auto runtime_blocked = std::atomic_bool{false};
   auto release_runtime = std::atomic_bool{false};
   auto canceled_ran = std::atomic_bool{false};
   auto successor_ran = std::atomic_bool{false};

   auto active = forge::asio::blocking::run(runtime, executor.submit({.name = "active"}, [&] {
      active_started.store(true, std::memory_order_release);
      while (!release_active.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
   }));
   BOOST_REQUIRE(wait_for_true(active_started));

   auto canceled_source = std::stop_source{};
   auto canceled = boost::asio::co_spawn(
       runtime.context(),
       executor.execute({.name = "canceled", .parent_stop_token = canceled_source.get_token()},
                        [&] { canceled_ran.store(true, std::memory_order_release); }),
       boost::asio::use_future);
   BOOST_REQUIRE(wait_until([&] { return pool.snapshot().waiting == 1; }));

   auto successor = boost::asio::co_spawn(
       runtime.context(),
       executor.execute({.name = "successor"}, [&] { successor_ran.store(true, std::memory_order_release); }),
       boost::asio::use_future);
   BOOST_REQUIRE(wait_until([&] { return pool.snapshot().waiting == 2; }));

   boost::asio::post(runtime.context(), [&] {
      runtime_blocked.store(true, std::memory_order_release);
      while (!release_runtime.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
   });
   BOOST_REQUIRE(wait_for_true(runtime_blocked));

   release_active.store(true, std::memory_order_release);
   BOOST_REQUIRE(wait_until([&] {
      const auto current = pool.snapshot();
      return current.running == 0 && current.waiting == 1;
   }));
   BOOST_REQUIRE(canceled_source.request_stop());

   const auto handed_off = wait_until([&] { return pool.snapshot().waiting == 0; }, std::chrono::milliseconds{250});
   release_runtime.store(true, std::memory_order_release);
   BOOST_CHECK(handed_off);
   if (!handed_off) {
      pool.request_stop();
   }

   BOOST_CHECK_THROW(static_cast<void>(canceled.get()), forge::asio::exceptions::canceled);
   BOOST_CHECK(!canceled_ran.load(std::memory_order_acquire));
   if (handed_off) {
      BOOST_REQUIRE(successor.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
      successor.get();
      BOOST_CHECK(successor_ran.load(std::memory_order_acquire));
   } else {
      BOOST_CHECK_THROW(static_cast<void>(successor.get()), forge::asio::exceptions::rejected);
   }

   forge::asio::blocking::run(runtime, std::move(active).wait());
   const auto final = pool.snapshot();
   BOOST_CHECK_EQUAL(final.waiting, 0U);
   BOOST_CHECK_EQUAL(final.running, 0U);
   BOOST_CHECK_GE(final.canceled, 1U);
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_hands_off_capacity_when_parent_cancels_after_reserve) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .max_pending_tasks = 0,
       .max_waiting_submissions = 2,
   }};
   auto executor = pool.get_executor();
   auto canceled_ran = std::atomic_bool{false};
   auto successor_ran = std::atomic_bool{false};
   auto parent = std::stop_source{};
   auto successor = std::optional<std::future<void>>{};
   auto successor_waiting = std::atomic_bool{false};
   auto canceling_work = cancel_parent_on_copy{
       [&] {
          successor.emplace(boost::asio::co_spawn(
              runtime.context(),
              executor.execute({.name = "successor"}, [&] {
                 successor_ran.store(true, std::memory_order_release);
              }),
              boost::asio::use_future));
          successor_waiting.store(wait_until([&] { return pool.snapshot().waiting == 1; }),
                                  std::memory_order_release);
          static_cast<void>(parent.request_stop());
       },
       canceled_ran};

   auto canceled = executor.try_submit(
       {.name = "cancel-after-reserve", .parent_stop_token = parent.get_token()}, canceling_work);
   BOOST_REQUIRE(canceled.has_value());
   BOOST_REQUIRE(successor.has_value());
   BOOST_REQUIRE(successor_waiting.load(std::memory_order_acquire));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(*canceled).wait()),
                     forge::asio::exceptions::canceled);
   BOOST_CHECK(parent.stop_requested());
   BOOST_CHECK(!canceled_ran.load(std::memory_order_acquire));

   const auto successor_ready = successor->wait_for(std::chrono::milliseconds{250}) == std::future_status::ready;
   BOOST_CHECK(successor_ready);
   if (successor_ready) {
      successor->get();
      BOOST_CHECK(successor_ran.load(std::memory_order_acquire));
   } else {
      pool.request_stop();
      BOOST_CHECK_THROW(static_cast<void>(successor->get()), forge::asio::exceptions::rejected);
   }

   const auto final = pool.snapshot();
   BOOST_CHECK_EQUAL(final.waiting, 0U);
   BOOST_CHECK_EQUAL(final.running, 0U);
   if (successor_ready) {
      BOOST_CHECK_EQUAL(final.submitted, 2U);
      BOOST_CHECK_EQUAL(final.completed, 1U);
      BOOST_CHECK_EQUAL(final.canceled, 1U);
   }
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_parent_token_cancels_a_waiting_submission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .max_pending_tasks = 0,
       .max_waiting_submissions = 1,
   }};
   auto release = std::atomic_bool{false};
   auto active = forge::asio::blocking::run(runtime, pool.get_executor().submit({.name = "active"}, [&] {
      while (!release.load(std::memory_order_acquire)) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
   }));
   auto parent = std::stop_source{};
   auto waiting = boost::asio::co_spawn(
       runtime.context(),
       pool.get_executor().submit({.name = "waiting", .parent_stop_token = parent.get_token()}, [] {}),
       boost::asio::use_future);
   BOOST_REQUIRE(wait_until([&] { return pool.snapshot().waiting == 1; }));

   BOOST_REQUIRE(parent.request_stop());
   BOOST_CHECK_THROW(static_cast<void>(waiting.get()), forge::asio::exceptions::canceled);
   BOOST_CHECK_EQUAL(pool.snapshot().waiting, 0U);

   release.store(true, std::memory_order_release);
   forge::asio::blocking::run(runtime, std::move(active).wait());
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_supports_pre_pending_and_running_cancellation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .max_pending_tasks = 1,
       .max_waiting_submissions = 1,
   }};
   auto executor = pool.get_executor();

   auto pre_stop = std::stop_source{};
   static_cast<void>(pre_stop.request_stop());
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         runtime, executor.submit({.name = "pre", .parent_stop_token = pre_stop.get_token()}, [] {})),
                     forge::asio::exceptions::canceled);

   auto running_started = std::atomic_bool{false};
   auto running = forge::asio::blocking::run(
       runtime, executor.submit({.name = "running"}, [&](forge::asio::compute::context& context) {
          running_started.store(true, std::memory_order_release);
          while (!context.stop_requested()) {
             std::this_thread::sleep_for(std::chrono::milliseconds{1});
          }
       }));
   BOOST_REQUIRE(wait_for_true(running_started));

   auto pending_ran = std::atomic_bool{false};
   auto pending = forge::asio::blocking::run(
       runtime, executor.submit({.name = "pending"}, [&] { pending_ran.store(true, std::memory_order_release); }));
   BOOST_CHECK(pending.cancel());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(pending).wait()), forge::asio::exceptions::canceled);
   BOOST_CHECK(!pending_ran.load(std::memory_order_acquire));

   BOOST_CHECK(running.cancel());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(running).wait()), forge::asio::exceptions::canceled);
   BOOST_CHECK_GE(pool.snapshot().canceled, 2U);
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_shutdown_waits_for_running_work_and_runs_worker_hooks_once,
                     * boost::unit_test::timeout(20)) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto starts = std::atomic_size_t{0};
   auto stops = std::atomic_size_t{0};
   auto hook_mutex = std::mutex{};
   auto worker_indexes = std::map<std::thread::id, std::size_t>{};
   auto worker_names = std::map<std::thread::id, std::string>{};
   auto mismatched_worker = std::atomic_bool{false};
   auto started = std::make_shared<std::promise<void>>();
   auto started_result = started->get_future();
   auto release = std::make_shared<std::promise<void>>();
   auto released = release->get_future().share();
   auto finished = std::make_shared<std::atomic_bool>(false);
   auto callback_timed_out = std::make_shared<std::atomic_bool>(false);
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 2,
       .thread_name = "forge-cpu-test",
       .on_worker_start =
           [&](std::size_t index) {
              const auto lock = std::scoped_lock{hook_mutex};
              worker_indexes.emplace(std::this_thread::get_id(), index);
              worker_names.emplace(std::this_thread::get_id(), current_thread_name());
              starts.fetch_add(1, std::memory_order_relaxed);
           },
       .on_worker_stop =
           [&](std::size_t index) {
              const auto lock = std::scoped_lock{hook_mutex};
              const auto found = worker_indexes.find(std::this_thread::get_id());
              if (found == worker_indexes.end() || found->second != index) {
                 mismatched_worker.store(true, std::memory_order_relaxed);
              }
              stops.fetch_add(1, std::memory_order_relaxed);
           },
   }};
   // Declared after the pool so every failure releases its callback before join.
   auto unblock = boost::scope::scope_exit{[release] { release->set_value(); }};
   BOOST_CHECK_EQUAL(starts.load(std::memory_order_relaxed), 2U);

   auto submitted = boost::asio::co_spawn(
       runtime.context(),
       pool.get_executor().submit({.name = "ignore-stop"}, [started, released, finished, callback_timed_out] {
          started->set_value();
          if (released.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
             callback_timed_out->store(true, std::memory_order_release);
          } else {
             released.get();
          }
          finished->store(true, std::memory_order_release);
       }),
       boost::asio::use_future);
   BOOST_REQUIRE(submitted.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   auto operation = submitted.get();
   BOOST_REQUIRE(started_result.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   started_result.get();
   // Submission is not proof of execution. Stop only after the callback entered,
   // and before release, so the running operation's cancellation is deterministic.
   pool.request_stop();
   BOOST_CHECK(operation.stop_requested());
   auto shutdown = boost::asio::co_spawn(runtime.context(), pool.shutdown(), boost::asio::use_future);
   BOOST_CHECK(shutdown.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   BOOST_CHECK(!finished->load(std::memory_order_acquire));
   BOOST_CHECK_EQUAL(stops.load(std::memory_order_relaxed), 0U);
   BOOST_CHECK_EQUAL(pool.snapshot().running, 1U);
   release->set_value();
   unblock.set_active(false);
   BOOST_REQUIRE(shutdown.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   shutdown.get();

   BOOST_CHECK(finished->load(std::memory_order_acquire));
   BOOST_CHECK(!callback_timed_out->load(std::memory_order_acquire));
   BOOST_CHECK_EQUAL(stops.load(std::memory_order_relaxed), 2U);
   BOOST_CHECK(!mismatched_worker.load(std::memory_order_relaxed));
   for (const auto& [thread, name] : worker_names) {
      static_cast<void>(thread);
      BOOST_CHECK_EQUAL(name, "forge-cpu-test");
   }
   auto completed = boost::asio::co_spawn(runtime.context(), std::move(operation).wait(), boost::asio::use_future);
   BOOST_REQUIRE(completed.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(completed.get(), forge::asio::exceptions::canceled);
   BOOST_CHECK_EQUAL(pool.snapshot().canceled, 1U);
   BOOST_CHECK_EQUAL(pool.snapshot().failed, 0U);
   BOOST_CHECK_EQUAL(pool.snapshot().running, 0U);
}

BOOST_AUTO_TEST_CASE(compute_shutdown_is_concurrent_and_idempotent, * boost::unit_test::timeout(20)) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto starts = std::atomic_size_t{0};
   auto stops = std::atomic_size_t{0};
   auto started = std::make_shared<std::promise<void>>();
   auto started_result = started->get_future();
   auto release = std::make_shared<std::promise<void>>();
   auto released = release->get_future().share();
   auto finished = std::make_shared<std::atomic_bool>(false);
   auto callback_timed_out = std::make_shared<std::atomic_bool>(false);
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .on_worker_start = [&](std::size_t) { starts.fetch_add(1, std::memory_order_relaxed); },
       .on_worker_stop = [&](std::size_t) { stops.fetch_add(1, std::memory_order_relaxed); },
   }};
   auto unblock = boost::scope::scope_exit{[release] { release->set_value(); }};
   auto submitted = boost::asio::co_spawn(
       runtime.context(),
       pool.get_executor().submit({.name = "shutdown-race"}, [started, released, finished, callback_timed_out] {
          started->set_value();
          if (released.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
             callback_timed_out->store(true, std::memory_order_release);
          } else {
             released.get();
          }
          finished->store(true, std::memory_order_release);
       }),
       boost::asio::use_future);
   BOOST_REQUIRE(submitted.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   auto operation = submitted.get();
   BOOST_REQUIRE(started_result.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   started_result.get();
   pool.request_stop();
   BOOST_CHECK(operation.stop_requested());

   auto first = boost::asio::co_spawn(runtime.context(), pool.shutdown(), boost::asio::use_future);
   auto second = boost::asio::co_spawn(runtime.context(), pool.shutdown(), boost::asio::use_future);
   BOOST_CHECK(first.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   BOOST_CHECK(second.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   BOOST_CHECK(!finished->load(std::memory_order_acquire));
   BOOST_CHECK_EQUAL(stops.load(std::memory_order_relaxed), 0U);
   BOOST_CHECK_EQUAL(pool.snapshot().running, 1U);
   release->set_value();
   unblock.set_active(false);
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   first.get();
   BOOST_REQUIRE(second.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   second.get();

   BOOST_CHECK(finished->load(std::memory_order_acquire));
   BOOST_CHECK(!callback_timed_out->load(std::memory_order_acquire));
   BOOST_CHECK_EQUAL(starts.load(std::memory_order_relaxed), 1U);
   BOOST_CHECK_EQUAL(stops.load(std::memory_order_relaxed), 1U);
   auto completed = boost::asio::co_spawn(runtime.context(), std::move(operation).wait(), boost::asio::use_future);
   BOOST_REQUIRE(completed.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(completed.get(), forge::asio::exceptions::canceled);
   auto repeated = boost::asio::co_spawn(runtime.context(), pool.shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(repeated.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   repeated.get();
   BOOST_CHECK_EQUAL(stops.load(std::memory_order_relaxed), 1U);
   BOOST_CHECK_EQUAL(pool.snapshot().canceled, 1U);
   BOOST_CHECK_EQUAL(pool.snapshot().failed, 0U);
   BOOST_CHECK_EQUAL(pool.snapshot().running, 0U);
}

BOOST_AUTO_TEST_CASE(compute_pool_destructor_waits_for_owned_work) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto started = std::atomic_bool{false};
   auto finished = std::atomic_bool{false};
   auto operation = forge::asio::compute::operation<void>{};
   {
      auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
      operation = forge::asio::blocking::run(runtime, pool.get_executor().submit({.name = "destructor"}, [&] {
         started.store(true, std::memory_order_release);
         std::this_thread::sleep_for(std::chrono::milliseconds{30});
         finished.store(true, std::memory_order_release);
      }));
      BOOST_REQUIRE(wait_for_true(started));
   }

   BOOST_CHECK(finished.load(std::memory_order_acquire));
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(operation).wait()),
                     forge::asio::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(compute_inherits_task_context_cancellation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto compute_started = std::atomic_bool{false};

   auto scheduled = scheduler.submit(forge::asio::task::awaitable{
       .priority = forge::asio::task::priority{100},
       .name = "task-to-compute",
       .work = [&](forge::asio::task::context& task_context) -> boost::asio::awaitable<void> {
          co_await pool.get_executor().execute({.name = "cooperative", .parent_stop_token = task_context.stop_token()},
                                               [&](forge::asio::compute::context& compute_context) {
                                                  compute_started.store(true, std::memory_order_release);
                                                  while (!compute_context.stop_requested()) {
                                                     std::this_thread::sleep_for(std::chrono::milliseconds{1});
                                                  }
                                               });
       },
   });

   BOOST_REQUIRE(wait_for_true(compute_started));
   BOOST_CHECK(scheduled.cancel());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, scheduled.wait()), forge::asio::exceptions::canceled);
   BOOST_CHECK_EQUAL(scheduler.snapshot().canceled, 1U);

   scheduler.stop();
   forge::asio::blocking::run(runtime, pool.shutdown());
}

BOOST_AUTO_TEST_CASE(compute_rejects_worker_start_hook_failure) {
   BOOST_CHECK_THROW(forge::asio::compute::pool(forge::asio::compute::pool::options{
                         .worker_threads = 2,
                         .on_worker_start =
                             [](std::size_t index) {
                                if (index == 1) {
                                   throw std::runtime_error{"worker initialization failed"};
                                }
                             },
                     }),
                     forge::asio::exceptions::internal);
}

BOOST_AUTO_TEST_CASE(compute_reports_worker_stop_hook_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 1,
       .on_worker_stop = [](std::size_t) { throw std::runtime_error{"worker cleanup failed"}; },
   }};

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, pool.shutdown()), forge::asio::exceptions::internal);
}

BOOST_AUTO_TEST_CASE(compute_completes_every_accepted_operation_before_shutdown) {
   constexpr auto operation_count = std::size_t{64};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{
       .worker_threads = 4,
       .max_pending_tasks = operation_count,
   }};
   auto completed = std::atomic_size_t{0};
   auto operations = std::vector<forge::asio::compute::operation<void>>{};
   operations.reserve(operation_count);

   for (auto index = std::size_t{0}; index < operation_count; ++index) {
      operations.push_back(forge::asio::blocking::run(runtime, pool.get_executor().submit({.name = "stress"}, [&] {
         completed.fetch_add(1, std::memory_order_relaxed);
      })));
   }
   for (auto& operation : operations) {
      forge::asio::blocking::run(runtime, std::move(operation).wait());
   }

   const auto metrics = pool.snapshot();
   BOOST_CHECK_EQUAL(completed.load(std::memory_order_relaxed), operation_count);
   BOOST_CHECK_EQUAL(metrics.submitted, operation_count);
   BOOST_CHECK_EQUAL(metrics.completed, operation_count);
   BOOST_CHECK_EQUAL(metrics.failed, 0U);
   BOOST_CHECK_EQUAL(metrics.canceled, 0U);

   forge::asio::blocking::run(runtime, pool.shutdown());
}
