#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.affine;
import forge.asio.blocking;
import forge.asio.runtime;

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   return predicate();
}

struct blocking_operation {
   std::mutex mutex;
   std::condition_variable condition;
   bool started = false;
   bool released = false;

   void run() {
      auto lock = std::unique_lock{mutex};
      started = true;
      condition.notify_all();
      condition.wait(lock, [&] { return released; });
   }

   void wait_started() {
      auto lock = std::unique_lock{mutex};
      condition.wait(lock, [&] { return started; });
   }

   void release() {
      const auto lock = std::scoped_lock{mutex};
      released = true;
      condition.notify_all();
   }
};

struct awaitable_work {
   boost::asio::awaitable<void> operator()() const {
      co_return;
   }
};

template <typename Work>
concept affine_work = requires(forge::asio::affine::executor executor, Work work) {
   executor.execute({}, std::move(work));
};

static_assert(!affine_work<awaitable_work>);

boost::asio::awaitable<std::size_t> run_fast_affine_operations(forge::asio::affine::executor executor,
                                                               std::size_t count) {
   auto completed = std::size_t{};
   for (auto index = std::size_t{}; index < count; ++index) {
      completed += co_await executor.execute({.name = "fast-completion"}, [] { return std::size_t{1}; });
   }
   co_return completed;
}

boost::asio::awaitable<void>
expect_affine_canceled(forge::asio::affine::executor executor, std::atomic_bool& observed) {
   try {
      co_await executor.execute({.name = "pending"}, [] {});
   } catch (const forge::asio::exceptions::canceled&) {
      observed.store(true, std::memory_order_release);
   }
}

boost::asio::awaitable<void>
expect_affine_rejected(forge::asio::affine::executor executor, std::atomic_bool& observed) {
   try {
      co_await executor.execute({.name = "waiting"}, [] {});
   } catch (const forge::asio::exceptions::rejected&) {
      observed.store(true, std::memory_order_release);
   }
}

boost::asio::awaitable<bool> execute_after_precancel(forge::asio::affine::executor executor,
                                                     std::atomic_bool& ready,
                                                     std::atomic_bool& invoked) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   const auto current = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{current, boost::asio::steady_timer::time_point::max()};
   ready.store(true, std::memory_order_release);
   auto error = boost::system::error_code{};
   co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   if (error != boost::asio::error::operation_aborted) {
      throw std::runtime_error{"test pre-cancel wait was not canceled"};
   }

   try {
      co_await executor.execute({.name = "pre-canceled"}, [&] { invoked.store(true, std::memory_order_release); });
   } catch (const forge::asio::exceptions::canceled&) {
      co_return true;
   }
   co_return false;
}

} // namespace

