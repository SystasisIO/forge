#include <boost/test/unit_test.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

import forge.asio.cancellation_scope;
import forge.asio.notification;
import forge.asio.runtime;

namespace {

template <typename Result> Result take(forge::asio::runtime& runtime, std::future<Result>& result) {
   if (result.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
      runtime.stop(); // External driver only; never stop/join from a runtime worker.
      throw std::runtime_error{"cancellation_scope regression did not finish"};
   }

   return result.get();
}

boost::asio::awaitable<void> pause(std::chrono::milliseconds delay) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor, delay};
   co_await timer.async_wait(boost::asio::use_awaitable);
}

boost::asio::awaitable<bool> wait_stopped(std::stop_token stop, std::shared_ptr<std::promise<void>> entered) {
   if (stop.stop_requested()) {
      co_return true;
   }

   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor, std::chrono::seconds{10}};
   entered->set_value();
   auto error = boost::system::error_code{};
   co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   co_return error == boost::asio::error::operation_aborted&& stop.stop_requested();
}

} // namespace

BOOST_AUTO_TEST_CASE(cancellation_scope_copies_source_array_before_its_storage_disappears) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   for (unsigned selected = 0; selected != 3; ++selected) {
      auto origins = std::array<std::stop_source, 3>{};
      auto scope = std::unique_ptr<forge::asio::cancellation_scope>{};
      {
         auto tokens = std::array{origins[0].get_token(), origins[1].get_token(), origins[2].get_token()};
         scope = std::make_unique<forge::asio::cancellation_scope>(runtime.context().get_executor(), tokens,
                                                                   boost::asio::cancellation_state{});
         tokens.fill(std::stop_token{});
      }

      auto entered = std::make_shared<std::promise<void>>();
      auto ready = entered->get_future();
      auto result = boost::asio::co_spawn(
          runtime.context(), scope->run<bool>([entered](std::stop_token stop) { return wait_stopped(stop, entered); }),
          boost::asio::use_future);
      take(runtime, ready);
      origins[selected].request_stop();
      BOOST_CHECK(take(runtime, result));
   }

   auto origin = std::stop_source{};
   auto scope = forge::asio::cancellation_scope{
       runtime.context().get_executor(), std::array{std::stop_token{}, origin.get_token(), origin.get_token()}, {}};
   auto deliveries = 0U;
   const auto counted = std::stop_callback{scope.stop_token(), [&deliveries] { ++deliveries; }};
   origin.request_stop();
   BOOST_CHECK_EQUAL(deliveries, 1U); // Duplicate sources share one sticky native stop latch.
}

BOOST_AUTO_TEST_CASE(cancellation_scope_preserves_values_void_and_original_failure) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto owner = std::stop_source{};
   auto scope = forge::asio::cancellation_scope{runtime.context().get_executor(), std::array{owner.get_token()}, {}};
   auto result = boost::asio::co_spawn(runtime.context(),
                                       scope.run<int>([owner](std::stop_token) mutable -> boost::asio::awaitable<int> {
                                          owner.request_stop();
                                          co_return 73;
                                       }),
                                       boost::asio::use_future);
   BOOST_CHECK_EQUAL(take(runtime, result), 73); // No blanket cancel-wins policy.
   BOOST_CHECK(scope.stop_token().stop_requested());
   BOOST_CHECK_THROW(
       static_cast<void>(scope.run<int>([](std::stop_token) -> boost::asio::awaitable<int> { co_return 0; })),
       forge::asio::exceptions::invalid_state);

   auto done = forge::asio::cancellation_scope{runtime.context().get_executor(), {}, {}};
   auto void_result = boost::asio::co_spawn(
       runtime.context(), done.run<void>([](std::stop_token) -> boost::asio::awaitable<void> { co_return; }),
       boost::asio::use_future);
   take(runtime, void_result);

   auto failing_owner = std::stop_source{};
   auto failing =
       forge::asio::cancellation_scope{runtime.context().get_executor(), std::array{failing_owner.get_token()}, {}};
   auto failed = boost::asio::co_spawn(
       runtime.context(), failing.run<void>([failing_owner](std::stop_token) mutable -> boost::asio::awaitable<void> {
          failing_owner.request_stop();
          throw std::runtime_error{"original-operation-failure"};
          co_return;
       }),
       boost::asio::use_future);
   try {
      take(runtime, failed);
      BOOST_FAIL("Operation failure disappeared");
   } catch (const std::runtime_error& failure) {
      BOOST_CHECK_EQUAL(std::string{failure.what()}, "original-operation-failure");
   }
}

