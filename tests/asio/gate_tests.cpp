#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.blocking;
import forge.asio.gate;
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

std::future<void> emit_cancellation(boost::asio::any_io_executor executor, boost::asio::cancellation_signal& signal) {
   auto completion = std::promise<void>{};
   auto result = completion.get_future();
   // Composed-operation cancellation must not race its intermediate Asio handlers.
   boost::asio::post(executor, [&signal, completion = std::move(completion)]() mutable {
      signal.emit(boost::asio::cancellation_type::all);
      completion.set_value();
   });
   return result;
}

boost::asio::awaitable<void>
acquire_and_record(forge::asio::gate& gate, int value, std::vector<int>& order, std::mutex& mutex) {
   auto ticket = co_await gate.acquire();
   const auto lock = std::scoped_lock{mutex};
   order.push_back(value);
}

boost::asio::awaitable<bool>
acquire_after_precancel(forge::asio::gate& gate, std::atomic_bool& ready) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor, boost::asio::steady_timer::time_point::max()};
   ready.store(true, std::memory_order_release);
   auto error = boost::system::error_code{};
   co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   if (error != boost::asio::error::operation_aborted) {
      throw std::runtime_error{"test pre-cancel wait was not canceled"};
   }
   try {
      auto ticket = co_await gate.acquire();
      co_return false;
   } catch (const forge::asio::exceptions::canceled&) {
      co_return true;
   }
}

} // namespace

BOOST_AUTO_TEST_CASE(asio_gate_releases_move_only_ticket_with_raii) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto gate = forge::asio::gate{};

   auto ticket = forge::asio::blocking::run(runtime, gate.acquire());
   BOOST_CHECK(ticket.active());
   auto moved = std::move(ticket);
   BOOST_CHECK(!ticket.active());
   moved.release();
   BOOST_CHECK(!moved.active());

   {
      auto next = forge::asio::blocking::run(runtime, gate.acquire());
      BOOST_CHECK(next.active());
   }
   auto final = forge::asio::blocking::run(runtime, gate.acquire());
   BOOST_CHECK(final.active());
}

BOOST_AUTO_TEST_CASE(asio_gate_waiters_are_granted_fifo) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto gate = forge::asio::gate{};
   auto owner = forge::asio::blocking::run(runtime, gate.acquire());
   auto order = std::vector<int>{};
   auto mutex = std::mutex{};

   auto enqueue = [&](int value) {
      return boost::asio::co_spawn(
         runtime.context(),
         acquire_and_record(gate, value, order, mutex),
         boost::asio::use_future);
   };

   auto first = enqueue(1);
   std::this_thread::sleep_for(std::chrono::milliseconds{10});
   auto second = enqueue(2);
   std::this_thread::sleep_for(std::chrono::milliseconds{10});
   auto third = enqueue(3);
   std::this_thread::sleep_for(std::chrono::milliseconds{20});
   owner.release();
   first.get();
   second.get();
   third.get();

   const auto expected = std::vector<int>{1, 2, 3};
   BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(asio_gate_cancels_queued_acquire_without_losing_grant) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto serial = boost::asio::make_strand(runtime.context());
   auto gate = forge::asio::gate{};
   auto owner = forge::asio::blocking::run(runtime, gate.acquire());
   auto cancellation = boost::asio::cancellation_signal{};

   auto canceled = boost::asio::co_spawn(
       serial, gate.acquire(), boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));

   std::this_thread::sleep_for(std::chrono::milliseconds{20});
   emit_cancellation(serial, cancellation).get();
   BOOST_CHECK_THROW(static_cast<void>(canceled.get()), forge::asio::exceptions::canceled);
   owner.release();

   auto successor = forge::asio::blocking::run(runtime, gate.acquire());
   BOOST_CHECK(successor.active());
}

BOOST_AUTO_TEST_CASE(asio_gate_rejects_precanceled_acquire_with_typed_error) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto serial = boost::asio::make_strand(runtime.context());
   auto gate = forge::asio::gate{};
   auto ready = std::atomic_bool{false};
   auto cancellation = boost::asio::cancellation_signal{};
   auto result =
       boost::asio::co_spawn(serial, acquire_after_precancel(gate, ready),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));

   BOOST_REQUIRE(wait_until([&] { return ready.load(std::memory_order_acquire); }));
   std::this_thread::sleep_for(std::chrono::milliseconds{10});
   emit_cancellation(serial, cancellation).get();
   BOOST_CHECK(result.get());

   auto ticket = forge::asio::blocking::run(runtime, gate.acquire());
   BOOST_CHECK(ticket.active());
}

BOOST_AUTO_TEST_CASE(asio_gate_release_cancel_race_never_loses_ticket) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto serial = boost::asio::make_strand(runtime.context());
   auto gate = forge::asio::gate{};

   for (auto attempt = 0; attempt != 32; ++attempt) {
      auto owner = forge::asio::blocking::run(runtime, gate.acquire());
      auto cancellation = boost::asio::cancellation_signal{};
      auto waiting = boost::asio::co_spawn(
          serial, gate.acquire(), boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

      auto release = std::thread{[&owner] { owner.release(); }};
      auto emitted = std::future<void>{};
      auto cancel = std::thread{[&] { emitted = emit_cancellation(serial, cancellation); }};
      release.join();
      cancel.join();
      emitted.get();

      try {
         auto ticket = waiting.get();
         ticket.release();
      } catch (const forge::asio::exceptions::canceled&) {
      }

      auto successor = forge::asio::blocking::run(runtime, gate.acquire());
      BOOST_REQUIRE(successor.active());
      successor.release();
   }
}

BOOST_AUTO_TEST_CASE(asio_gate_close_rejects_waiters_and_future_acquires) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto gate = forge::asio::gate{};
   auto owner = forge::asio::blocking::run(runtime, gate.acquire());
   auto waiting = boost::asio::co_spawn(runtime.context(), gate.acquire(), boost::asio::use_future);

   std::this_thread::sleep_for(std::chrono::milliseconds{20});
   gate.close();
   BOOST_CHECK_THROW(static_cast<void>(waiting.get()), forge::asio::exceptions::rejected);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, gate.acquire()), forge::asio::exceptions::rejected);
   owner.release();
}

BOOST_AUTO_TEST_CASE(asio_gate_deferred_acquire_keeps_state_alive) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto deferred = forge::asio::gate{}.acquire();

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, std::move(deferred)), forge::asio::exceptions::rejected);
}

BOOST_AUTO_TEST_CASE(asio_gate_ticket_can_be_released_from_another_thread) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto gate = forge::asio::gate{};
   auto owner = forge::asio::blocking::run(runtime, gate.acquire());
   auto release = std::thread{[ticket = std::move(owner)]() mutable { ticket.release(); }};
   release.join();

   auto successor = forge::asio::blocking::run(runtime, gate.acquire());
   BOOST_CHECK(successor.active());
}