BOOST_AUTO_TEST_CASE(asio_affine_runs_every_operation_on_one_owned_thread) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lane = forge::asio::affine::lane{};
   auto executor = lane.get_executor();
   auto ids = std::vector<std::thread::id>{};

   for (auto index = 0; index < 8; ++index) {
      ids.push_back(forge::asio::blocking::run(
         runtime, executor.execute({.name = "identity"}, [] { return std::this_thread::get_id(); })));
   }

   BOOST_REQUIRE(!ids.empty());
   for (const auto id : ids) {
      BOOST_CHECK(id == ids.front());
   }
   BOOST_CHECK(ids.front() != std::this_thread::get_id());
   BOOST_CHECK_EQUAL(lane.snapshot().completed, ids.size());
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(asio_affine_fast_completion_returns_to_multiple_runtime_workers) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lane = forge::asio::affine::lane{};
   constexpr auto count = std::size_t{10000};
   auto result = boost::asio::co_spawn(runtime.context(), run_fast_affine_operations(lane.get_executor(), count),
                                       boost::asio::use_future);
   BOOST_REQUIRE(result.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
   BOOST_CHECK_EQUAL(result.get(), count);
   BOOST_CHECK_EQUAL(lane.snapshot().completed, count);
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(asio_affine_preserves_fifo_across_pending_and_waiting_queues) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lane = forge::asio::affine::lane{{.max_pending_operations = 1, .max_waiting_submissions = 2}};
   auto executor = lane.get_executor();
   auto blocker = std::make_shared<blocking_operation>();
   auto order = std::vector<int>{};
   auto mutex = std::mutex{};

   auto active = boost::asio::co_spawn(
      runtime.context(), executor.execute({.name = "active"}, [blocker] { blocker->run(); }), boost::asio::use_future);
   blocker->wait_started();

   auto enqueue = [&](int value) {
      return boost::asio::co_spawn(
         runtime.context(),
         executor.execute({.name = "queued"}, [&, value] {
            const auto lock = std::scoped_lock{mutex};
            order.push_back(value);
         }),
         boost::asio::use_future);
   };
   auto first = enqueue(1);
   BOOST_REQUIRE(wait_until([&] { return lane.snapshot().pending == 1; }));
   auto second = enqueue(2);
   BOOST_REQUIRE(wait_until([&] { return lane.snapshot().waiting == 1; }));
   auto third = enqueue(3);
   BOOST_REQUIRE(wait_until([&] {
      const auto value = lane.snapshot();
      return value.pending == 1 && value.waiting == 2;
   }));

   blocker->release();
   active.get();
   first.get();
   second.get();
   third.get();

   const auto expected = std::vector<int>{1, 2, 3};
   BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(asio_affine_rejects_when_bounded_admission_is_full) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lane = forge::asio::affine::lane{{.max_pending_operations = 1, .max_waiting_submissions = 1}};
   auto executor = lane.get_executor();
   auto blocker = std::make_shared<blocking_operation>();

   auto active = boost::asio::co_spawn(
      runtime.context(), executor.execute({.name = "active"}, [blocker] { blocker->run(); }), boost::asio::use_future);
   blocker->wait_started();
   auto pending = boost::asio::co_spawn(
      runtime.context(), executor.execute({.name = "pending"}, [] {}), boost::asio::use_future);
   auto waiting = boost::asio::co_spawn(
      runtime.context(), executor.execute({.name = "waiting"}, [] {}), boost::asio::use_future);
   BOOST_REQUIRE(wait_until([&] {
      const auto value = lane.snapshot();
      return value.pending == 1 && value.waiting == 1;
   }));

   BOOST_CHECK_THROW(
      forge::asio::blocking::run(runtime, executor.execute({.name = "rejected"}, [] {})),
      forge::asio::exceptions::rejected);
   blocker->release();
   active.get();
   pending.get();
   waiting.get();
   BOOST_CHECK_EQUAL(lane.snapshot().rejected, 1U);
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(asio_affine_cancels_before_start_and_completion_wins_after_start) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lane = forge::asio::affine::lane{{.max_pending_operations = 2, .max_waiting_submissions = 2}};
   auto executor = lane.get_executor();
   auto first_blocker = std::make_shared<blocking_operation>();
   auto pending_ran = std::atomic_bool{false};
   auto pending_signal = boost::asio::cancellation_signal{};

   auto active = boost::asio::co_spawn(
      runtime.context(), executor.execute({.name = "active"}, [first_blocker] { first_blocker->run(); }),
      boost::asio::use_future);
   first_blocker->wait_started();
   auto canceled = boost::asio::co_spawn(
      runtime.context(),
      executor.execute({.name = "cancel-pending"}, [&] {
         pending_ran.store(true, std::memory_order_release);
      }),
      boost::asio::bind_cancellation_slot(pending_signal.slot(), boost::asio::use_future));
   BOOST_REQUIRE(wait_until([&] { return lane.snapshot().pending == 1; }));
   pending_signal.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK_THROW(canceled.get(), forge::asio::exceptions::canceled);
   BOOST_CHECK(!pending_ran.load(std::memory_order_acquire));
   first_blocker->release();
   active.get();

   auto running_blocker = std::make_shared<blocking_operation>();
   auto running_signal = boost::asio::cancellation_signal{};
   auto completed = boost::asio::co_spawn(
      runtime.context(),
      executor.execute({.name = "completion-wins"}, [running_blocker] {
         running_blocker->run();
         return 42;
      }),
      boost::asio::bind_cancellation_slot(running_signal.slot(), boost::asio::use_future));
   running_blocker->wait_started();
   running_signal.emit(boost::asio::cancellation_type::all);
   running_blocker->release();
   BOOST_CHECK_EQUAL(completed.get(), 42);
   const auto metrics = lane.snapshot();
   BOOST_CHECK_EQUAL(metrics.completed, 2U);
   BOOST_CHECK_EQUAL(metrics.canceled, 1U);
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(asio_affine_rejects_precanceled_submission_before_admission) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lane = forge::asio::affine::lane{};
   auto ready = std::atomic_bool{false};
   auto invoked = std::atomic_bool{false};
   auto cancellation = boost::asio::cancellation_signal{};
   auto result = boost::asio::co_spawn(
      runtime.context(),
      execute_after_precancel(lane.get_executor(), ready, invoked),
      boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));

   BOOST_REQUIRE(wait_until([&] { return ready.load(std::memory_order_acquire); }));
   cancellation.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK(result.get());
   BOOST_CHECK(!invoked.load(std::memory_order_acquire));
   BOOST_CHECK_EQUAL(lane.snapshot().canceled, 1U);
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(asio_affine_returns_to_caller_executor_and_supports_move_only_results) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto lane = forge::asio::affine::lane{};
   auto executor = lane.get_executor();

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto caller = std::this_thread::get_id();
      auto input = std::make_unique<int>(42);
      const auto affine_thread = co_await executor.execute({.name = "move-only"}, [value = std::move(input)]() mutable {
         return std::make_pair(std::this_thread::get_id(), std::move(value));
      });
      BOOST_CHECK(input == nullptr);
      BOOST_CHECK(affine_thread.first != caller);
      BOOST_CHECK(std::this_thread::get_id() == caller);
      BOOST_REQUIRE(affine_thread.second != nullptr);
      BOOST_CHECK_EQUAL(*affine_thread.second, 42);
   }());

   BOOST_CHECK_THROW(
      forge::asio::blocking::run(
         runtime,
         executor.execute({.name = "failure"}, []() -> int { throw std::runtime_error{"affine failed"}; })),
      std::runtime_error);
   const auto metrics = lane.snapshot();
   BOOST_CHECK_EQUAL(metrics.completed, 1U);
   BOOST_CHECK_EQUAL(metrics.failed, 1U);
   forge::asio::blocking::run(runtime, lane.shutdown());
}