BOOST_AUTO_TEST_CASE(cancellation_scope_retains_unpolled_work_and_late_stream_links) {
   auto runtime = forge::asio::runtime{{.worker_threads = 1}};
   auto owner = std::stop_source{};
   auto payload = std::make_shared<int>(19);
   const auto weak = std::weak_ptr{payload};
   auto scope = std::make_unique<forge::asio::cancellation_scope>(
       runtime.context().get_executor(), std::array{owner.get_token()}, boost::asio::cancellation_state{});
   auto pending = scope->run<int>([payload = std::move(payload)](std::stop_token stop) -> boost::asio::awaitable<int> {
      co_return stop.stop_requested() ? *payload : 0;
   });
   scope.reset(); // No raw this in the lazy awaitable; parent is disconnected.
   owner.request_stop();
   auto result = boost::asio::co_spawn(runtime.context(), std::move(pending), boost::asio::use_future);
   BOOST_CHECK_EQUAL(take(runtime, result), 19);
   BOOST_CHECK(weak.expired());

   auto stream_owner = std::stop_source{};
   auto stream_scope =
       forge::asio::cancellation_scope{runtime.context().get_executor(), std::array{stream_owner.get_token()}, {}};
   auto opened = boost::asio::co_spawn(
       runtime.context(), stream_scope.run<int>([](std::stop_token) -> boost::asio::awaitable<int> { co_return 1; }),
       boost::asio::use_future);
   BOOST_CHECK_EQUAL(take(runtime, opened), 1);
   auto interrupted = false;
   const auto stream_stop = std::stop_callback{stream_scope.stop_token(), [&interrupted] { interrupted = true; }};
   stream_owner.request_stop();
   BOOST_CHECK(interrupted); // Links remain after opening/join, not only during run.
}

BOOST_AUTO_TEST_CASE(cancellation_scope_latches_each_precancelled_source_before_child_io) {
   auto runtime = forge::asio::runtime{{.worker_threads = 1}};
   for (unsigned source = 0; source != 4; ++source) {
      auto owner = std::stop_source{};
      auto task = std::stop_source{};
      auto third = std::stop_source{};
      auto signal = boost::asio::cancellation_signal{};
      auto parent = boost::asio::cancellation_state{signal.slot(), boost::asio::enable_total_cancellation{}};
      if (source == 0) {
         owner.request_stop();
      } else if (source == 1) {
         task.request_stop();
      } else if (source == 2) {
         third.request_stop();
      } else {
         signal.emit(boost::asio::cancellation_type::terminal);
      }

      auto scope = forge::asio::cancellation_scope{
          runtime.context().get_executor(), std::array{owner.get_token(), task.get_token(), third.get_token()}, parent};
      auto result = boost::asio::co_spawn(runtime.context(),
                                          scope.run<bool>([](std::stop_token stop) -> boost::asio::awaitable<bool> {
                                             // Consumer policy checks the sticky token before external I/O.
                                             co_return stop.stop_requested();
                                          }),
                                          boost::asio::use_future);
      BOOST_CHECK(take(runtime, result));
      if (source == 3) {
         BOOST_CHECK(parent.cancelled() == boost::asio::cancellation_type::terminal);
      }
   }
}

BOOST_AUTO_TEST_CASE(cancellation_scope_concurrent_standard_stop_joins_child) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto owner = std::stop_source{};
   auto task = std::stop_source{};
   auto third = std::stop_source{};
   auto scope = forge::asio::cancellation_scope{
       runtime.context().get_executor(), std::array{owner.get_token(), task.get_token(), third.get_token()}, {}};
   auto entered = std::make_shared<std::promise<void>>();
   auto ready = entered->get_future();
   auto result = boost::asio::co_spawn(
       runtime.context(), scope.run<bool>([entered](std::stop_token stop) { return wait_stopped(stop, entered); }),
       boost::asio::use_future);
   take(runtime, ready);
   auto one = std::jthread{[owner]() mutable { owner.request_stop(); }};
   auto two = std::jthread{[task]() mutable { task.request_stop(); }};
   auto three = std::jthread{[third]() mutable { third.request_stop(); }};
   one.join();
   two.join();
   three.join();
   BOOST_CHECK(take(runtime, result));
}

