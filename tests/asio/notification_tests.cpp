#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include "../../libraries/asio/details/notification_waiter.hxx"
#include "../../libraries/asio/details/async_waiter.hxx"

import forge.asio.notification;
import forge.asio.runtime;

namespace {

boost::asio::awaitable<bool> wait_for_cancellation(
    forge::asio::notification& signal, std::atomic_bool& started, std::stop_token stop) {
   const auto before = co_await boost::asio::this_coro::executor;
   const auto observed = signal.epoch();
   started.store(true, std::memory_order_release);
   auto canceled = false;
   try {
      (void)co_await signal.async_wait(observed, std::move(stop));
   } catch (const boost::system::system_error& error) {
      canceled = error.code() == boost::asio::error::operation_aborted;
   }
   const auto after = co_await boost::asio::this_coro::executor;
   co_return canceled && before == after;
}

boost::asio::awaitable<void> notify(forge::asio::notification* signal) {
   signal->notify();
   co_return;
}

boost::asio::awaitable<bool> wait_owned_early_completion(bool sticky) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto waiter = std::make_shared<forge::asio::detail::notification_waiter>(executor);
   if (sticky) {
      waiter->wake();
   }
   const auto error = co_await waiter->wait_until(sticky ? boost::asio::steady_timer::time_point::max()
                                                         : std::chrono::steady_clock::now());
   // No owner survives the coroutine: completion can release the timer while
   // its initiating function is still returning on another runtime worker.
   co_return sticky ? error == boost::asio::error::operation_aborted : !error;
}

boost::asio::awaitable<std::size_t> repeat_owned_early_completion(std::size_t count) {
   auto completed = std::size_t{};
   for (auto index = std::size_t{}; index < count; ++index) {
      completed += co_await wait_owned_early_completion(index % 2 == 0);
   }
   co_return completed;
}

} // namespace

BOOST_AUTO_TEST_CASE(asio_notification_waiter_survives_early_completion_releasing_last_external_owner) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   constexpr auto count = std::size_t{10000};
   auto results = std::vector<std::future<std::size_t>>{};
   for (auto index = 0; index < 4; ++index) {
      results.push_back(
          boost::asio::co_spawn(runtime.context(), repeat_owned_early_completion(count), boost::asio::use_future));
   }
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
   for (auto& result : results) {
      BOOST_REQUIRE(result.wait_until(deadline) == std::future_status::ready);
      BOOST_CHECK_EQUAL(result.get(), count);
   }
}

BOOST_AUTO_TEST_CASE(asio_async_waiter_keeps_sticky_wake_before_wait_arm) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto waiter = std::make_shared<forge::asio::detail::async_waiter>(runtime.context().get_executor());

   auto notifier = std::thread{[waiter] { waiter->wake(); }};
   notifier.join();
   auto waiting = boost::asio::co_spawn(
       runtime.context(), waiter->wait_until(boost::asio::steady_timer::time_point::max()),
       boost::asio::use_future);

   BOOST_REQUIRE(waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK(waiting.get() == boost::asio::error::operation_aborted);
}

BOOST_AUTO_TEST_CASE(asio_notification_waiter_keeps_terminal_wake_before_wait_arm) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto waiter = std::make_shared<forge::asio::detail::notification_waiter>(runtime.context().get_executor());

   waiter->wake();
   auto waiting = boost::asio::co_spawn(
       runtime.context(), waiter->wait_until(boost::asio::steady_timer::time_point::max()),
       boost::asio::use_future);

   BOOST_REQUIRE(waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK(waiting.get() == boost::asio::error::operation_aborted);
}

BOOST_AUTO_TEST_CASE(asio_notification_waiter_serializes_expiry_and_wake) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};

   for (auto attempt = 0; attempt != 64; ++attempt) {
      auto waiter = std::make_shared<forge::asio::detail::notification_waiter>(runtime.context().get_executor());
      auto waiting = boost::asio::co_spawn(
          runtime.context(),
          waiter->wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds{1}),
          boost::asio::use_future);
      auto notifier = std::thread{[waiter] {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
         waiter->wake();
      }};
      notifier.join();

      BOOST_REQUIRE(waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      const auto result = waiting.get();
      BOOST_CHECK(!result || result == boost::asio::error::operation_aborted);
   }
}

BOOST_AUTO_TEST_CASE(asio_notification_preserves_late_and_racing_wakes) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto signal = forge::asio::notification{};

   const auto late_epoch = signal.epoch();
   signal.notify();
   auto late = boost::asio::co_spawn(runtime.context(), signal.async_wait(late_epoch), boost::asio::use_future);
   BOOST_REQUIRE(late.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NE(late.get(), late_epoch);

   auto serial = boost::asio::make_strand(runtime.context());
   const auto shared_epoch = signal.epoch();
   auto waiters = std::vector<std::future<forge::asio::notification::epoch_type>>{};
   waiters.reserve(64);
   for (auto index = 0; index != 64; ++index) {
      waiters.push_back(boost::asio::co_spawn(serial, signal.async_wait(shared_epoch), boost::asio::use_future));
   }
   // The serialized notifier runs after every waiter has subscribed. Terminal
   // delivery must wake these waiters without posting a new executor handler.
   auto notifying = boost::asio::co_spawn(serial, notify(&signal), boost::asio::use_future);
   BOOST_REQUIRE(notifying.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(notifying.get());
   for (auto& waiter : waiters) {
      BOOST_REQUIRE(waiter.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      BOOST_CHECK_NE(waiter.get(), shared_epoch);
   }

   for (auto attempt = 0; attempt != 64; ++attempt) {
      const auto observed = signal.epoch();
      auto waiting = boost::asio::co_spawn(runtime.context(), signal.async_wait(observed), boost::asio::use_future);
      auto notifier = std::thread{[&signal] { signal.notify(); }};
      notifier.join();

      BOOST_REQUIRE(waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      BOOST_CHECK_NE(waiting.get(), observed);
   }
}

BOOST_AUTO_TEST_CASE(asio_notification_stop_is_sticky_prompt_and_preserves_caller_executor) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto signal = forge::asio::notification{};
   auto caller = boost::asio::make_strand(runtime.context());

   for (auto attempt = 0; attempt != 32; ++attempt) {
      auto started = std::atomic_bool{false};
      auto stop = std::stop_source{};
      auto waiting = boost::asio::co_spawn(
          caller, wait_for_cancellation(signal, started, stop.get_token()), boost::asio::use_future);

      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
      while (!started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      BOOST_REQUIRE(started.load(std::memory_order_acquire));
      auto stopper = std::thread{[&stop] { static_cast<void>(stop.request_stop()); }};
      stopper.join();
      BOOST_REQUIRE(waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      BOOST_CHECK(waiting.get());
   }

   auto already_stopped = std::stop_source{};
   static_cast<void>(already_stopped.request_stop());
   auto prestarted = std::atomic_bool{false};
   auto sticky = boost::asio::co_spawn(
       caller, wait_for_cancellation(signal, prestarted, already_stopped.get_token()), boost::asio::use_future);
   BOOST_REQUIRE(sticky.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK(sticky.get());

   const auto observed = signal.epoch();
   signal.notify();
   auto successor = boost::asio::co_spawn(caller, signal.async_wait(observed), boost::asio::use_future);
   BOOST_REQUIRE(successor.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_NE(successor.get(), observed);
}
