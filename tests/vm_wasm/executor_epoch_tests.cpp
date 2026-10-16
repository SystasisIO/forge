module;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

module forge.vm.wasm.optimizing.executor;

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.vm.wasm.interpret.exceptions;
import forge.vm.wasm.optimizing.invocation_abi;

#include "details/executor_epoch.hxx"

namespace forge::vm::wasm::optimizing {
// Owning test access only; no runtime hook, parallel lifecycle or exported API.
struct executor_epoch_test_access {
   static std::unique_lock<std::mutex> hold_completion(executor_epoch& owner) {
      return std::unique_lock{owner._completion_mutex};
   }
   static auto notification_epoch(executor_epoch& owner) {
      return owner._changed.epoch();
   }
};
} // namespace forge::vm::wasm::optimizing

namespace {
using forge::vm::wasm::optimizing::executor_epoch;
using forge::vm::wasm::optimizing::invocation_abi;
using forge::vm::wasm::optimizing::trap_reason;
using namespace std::chrono_literals;

struct runtime_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 1}};
   forge::asio::task::scheduler scheduler{runtime};
   executor_epoch epoch{scheduler};

   runtime_fixture() {
      forge::asio::blocking::run(runtime, epoch.start(std::chrono::steady_clock::now() + 2s));
   }
   ~runtime_fixture() {
      forge::asio::blocking::run(runtime, epoch.shutdown());
      forge::asio::blocking::run(runtime, scheduler.shutdown());
   }
};

bool wait_for(const std::atomic<bool>& value, std::chrono::milliseconds limit = 1s) {
   const auto deadline = std::chrono::steady_clock::now() + limit;
   while (!value.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(1ms);
   return value.load(std::memory_order_acquire);
}
} // namespace

BOOST_AUTO_TEST_SUITE(optimizing_executor_epoch)

BOOST_AUTO_TEST_CASE(admission_requires_started_ticker_and_real_ready_state) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto epoch = executor_epoch{scheduler};
   auto context = invocation_abi{};
   BOOST_TEST(!epoch.ready());
   BOOST_CHECK_THROW((executor_epoch::activation{epoch, context, std::chrono::steady_clock::now() + 1s, {}, {}}),
                     forge::vm::wasm::interpret::exceptions::execution_unavailable);
   BOOST_TEST(context.epoch == nullptr);
   forge::asio::blocking::run(runtime, epoch.start(std::chrono::steady_clock::now() + 1s));
   BOOST_TEST(epoch.ready());
   forge::asio::blocking::run(runtime, epoch.shutdown());
   BOOST_TEST(!epoch.ready());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, epoch.start(std::chrono::steady_clock::now() + 1s)),
                     forge::vm::wasm::interpret::exceptions::execution_unavailable);
   forge::asio::blocking::run(runtime, scheduler.shutdown());
}

BOOST_AUTO_TEST_CASE(already_stopped_tokens_publish_before_any_guest_effect) {
   auto fixture = runtime_fixture{};
   auto stopped = std::stop_source{};
   stopped.request_stop();
   for (const auto caller : {true, false}) {
      auto context = invocation_abi{};
      auto effects = 0;
      {
         auto activation = executor_epoch::activation{fixture.epoch, context, std::chrono::steady_clock::now() + 1s,
                                                      caller ? stopped.get_token() : std::stop_token{},
                                                      caller ? std::stop_token{} : stopped.get_token()};
         if (activation.poll() == trap_reason::none)
            ++effects;
         BOOST_TEST(static_cast<unsigned>(activation.finish()) == static_cast<unsigned>(trap_reason::cancelled));
         BOOST_TEST(context.epoch != nullptr);
         BOOST_TEST(context.sticky_reason != nullptr);
      }
      BOOST_TEST(effects == 0);
      BOOST_TEST(context.epoch == nullptr);
      BOOST_TEST(context.sticky_reason == nullptr);
   }
}