BOOST_AUTO_TEST_CASE(cancellation_scope_each_standard_link_and_direct_stop_interrupts_active_io) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   for (unsigned selected = 0; selected != 4; ++selected) {
      auto owner = std::stop_source{};
      auto task = std::stop_source{};
      auto third = std::stop_source{};
      auto scope = forge::asio::cancellation_scope{
          runtime.context().get_executor(), std::array{owner.get_token(), task.get_token(), third.get_token()}, {}};
      auto entered = std::make_shared<std::promise<void>>();
      auto ready = entered->get_future();
      auto result = boost::asio::co_spawn(
          runtime.context(), scope.run<bool>([entered](std::stop_token stop) { return wait_stopped(stop, entered); }),
          boost::asio::use_future);
      take(runtime, ready);
      if (selected == 0) {
         owner.request_stop();
      } else if (selected == 1) {
         task.request_stop();
      } else if (selected == 2) {
         third.request_stop();
      } else {
         scope.request_stop();
      }

      BOOST_CHECK(take(runtime, result));
   }
}

BOOST_AUTO_TEST_CASE(cancellation_scope_deadline_expiry_and_disarm_on_value_or_error) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto timeout = forge::asio::cancellation_scope{runtime.context().get_executor(), {}, {}};
   auto expired =
       boost::asio::co_spawn(runtime.context(),
                             timeout.run<bool>([entered = std::make_shared<std::promise<void>>()](
                                                   std::stop_token stop) { return wait_stopped(stop, entered); },
                                               std::chrono::milliseconds{20}),
                             boost::asio::use_future);
   BOOST_CHECK(take(runtime, expired));

   for (const auto fails : {false, true}) {
      auto scope = forge::asio::cancellation_scope{runtime.context().get_executor(), {}, {}};
      auto result = boost::asio::co_spawn(runtime.context(),
                                          scope.run<int>(
                                              [fails](std::stop_token) -> boost::asio::awaitable<int> {
                                                 if (fails) {
                                                    throw std::runtime_error{"deadline-disarmed-error"};
                                                 }
                                                 co_return 11;
                                              },
                                              std::chrono::milliseconds{40}),
                                          boost::asio::use_future);
      if (fails) {
         BOOST_CHECK_THROW(take(runtime, result), std::runtime_error);
      } else {
         BOOST_CHECK_EQUAL(take(runtime, result), 11);
      }

      auto elapsed =
          boost::asio::co_spawn(runtime.context(), pause(std::chrono::milliseconds{80}), boost::asio::use_future);
      take(runtime, elapsed);
      BOOST_CHECK(!scope.stop_token().stop_requested()); // Queued timer must not cancel after join.
   }
}

BOOST_AUTO_TEST_CASE(cancellation_scope_cancellation_waits_for_child_cleanup_retirement) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto owner = std::stop_source{};
   auto scope = forge::asio::cancellation_scope{runtime.context().get_executor(), std::array{owner.get_token()}, {}};
   auto entered = std::make_shared<std::promise<void>>();
   auto cleaning = std::make_shared<std::promise<void>>();
   auto cleanup = std::make_shared<forge::asio::notification>();
   auto ready = entered->get_future();
   auto cleanup_ready = cleaning->get_future();
   auto payload = std::make_shared<int>(1);
   const auto weak = std::weak_ptr{payload};
   auto result = boost::asio::co_spawn(
       runtime.context(),
       scope.run<void>([entered, cleaning, cleanup,
                        payload = std::move(payload)](std::stop_token stop) -> boost::asio::awaitable<void> {
          (void)co_await wait_stopped(stop, entered);
          // Cleanup is part of this child, not detached work.
          co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
          cleaning->set_value();
          co_await cleanup->async_wait(0);
          throw std::runtime_error{"retired-child-failure"};
       }),
       boost::asio::use_future);
   take(runtime, ready);
   owner.request_stop();
   take(runtime, cleanup_ready);
   BOOST_CHECK(result.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
   BOOST_CHECK(!weak.expired());
   cleanup->notify();
   try {
      take(runtime, result);
      BOOST_FAIL("Retired child failure disappeared");
   } catch (const std::runtime_error& failure) {
      BOOST_CHECK_EQUAL(std::string{failure.what()}, "retired-child-failure");
   }
   BOOST_CHECK(weak.expired());
}

