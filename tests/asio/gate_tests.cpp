#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <memory>
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
#include <boost/scope/scope_exit.hpp>

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

template <typename Operation>
boost::asio::awaitable<bool> check_cancellation_scope(Operation& operation, std::promise<void>& timer_ready,
                                                      bool throw_policy) {
   auto filters = std::make_shared<std::array<unsigned, 2>>();
   co_await boost::asio::this_coro::reset_cancellation_state(
       [filters](boost::asio::cancellation_type_t type) {
          ++(*filters)[0];
          return type & boost::asio::cancellation_type::partial;
       },
       [filters](boost::asio::cancellation_type_t type) {
          ++(*filters)[1];
          return type & boost::asio::cancellation_type::partial;
       });
   co_await boost::asio::this_coro::throw_if_cancelled(throw_policy);
   co_await operation();
   const auto preserved = (co_await boost::asio::this_coro::throw_if_cancelled()) == throw_policy;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   timer.expires_after(std::chrono::milliseconds{500}); // Finite regression control, not a runtime policy.
   timer_ready.set_value();
   auto error = boost::system::error_code{};
   co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   co_return preserved &&
       (co_await boost::asio::this_coro::throw_if_cancelled()) == throw_policy&& error ==
           boost::asio::error::operation_aborted &&
       (*filters)[0] == 2U && (*filters)[1] == 2U &&
       cancellation.cancelled() == boost::asio::cancellation_type::partial;
}

} // namespace

BOOST_AUTO_TEST_CASE(asio_gate_preserves_caller_filters_throw_policy_and_next_timer_on_success_and_rejection) {
   for (const auto throw_policy : {false, true}) {
      for (const auto closed : {false, true}) {
         auto runtime = forge::asio::runtime{{.worker_threads = 1}};
         auto gate = forge::asio::gate{};
         if (closed)
            gate.close();
         auto expected_exit = false;
         auto action = [&]() -> boost::asio::awaitable<void> {
            try {
               auto ticket = co_await gate.acquire();
               expected_exit = !closed && ticket.active();
            } catch (const forge::asio::exceptions::rejected&) {
               if (!closed)
                  throw;
               expected_exit = true;
            }
         };
         auto timer_ready = std::promise<void>{};
         auto ready = timer_ready.get_future();
         auto cancellation = boost::asio::cancellation_signal{};
         auto result =
             boost::asio::co_spawn(runtime.context(), check_cancellation_scope(action, timer_ready, throw_policy),
                                   boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
         if (ready.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
            std::terminate();
         boost::asio::post(runtime.context(), [&] {
            cancellation.emit(boost::asio::cancellation_type::terminal);
            cancellation.emit(boost::asio::cancellation_type::partial);
         });
         if (result.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
            std::terminate();
         BOOST_CHECK(result.get());
         BOOST_CHECK(expected_exit);
      }
   }
}

BOOST_AUTO_TEST_CASE(asio_gate_queued_and_granted_cancellation_preserve_history_and_next_timer) {
   for (const auto grant_before_cancel : {false, true}) {
      auto runtime = forge::asio::runtime{{.worker_threads = 1}};
      auto gate = forge::asio::gate{};
      auto owner = forge::asio::blocking::run(runtime, gate.acquire());
      auto entered = std::promise<void>{};
      auto entering = entered.get_future();
      auto timer_ready = std::promise<void>{};
      auto ready = timer_ready.get_future();
      auto cancellation = boost::asio::cancellation_signal{};
      auto result = std::future<bool>{};
      auto canceled = false;
      auto history_preserved = false;
      auto action = [&]() -> boost::asio::awaitable<void> {
         // On this single worker, this posted marker runs only after the
         // nested acquire has suspended with the owner's ticket still held.
         boost::asio::post(runtime.context(), [&] { entered.set_value(); });
         try {
            auto ticket = co_await gate.acquire();
         } catch (const forge::asio::exceptions::canceled&) {
            canceled = true;
         }
         const auto state = co_await boost::asio::this_coro::cancellation_state;
         history_preserved = state.cancelled() == boost::asio::cancellation_type::partial &&
                             !(co_await boost::asio::this_coro::throw_if_cancelled());
      };
      const auto cleanup = boost::scope::scope_exit{[&] {
         owner.release();
         gate.close();
         if (result.valid()) {
            if (result.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
               std::terminate();
            try {
               static_cast<void>(result.get());
            } catch (...) {
            }
         }
      }};
      result = boost::asio::co_spawn(runtime.context(), check_cancellation_scope(action, timer_ready, false),
                                     boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
      if (entering.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
         std::terminate();
      boost::asio::post(runtime.context(), [&] {
         if (grant_before_cancel)
            owner.release(); // Grant wake is posted; cancel wins before the waiting child resumes.
         cancellation.emit(boost::asio::cancellation_type::partial);
      });
      if (ready.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
         std::terminate();
      boost::asio::post(runtime.context(), [&] { cancellation.emit(boost::asio::cancellation_type::partial); });
      if (result.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
         std::terminate();
      BOOST_CHECK(result.get());
      BOOST_CHECK(canceled);
      BOOST_CHECK(history_preserved);
      owner.release();
      auto successor = forge::asio::blocking::run(runtime, gate.acquire());
      BOOST_CHECK(successor.active());
   }
}

BOOST_AUTO_TEST_CASE(asio_gate_completed_ticket_wins_and_next_timer_receives_cancellation) {
   for (const auto throw_policy : {false, true}) {
      auto runtime = forge::asio::runtime{{.worker_threads = 1}};
      auto gate = forge::asio::gate{};
      auto ticket = std::optional<forge::asio::gate::ticket>{};
      auto timer_ready = std::promise<void>{};
      auto ready = timer_ready.get_future();
      auto cancellation = boost::asio::cancellation_signal{};
      auto result = std::future<bool>{};
      auto action = [&]() -> boost::asio::awaitable<void> { ticket.emplace(co_await gate.acquire()); };
      const auto cleanup = boost::scope::scope_exit{[&] {
         gate.close();
         if (result.valid()) {
            if (result.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
               std::terminate();
            try {
               static_cast<void>(result.get());
            } catch (...) {
            }
         }
         ticket.reset();
      }};
      result = boost::asio::co_spawn(runtime.context(), check_cancellation_scope(action, timer_ready, throw_policy),
                                     boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
      if (ready.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
         std::terminate();
      boost::asio::post(runtime.context(), [&] {
         cancellation.emit(boost::asio::cancellation_type::terminal);
         cancellation.emit(boost::asio::cancellation_type::partial);
      });
      if (result.wait_for(std::chrono::seconds{2}) != std::future_status::ready)
         std::terminate();
      BOOST_CHECK(result.get());
      BOOST_REQUIRE(ticket.has_value());
      BOOST_CHECK(ticket->active()); // Cancellation after publication does not revoke its ticket.
      ticket.reset();
      auto successor = forge::asio::blocking::run(runtime, gate.acquire());
      BOOST_CHECK(successor.active());
   }
}

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