BOOST_AUTO_TEST_CASE(shared_wake_rearms_without_cancelling_independent_nested_activation) {
   auto fixture = runtime_fixture{};
   auto outer_context = invocation_abi{};
   auto nested_context = invocation_abi{};
   auto outer_stop = std::stop_source{};
   auto outer = executor_epoch::activation{
       fixture.epoch, outer_context, std::chrono::steady_clock::now() + 1s, outer_stop.get_token(), {}};
   auto nested =
       executor_epoch::activation{fixture.epoch, nested_context, std::chrono::steady_clock::now() + 1s, {}, {}};
   const auto old_epoch = fixture.epoch.value();
   outer_stop.request_stop();
   BOOST_TEST(fixture.epoch.value() > old_epoch);
   BOOST_TEST(static_cast<unsigned>(outer.reason()) == static_cast<unsigned>(trap_reason::cancelled));
   BOOST_TEST(static_cast<unsigned>(nested.poll()) == static_cast<unsigned>(trap_reason::none));
   const auto rearmed = nested_context.epoch_deadline;
   BOOST_TEST(rearmed > 0U);
   BOOST_TEST(static_cast<unsigned>(nested.finish()) == static_cast<unsigned>(trap_reason::none));
   BOOST_TEST(static_cast<unsigned>(outer.finish()) == static_cast<unsigned>(trap_reason::cancelled));
}

BOOST_AUTO_TEST_CASE(finish_checks_original_absolute_clock_after_callback_deregistration) {
   auto fixture = runtime_fixture{};
   auto context = invocation_abi{};
   auto activation = executor_epoch::activation{fixture.epoch, context, std::chrono::steady_clock::now() + 5ms, {}, {}};
   BOOST_TEST(static_cast<unsigned>(activation.reason()) == static_cast<unsigned>(trap_reason::none));
   std::this_thread::sleep_for(10ms);
   BOOST_TEST(static_cast<unsigned>(activation.finish()) == static_cast<unsigned>(trap_reason::deadline));
   BOOST_TEST(context.epoch != nullptr);
}

BOOST_AUTO_TEST_CASE(sticky_first_reason_is_never_cleared_by_rearm_or_later_stop) {
   auto fixture = runtime_fixture{};
   auto context = invocation_abi{};
   auto stop = std::stop_source{};
   auto activation =
       executor_epoch::activation{fixture.epoch, context, std::chrono::steady_clock::now() - 1ms, stop.get_token(), {}};
   stop.request_stop();
   BOOST_TEST(static_cast<unsigned>(activation.poll()) == static_cast<unsigned>(trap_reason::deadline));
   BOOST_TEST(static_cast<unsigned>(activation.finish()) == static_cast<unsigned>(trap_reason::deadline));
}

BOOST_AUTO_TEST_CASE(epoch_saturation_does_not_wrap_and_blocks_new_admission) {
   auto fixture = runtime_fixture{};
   {
      auto context = invocation_abi{};
      auto stop = std::stop_source{};
      auto activation = executor_epoch::activation{
          fixture.epoch, context, std::chrono::steady_clock::now() + 1s, stop.get_token(), {}};
      // Test-only monotonic RMW moves the SAME backing word to the finite edge;
      // no alternate epoch owner, reset/store, parser or emitted ABI is introduced.
      auto word = std::atomic_ref{*const_cast<std::uint64_t*>(context.epoch)};
      auto observed = word.load(std::memory_order_acquire);
      const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
      while (observed < maximum - 1 &&
             !word.compare_exchange_weak(observed, maximum - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      }
      stop.request_stop();
      BOOST_TEST(fixture.epoch.value() == maximum);
      BOOST_TEST(!fixture.epoch.ready());
      BOOST_TEST(fixture.epoch.failed());
      auto rejected = invocation_abi{};
      BOOST_CHECK_THROW(
          (executor_epoch::activation{fixture.epoch, rejected, std::chrono::steady_clock::now() + 1s, {}, {}}),
          forge::vm::wasm::interpret::exceptions::execution_unavailable);
      BOOST_TEST(rejected.epoch == nullptr);
      BOOST_TEST(static_cast<unsigned>(activation.finish()) == static_cast<unsigned>(trap_reason::cancelled));
   }
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.runtime, fixture.epoch.shutdown()),
                     forge::vm::wasm::interpret::exceptions::execution_unavailable);
}