BOOST_AUTO_TEST_CASE(asio_affine_shutdown_cancels_pending_and_waits_for_running_operation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto lane = forge::asio::affine::lane{{.max_pending_operations = 1, .max_waiting_submissions = 1}};
   auto executor = lane.get_executor();
   auto blocker = std::make_shared<blocking_operation>();
   auto pending_canceled = std::atomic_bool{false};
   auto waiting_rejected = std::atomic_bool{false};

   auto active = boost::asio::co_spawn(
      runtime.context(), executor.execute({.name = "active"}, [blocker] { blocker->run(); }), boost::asio::use_future);
   blocker->wait_started();
   boost::asio::co_spawn(runtime.context(), expect_affine_canceled(executor, pending_canceled), boost::asio::detached);
   BOOST_REQUIRE(wait_until([&] { return lane.snapshot().pending == 1; }));
   boost::asio::co_spawn(runtime.context(), expect_affine_rejected(executor, waiting_rejected), boost::asio::detached);
   BOOST_REQUIRE(wait_until([&] { return lane.snapshot().waiting == 1; }));

   auto shutdown = boost::asio::co_spawn(runtime.context(), lane.shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(wait_until([&] { return lane.snapshot().stopped; }));
   BOOST_REQUIRE(wait_until([&] { return pending_canceled.load(std::memory_order_acquire); }));
   BOOST_REQUIRE(wait_until([&] { return waiting_rejected.load(std::memory_order_acquire); }));
   BOOST_CHECK(shutdown.wait_for(std::chrono::milliseconds{10}) == std::future_status::timeout);
   blocker->release();
   active.get();
   shutdown.get();
   const auto metrics = lane.snapshot();
   BOOST_CHECK_EQUAL(metrics.running, 0U);
   BOOST_CHECK_EQUAL(metrics.canceled, 1U);
   BOOST_CHECK_EQUAL(metrics.rejected, 1U);
}