BOOST_AUTO_TEST_CASE(cancellation_scope_parent_stop_after_open_interrupts_second_read_and_joins) {
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto parent_executor = boost::asio::make_strand(runtime.context());
   auto signal = std::make_shared<boost::asio::cancellation_signal>();
   auto second_read = std::make_shared<std::promise<void>>();
   auto ready = second_read->get_future();
   auto cleaned = std::make_shared<std::atomic_bool>(false);
   auto result = boost::asio::co_spawn(
       parent_executor,
       [second_read, cleaned]() -> boost::asio::awaitable<bool> {
          co_await boost::asio::this_coro::throw_if_cancelled(false);
          auto scope = forge::asio::cancellation_scope{
              co_await boost::asio::this_coro::executor, {}, co_await boost::asio::this_coro::cancellation_state};
          co_return co_await scope.run<bool>(
              [second_read, cleaned](std::stop_token stop) -> boost::asio::awaitable<bool> {
                 // Models one joined open+read lifetime. Subsequent native waits
                 // use this child's output slot, never scope's borrowed parent.
                 const auto cleanup =
                     boost::scope::scope_exit([cleaned] { cleaned->store(true, std::memory_order_release); });
                 co_await pause(std::chrono::milliseconds{1});       // First read after open.
                 co_return co_await wait_stopped(stop, second_read); // Second native read.
              });
       },
       boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_future));
   take(runtime, ready);
   boost::asio::post(parent_executor, [signal] { signal->emit(boost::asio::cancellation_type::terminal); });
   BOOST_CHECK(take(runtime, result));
   BOOST_CHECK(cleaned->load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(cancellation_scope_preserves_parent_filters_throw_policy_and_next_operation) {
   for (const auto throws : {false, true}) {
      auto runtime = forge::asio::runtime{{.worker_threads = 2}};
      auto parent_executor = boost::asio::make_strand(runtime.context());
      auto signal = std::make_shared<boost::asio::cancellation_signal>();
      auto owner = std::stop_source{};
      auto entered = std::make_shared<std::promise<void>>();
      auto next_timer = std::make_shared<std::promise<void>>();
      auto ready = entered->get_future();
      auto timer_ready = next_timer->get_future();
      auto result = boost::asio::co_spawn(
          parent_executor,
          [owner, entered, next_timer, throws]() -> boost::asio::awaitable<bool> {
             auto counts = std::make_shared<std::pair<unsigned, unsigned>>();
             co_await boost::asio::this_coro::reset_cancellation_state(
                 [counts](boost::asio::cancellation_type type) {
                    ++counts->first;
                    return type & boost::asio::cancellation_type::partial;
                 },
                 [counts](boost::asio::cancellation_type type) {
                    ++counts->second;
                    return type & boost::asio::cancellation_type::partial;
                 });
             co_await boost::asio::this_coro::throw_if_cancelled(throws);
             auto completed = false;
             {
                auto scope = forge::asio::cancellation_scope{co_await boost::asio::this_coro::executor,
                                                             std::array{owner.get_token()},
                                                             co_await boost::asio::this_coro::cancellation_state};
                completed =
                    co_await scope.run<bool>([entered](std::stop_token stop) { return wait_stopped(stop, entered); });
             }

             // The first terminal signal was filtered out, not reset by scope.
             const auto parent = co_await boost::asio::this_coro::cancellation_state;
             const auto intact = parent.cancelled() == boost::asio::cancellation_type::none &&
                                 (co_await boost::asio::this_coro::throw_if_cancelled()) == throws &&
                                 counts->first == 1 && counts->second == 1 && !parent.slot().has_handler();
             auto timer =
                 boost::asio::steady_timer{co_await boost::asio::this_coro::executor, std::chrono::seconds{10}};
             next_timer->set_value();
             auto error = boost::system::error_code{};
             co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
             co_return completed && intact && error == boost::asio::error::operation_aborted &&
                 (co_await boost::asio::this_coro::throw_if_cancelled()) == throws&& parent.cancelled() ==
                     boost::asio::cancellation_type::partial &&
                 counts->first == 2 && counts->second == 2;
          },
          boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_future));
      take(runtime, ready);
      boost::asio::post(parent_executor, [signal, owner]() mutable {
         signal->emit(boost::asio::cancellation_type::terminal); // Filtered, not forgotten.
         owner.request_stop();
      });
      take(runtime, timer_ready);
      boost::asio::post(parent_executor, [signal] { signal->emit(boost::asio::cancellation_type::partial); });
      BOOST_CHECK(take(runtime, result));
   }
}