BOOST_AUTO_TEST_CASE(mandatory_drain_ignores_cancellation_after_it_has_entered) {
   auto fixture = runtime_fixture{};
   auto context = invocation_abi{};
   auto activation = std::optional<executor_epoch::activation>{};
   activation.emplace(fixture.epoch, context, std::chrono::steady_clock::now() + 1s, std::stop_token{},
                      std::stop_token{});
   auto cancellation = boost::asio::cancellation_signal{};
   auto drain =
       boost::asio::co_spawn(fixture.runtime.context(), fixture.epoch.shutdown(),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   const auto deadline = std::chrono::steady_clock::now() + 1s;
   while (activation->reason() == trap_reason::none && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(1ms);
   const auto entered_drain = activation->reason() == trap_reason::cancelled;
   cancellation.emit(boost::asio::cancellation_type::all);
   const auto held_until_retirement = drain.wait_for(10ms) == std::future_status::timeout;
   activation.reset();
   const auto completed = drain.wait_for(1s) == std::future_status::ready;
   BOOST_TEST(entered_drain);
   BOOST_TEST(held_until_retirement);
   BOOST_REQUIRE(completed);
   BOOST_CHECK_NO_THROW(drain.get());
   BOOST_TEST(context.epoch == nullptr);
}

BOOST_AUTO_TEST_CASE(already_cancelled_caller_enters_standard_noncancellable_cleanup_before_child_await) {
   auto fixture = runtime_fixture{};
   auto cancellation = boost::asio::cancellation_signal{};
   auto drain = boost::asio::co_spawn(
       fixture.runtime.context(),
       [&]() -> boost::asio::awaitable<void> {
          co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
          cancellation.emit(boost::asio::cancellation_type::all);
          // Awaiting a child coroutine can reject before that child's body is
          // entered. The enclosing owner enters its mandatory cleanup region
          // first, as existing affine/gate lifecycle paths do.
          co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
          co_await fixture.epoch.shutdown();
       },
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   BOOST_REQUIRE(drain.wait_for(1s) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drain.get());
   BOOST_TEST(!fixture.epoch.ready());
}

BOOST_AUTO_TEST_CASE(last_retirement_and_zero_observation_share_the_complete_notification_gate) {
   auto fixture = runtime_fixture{};
   auto context = invocation_abi{};
   auto activation = std::optional<executor_epoch::activation>{};
   activation.emplace(fixture.epoch, context, std::chrono::steady_clock::now() + 1s, std::stop_token{},
                      std::stop_token{});
   auto drain = boost::asio::co_spawn(fixture.runtime.context(), fixture.epoch.shutdown(), boost::asio::use_future);
   const auto deadline = std::chrono::steady_clock::now() + 1s;
   while (activation->reason() == trap_reason::none && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(1ms);
   const auto stopped = activation->reason() == trap_reason::cancelled;
   auto gate = forge::vm::wasm::optimizing::executor_epoch_test_access::hold_completion(fixture.epoch);
   auto retiring = std::atomic<bool>{false};
   auto retired = std::atomic<bool>{false};
   auto worker = std::thread{[&] {
      retiring.store(true, std::memory_order_release);
      activation.reset();
      retired.store(true, std::memory_order_release);
   }};
   const auto reached_retirement = wait_for(retiring);
   const auto held_retirement = !retired.load(std::memory_order_acquire);
   const auto held_drain = drain.wait_for(10ms) == std::future_status::timeout;
   gate.unlock();
   worker.join();
   const auto completed = drain.wait_for(1s) == std::future_status::ready;
   BOOST_TEST(stopped);
   BOOST_TEST(reached_retirement);
   BOOST_TEST(held_retirement);
   BOOST_TEST(held_drain);
   BOOST_TEST(retired.load(std::memory_order_acquire));
   BOOST_REQUIRE(completed);
   BOOST_CHECK_NO_THROW(drain.get());
   BOOST_TEST(context.epoch == nullptr);
}

BOOST_AUTO_TEST_CASE(zero_active_drain_waits_for_completion_gate_and_late_admission_never_notifies) {
   auto fixture = runtime_fixture{};
   auto gate = forge::vm::wasm::optimizing::executor_epoch_test_access::hold_completion(fixture.epoch);
   auto entered = std::atomic<bool>{false};
   auto drain = boost::asio::co_spawn(
       fixture.runtime.context(),
       [&]() -> boost::asio::awaitable<void> {
          entered.store(true, std::memory_order_release);
          co_await fixture.epoch.shutdown();
       },
       boost::asio::use_future);
   const auto reached_drain = wait_for(entered);
   const auto held_zero_observation = drain.wait_for(10ms) == std::future_status::timeout;
   gate.unlock();
   const auto completed = drain.wait_for(1s) == std::future_status::ready;
   BOOST_TEST(reached_drain);
   BOOST_TEST(held_zero_observation);
   BOOST_REQUIRE(completed);
   BOOST_CHECK_NO_THROW(drain.get());
   const auto epoch = forge::vm::wasm::optimizing::executor_epoch_test_access::notification_epoch(fixture.epoch);
   for (auto attempt = 0; attempt != 32; ++attempt) {
      auto rejected = invocation_abi{};
      BOOST_CHECK_THROW(
          (executor_epoch::activation{fixture.epoch, rejected, std::chrono::steady_clock::now() + 1s, {}, {}}),
          forge::vm::wasm::interpret::exceptions::execution_unavailable);
      BOOST_TEST(rejected.epoch == nullptr);
   }
   BOOST_TEST(forge::vm::wasm::optimizing::executor_epoch_test_access::notification_epoch(fixture.epoch) == epoch);
}

BOOST_AUTO_TEST_CASE(full_running_scheduler_capacity_cannot_be_mistaken_for_ready) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{
       runtime, forge::asio::task::scheduler::options{.max_blocking_tasks = 1, .max_awaitable_tasks = 1}};
   auto release = forge::asio::notification{};
   auto started = std::atomic<bool>{false};
   auto occupied = scheduler.submit(forge::asio::task::awaitable{
       .name = "epoch-capacity-control",
       .work = [&](forge::asio::task::context& context) -> boost::asio::awaitable<void> {
          const auto observed = release.epoch();
          started.store(true, std::memory_order_release);
          static_cast<void>(co_await release.async_wait(observed, context.stop_token()));
       },
   });
   const auto actually_started = wait_for(started);
   auto epoch = executor_epoch{scheduler};
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, epoch.start(std::chrono::steady_clock::now() + 20ms));
   } catch (...) {
      failure = std::current_exception();
   }
   release.notify();
   forge::asio::blocking::run(runtime, occupied.wait());
   forge::asio::blocking::run(runtime, epoch.shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   BOOST_TEST(actually_started);
   BOOST_REQUIRE(failure != nullptr);
   BOOST_CHECK_THROW(std::rethrow_exception(failure), forge::vm::wasm::interpret::exceptions::execution_unavailable);
   BOOST_TEST(!epoch.ready());
}

BOOST_AUTO_TEST_CASE(compute_stop_bumps_backing_word_even_when_ticker_cannot_run) {
   auto fixture = runtime_fixture{};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
   auto entered = std::atomic<bool>{false};
   auto exited = std::atomic<bool>{false};
   auto force_exit = std::atomic<bool>{false};
   auto observed_reason = std::atomic<unsigned>{static_cast<unsigned>(trap_reason::none)};
   auto operation = forge::asio::blocking::run(
       fixture.runtime,
       pool.get_executor().submit({.name = "epoch-stop-backing-control"}, [&](forge::asio::compute::context& compute) {
          auto context = invocation_abi{};
          auto activation = executor_epoch::activation{
              fixture.epoch, context, std::chrono::steady_clock::now() + 2s, {}, compute.stop_token()};
          entered.store(true, std::memory_order_release);
          // C++ backing-word control, NOT generated guest proof.
          while (!force_exit.load(std::memory_order_acquire)) {
             const auto epoch =
                 std::atomic_ref{*const_cast<std::uint64_t*>(context.epoch)}.load(std::memory_order_acquire);
             if (epoch >= context.epoch_deadline && activation.poll() != trap_reason::none)
                break;
          }
          observed_reason.store(static_cast<unsigned>(activation.finish()), std::memory_order_release);
          exited.store(true, std::memory_order_release);
       }));
   const auto compute_entered = wait_for(entered);
   auto mutex = std::mutex{};
   auto changed = std::condition_variable{};
   auto released = false;
   auto io_blocked = std::atomic<bool>{false};
   boost::asio::post(fixture.runtime.context(), [&] {
      auto lock = std::unique_lock{mutex};
      io_blocked.store(true, std::memory_order_release);
      changed.wait_for(lock, 1s, [&] { return released; });
   });
   const auto ticker_blocked = wait_for(io_blocked);
   pool.request_stop();
   const auto stopped_without_tick = wait_for(exited, 100ms);
   force_exit.store(true, std::memory_order_release);
   {
      const auto lock = std::lock_guard{mutex};
      released = true;
   }
   changed.notify_all();
   auto terminal = std::exception_ptr{};
   try {
      forge::asio::blocking::run(fixture.runtime, std::move(operation).wait());
   } catch (...) {
      terminal = std::current_exception();
   }
   forge::asio::blocking::run(fixture.runtime, pool.shutdown());
   BOOST_TEST(compute_entered);
   BOOST_TEST(ticker_blocked);
   BOOST_TEST(stopped_without_tick);
   BOOST_TEST(observed_reason.load(std::memory_order_acquire) == static_cast<unsigned>(trap_reason::cancelled));
   BOOST_REQUIRE(terminal != nullptr);
   BOOST_CHECK_THROW(std::rethrow_exception(terminal), forge::asio::exceptions::canceled);
}

BOOST_AUTO_TEST_SUITE_END()
