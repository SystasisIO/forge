module;

#include <boost/test/unit_test.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.gate;
import forge.asio.notification;
import forge.asio.runtime;
import forge.crypto.asymmetric;
import forge.exceptions;
import forge.net.p2p.exceptions;
import forge.net.transport.session;

#include "../../libraries/net/p2p/details/direct_transport.hxx"
#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/libp2p_identity_material.hxx"
#include "../../libraries/net/p2p/details/operation_deadline.hxx"
#include "../../libraries/net/p2p/details/session_lifecycle.hxx"
#include "../../libraries/net/p2p/details/session_retirement.hxx"
#include "../../libraries/net/p2p/details/session_teardown.hxx"
#include "gossipsub_test_shutdown.hxx"

namespace forge::net::p2p {
namespace {

bool wait_for_count(const std::atomic_size_t& value, std::size_t expected,
                    std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (value.load(std::memory_order_acquire) != expected && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   return value.load(std::memory_order_acquire) == expected;
}

boost::asio::awaitable<bool> wait_for_terminal_cleanup(
    detail::session_teardown* teardown, const std::atomic_bool* ownership_released) {
   co_await teardown->wait();
   co_return ownership_released->load(std::memory_order_acquire);
}

class throwing_cancel_session final : public forge::net::transport::detail::session_concept {
 public:
   [[nodiscard]] bool valid() const noexcept override {
      return open_.load(std::memory_order_acquire);
   }

   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<void> async_close() override {
      close_calls_.fetch_add(1, std::memory_order_release);
      open_.store(false, std::memory_order_release);
      co_return;
   }

   void cancel() override {
      cancel_calls_.fetch_add(1, std::memory_order_release);
      throw std::runtime_error{"expected throwing session cancel"};
   }

   [[nodiscard]] std::size_t cancel_calls() const noexcept {
      return cancel_calls_.load(std::memory_order_acquire);
   }

   [[nodiscard]] std::size_t close_calls() const noexcept {
      return close_calls_.load(std::memory_order_acquire);
   }

 private:
   std::atomic_bool open_{true};
   std::atomic_size_t cancel_calls_{0};
   std::atomic_size_t close_calls_{0};
};

class terminal_throwing_session final : public forge::net::transport::detail::session_concept {
 public:
   explicit terminal_throwing_session(std::shared_ptr<void> native_lifetime)
       : native_lifetime_(std::move(native_lifetime)) {}

   [[nodiscard]] bool valid() const noexcept override {
      return open_.load(std::memory_order_acquire);
   }

   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<void> async_close() override {
      close_calls_.fetch_add(1, std::memory_order_release);
      open_.store(false, std::memory_order_release);
      native_lifetime_.reset();
      throw std::runtime_error{"injected terminal close failure"};
      co_return;
   }

   void cancel() override {
      cancel_calls_.fetch_add(1, std::memory_order_release);
      open_.store(false, std::memory_order_release);
   }

   [[nodiscard]] std::size_t cancel_calls() const noexcept {
      return cancel_calls_.load(std::memory_order_acquire);
   }

   [[nodiscard]] std::size_t close_calls() const noexcept {
      return close_calls_.load(std::memory_order_acquire);
   }

 private:
   std::shared_ptr<void> native_lifetime_;
   std::atomic_bool open_{true};
   std::atomic_size_t cancel_calls_{0};
   std::atomic_size_t close_calls_{0};
};

struct terminal_barrier_state {
   std::atomic_bool close_entered = false;
   std::atomic_bool release_close = false;
   std::atomic_size_t cancel_calls{0};
   forge::asio::notification changed;
};

class terminal_barrier_session final : public forge::net::transport::detail::session_concept {
 public:
   terminal_barrier_session(std::shared_ptr<terminal_barrier_state> state, std::shared_ptr<void> native_lifetime)
       : state_(std::move(state)), native_lifetime_(std::move(native_lifetime)) {}

   [[nodiscard]] bool valid() const noexcept override {
      return open_.load(std::memory_order_acquire);
   }

   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<void> async_close() override {
      state_->close_entered.store(true, std::memory_order_release);
      state_->changed.notify();
      while (!state_->release_close.load(std::memory_order_acquire)) {
         const auto observed = state_->changed.epoch();
         if (!state_->release_close.load(std::memory_order_acquire)) {
            (void)co_await state_->changed.async_wait(observed);
         }
      }
      open_.store(false, std::memory_order_release);
      native_lifetime_.reset();
   }

   void cancel() override {
      state_->cancel_calls.fetch_add(1, std::memory_order_release);
   }

 private:
   std::shared_ptr<terminal_barrier_state> state_;
   std::shared_ptr<void> native_lifetime_;
   std::atomic_bool open_{true};
};

BOOST_AUTO_TEST_CASE(p2p_session_rejection_stages_transport_cancel_after_owner_unlock) {
   auto owner_mutex = std::mutex{};
   auto cancel_calls = std::atomic_size_t{0};
   auto canceled_after_unlock = std::atomic_bool{false};
   auto closed = false;
   const auto cancel = [&] {
      cancel_calls.fetch_add(1, std::memory_order_release);
      if (owner_mutex.try_lock()) {
         canceled_after_unlock.store(true, std::memory_order_release);
         owner_mutex.unlock();
      }
   };

   {
      auto lock = std::scoped_lock{owner_mutex};
      detail::mark_rejected_session(closed);
      BOOST_TEST(closed);
      BOOST_TEST(cancel_calls.load(std::memory_order_acquire) == 0U);
   }
   cancel();

   BOOST_TEST(cancel_calls.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(canceled_after_unlock.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_cancellation_latch_preserves_stop_before_arm_and_terminal_finish) {
   auto latch = cancellation_latch{};
   auto canceled = std::atomic_size_t{0};

   latch.request_stop();
   latch.arm([&] { canceled.fetch_add(1, std::memory_order_release); });
   latch.request_stop();
   latch.request_stop();
   latch.request_stop();
   BOOST_TEST(!latch.finish());
   latch.arm([&] { canceled.fetch_add(1, std::memory_order_release); });

   BOOST_TEST(canceled.load(std::memory_order_acquire) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_cancellation_latch_stop_before_throwing_arm_propagates_after_callback_accounting) {
   auto latch = cancellation_latch{};
   auto invoked = std::atomic_size_t{0};

   latch.request_stop();
   BOOST_CHECK_THROW(latch.arm([&] {
      invoked.fetch_add(1, std::memory_order_release);
      throw std::runtime_error{"injected cancellation failure"};
   }),
                     std::runtime_error);

   BOOST_TEST(invoked.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(!latch.finish());
   latch.clear();
   BOOST_TEST(invoked.load(std::memory_order_acquire) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_cancellation_latch_races_arm_and_stop_with_one_cancel) {
   for (auto iteration = 0U; iteration < 64U; ++iteration) {
      auto latch = cancellation_latch{};
      auto canceled = std::atomic_size_t{0};
      auto start = std::barrier{2};
      auto stop = std::thread{[&] {
         start.arrive_and_wait();
         latch.request_stop();
      }};

      start.arrive_and_wait();
      latch.arm([&] { canceled.fetch_add(1, std::memory_order_release); });
      stop.join();
      latch.request_stop();
      BOOST_TEST(!latch.finish());

      BOOST_TEST(canceled.load(std::memory_order_acquire) == 1U);
   }
}

BOOST_AUTO_TEST_CASE(p2p_cancellation_latch_finish_waits_for_in_flight_cancel) {
   auto latch = cancellation_latch{};
   auto callback_entered = std::atomic_bool{false};
   auto release_callback = std::atomic_bool{false};
   auto finish_returned = std::atomic_bool{false};
   auto finish_result = std::atomic_bool{true};
   latch.arm([&] {
      callback_entered.store(true, std::memory_order_release);
      while (!release_callback.load(std::memory_order_acquire)) {
         std::this_thread::yield();
      }
   });

   auto stop = std::thread{[&] { latch.request_stop(); }};
   const auto callback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!callback_entered.load(std::memory_order_acquire)) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < callback_deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   auto finish = std::thread{[&] {
      finish_result.store(latch.finish(), std::memory_order_release);
      finish_returned.store(true, std::memory_order_release);
   }};
   std::this_thread::sleep_for(std::chrono::milliseconds{5});
   BOOST_TEST(!finish_returned.load(std::memory_order_acquire));

   release_callback.store(true, std::memory_order_release);
   stop.join();
   finish.join();
   BOOST_TEST(!finish_result.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_cancellation_latch_has_one_finish_stop_terminal_winner) {
   for (auto iteration = 0U; iteration < 64U; ++iteration) {
      auto latch = cancellation_latch{};
      auto canceled = std::atomic_size_t{0};
      auto start = std::barrier{2};
      latch.arm([&] { canceled.fetch_add(1, std::memory_order_release); });
      auto stop = std::thread{[&] {
         start.arrive_and_wait();
         latch.request_stop();
      }};

      start.arrive_and_wait();
      const auto completed = latch.finish();
      stop.join();
      latch.request_stop();

      BOOST_TEST(canceled.load(std::memory_order_acquire) == (completed ? 0U : 1U));
   }
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_finish_waits_for_in_flight_cancel) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto deadline = operation_deadline{runtime.context(), std::chrono::seconds{5}};
   auto state_mutex = std::mutex{};
   auto state_changed = std::condition_variable{};
   auto callback_entered = false;
   auto release_callback = false;
   auto finish_attempting = false;
   auto finish_returned = false;
   auto finish_result = std::atomic_bool{false};
   auto stop_result = std::atomic_bool{false};
   deadline.arm([&] {
      auto lock = std::unique_lock{state_mutex};
      callback_entered = true;
      state_changed.notify_all();
      state_changed.wait(lock, [&] { return release_callback; });
   });

   auto stop = std::thread{[token = deadline.stopping(), &stop_result] {
      stop_result.store(token.request_stop(), std::memory_order_release);
   }};
   {
      auto lock = std::unique_lock{state_mutex};
      BOOST_REQUIRE(state_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return callback_entered; }));
   }
   auto finish = std::thread{[&] {
      {
         auto lock = std::scoped_lock{state_mutex};
         finish_attempting = true;
      }
      state_changed.notify_all();
      finish_result.store(deadline.finish(), std::memory_order_release);
      {
         auto lock = std::scoped_lock{state_mutex};
         finish_returned = true;
      }
      state_changed.notify_all();
   }};

   {
      auto lock = std::unique_lock{state_mutex};
      BOOST_REQUIRE(state_changed.wait_for(lock, std::chrono::seconds{2}, [&] { return finish_attempting; }));
      const auto returned_while_callback_active =
          state_changed.wait_for(lock, std::chrono::milliseconds{100}, [&] { return finish_returned; });
      BOOST_TEST(!returned_while_callback_active);
      release_callback = true;
   }
   state_changed.notify_all();
   stop.join();
   finish.join();
   BOOST_TEST(stop_result.load(std::memory_order_acquire));
   BOOST_TEST(finish_returned);
   BOOST_TEST(finish_result.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_finish_seals_concurrent_late_arm) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};

   for (auto iteration = 0U; iteration < 64U; ++iteration) {
      auto deadline = operation_deadline{runtime.context(), std::chrono::seconds{5}};
      BOOST_REQUIRE(deadline.stopping().request_stop());
      auto callbacks = std::atomic_size_t{0};
      auto start = std::barrier{3};
      auto finish_result = std::atomic_bool{false};
      auto finish = std::thread{[&] {
         start.arrive_and_wait();
         finish_result.store(deadline.finish(), std::memory_order_release);
      }};
      auto arm = std::thread{[&] {
         start.arrive_and_wait();
         deadline.arm([&] { callbacks.fetch_add(1, std::memory_order_release); });
      }};
      start.arrive_and_wait();
      finish.join();
      arm.join();

      BOOST_TEST(finish_result.load(std::memory_order_acquire));
      const auto sealed_count = callbacks.load(std::memory_order_acquire);
      BOOST_TEST(sealed_count <= 1U);
      deadline.arm([&] { callbacks.fetch_add(1, std::memory_order_release); });
      BOOST_TEST(callbacks.load(std::memory_order_acquire) == sealed_count);
   }
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_cancel_callback_may_reenter_finish) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto deadline = operation_deadline{runtime.context(), std::chrono::seconds{5}};
   auto callback_finished = std::atomic_bool{false};
   auto finish_result = std::atomic_bool{false};
   auto stop_result = std::atomic_bool{false};
   deadline.arm([&] {
      finish_result.store(deadline.finish(), std::memory_order_release);
      callback_finished.store(true, std::memory_order_release);
   });

   auto stop = std::thread{[token = deadline.stopping(), &stop_result] {
      stop_result.store(token.request_stop(), std::memory_order_release);
   }};
   stop.join();
   BOOST_TEST(stop_result.load(std::memory_order_acquire));
   BOOST_TEST(callback_finished.load(std::memory_order_acquire));
   BOOST_TEST(finish_result.load(std::memory_order_acquire));
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_timeout_before_arm_invokes_late_callback_once) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto deadline = operation_deadline{runtime.context(), std::chrono::milliseconds{10}};
   const auto timeout_limit = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!deadline.timed_out()) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < timeout_limit);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }

   auto callbacks = std::atomic_size_t{0};
   deadline.arm([&] { callbacks.fetch_add(1, std::memory_order_release); });
   deadline.arm([&] { callbacks.fetch_add(1, std::memory_order_release); });
   BOOST_TEST(callbacks.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(!deadline.finish());
}

BOOST_AUTO_TEST_CASE(p2p_session_teardown_waits_for_started_transport_cleanup) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto release =
       std::make_shared<boost::asio::steady_timer>(runtime.context(), boost::asio::steady_timer::time_point::max());
   auto close_started = std::atomic_size_t{0};
   auto cancel_called = std::atomic_size_t{0};
   auto teardown = detail::session_teardown{runtime.context().get_executor()};

   auto operations = std::vector<detail::session_teardown::operation>{};
   for (auto remaining = 2U; remaining != 0U; --remaining) {
      operations.push_back(detail::session_teardown::operation{
          .close = [release, &close_started]() -> boost::asio::awaitable<void> {
             close_started.fetch_add(1, std::memory_order_release);
             auto error = boost::system::error_code{};
             co_await release->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
          },
          .cancel = [&cancel_called] { cancel_called.fetch_add(1, std::memory_order_release); },
      });
   }
   teardown.start(std::move(operations));

   const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (close_started.load(std::memory_order_acquire) != 2U) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < close_deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }

   auto cancellation = boost::asio::cancellation_signal{};
   auto stopped =
       boost::asio::co_spawn(runtime.context(), teardown.wait(),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   const auto waiting_for_cleanup = stopped.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout;
   BOOST_TEST(waiting_for_cleanup);

   cancellation.emit(boost::asio::cancellation_type::total);
   const auto cancellation_did_not_bypass_cleanup =
       stopped.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout;
   BOOST_TEST(cancellation_did_not_bypass_cleanup);

   boost::asio::post(runtime.context(), [release] { release->cancel(); });
   const auto cleanup_completed = stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
   BOOST_REQUIRE(cleanup_completed);
   stopped.get();
   BOOST_TEST(cancel_called.load(std::memory_order_acquire) == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_session_teardown_waits_for_tracked_background_operation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   auto tracked = teardown.track();
   BOOST_REQUIRE(tracked.active());

   teardown.start({});
   auto stopped = boost::asio::co_spawn(runtime.context(), teardown.wait(), boost::asio::use_future);
   const auto waits_for_background_operation =
       stopped.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout;
   BOOST_TEST(waits_for_background_operation);

   tracked.release();
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   stopped.get();
}

BOOST_AUTO_TEST_CASE(p2p_session_teardown_cancels_tracked_background_operation) {
   for (auto order = 0U; order != 2U; ++order) {
      const auto stop_before_run = order == 1U;
      BOOST_TEST_CONTEXT("stop_before_coroutine_run=" << stop_before_run) {
         auto context = boost::asio::io_context{};
         auto teardown = detail::session_teardown{context.get_executor()};
         auto blocked = std::make_shared<forge::asio::gate>();
         auto release_tracking = std::make_shared<forge::asio::notification>();
         auto cancel_called = std::size_t{};
         auto operation_started = false;
         auto cancel_observed = false;
         auto tracking_released = false;
         auto held = forge::asio::gate::ticket{};
         auto acquired = std::future<forge::asio::gate::ticket>{};
         auto operation = std::future<void>{};
         auto stopped = std::future<void>{};
         const auto drain = [&]() noexcept {
            teardown.start({});
            blocked->close();
            release_tracking->notify();
            try {
               if (context.stopped()) { context.restart(); }
               context.run_for(std::chrono::seconds{2});
            } catch (...) {
               forge::tests::p2p::gossipsub_test_shutdown::fail_closed();
            }
            if ((acquired.valid() && acquired.wait_for(std::chrono::seconds{0}) != std::future_status::ready) ||
                (operation.valid() && operation.wait_for(std::chrono::seconds{0}) != std::future_status::ready) ||
                (stopped.valid() && stopped.wait_for(std::chrono::seconds{0}) != std::future_status::ready)) {
               forge::tests::p2p::gossipsub_test_shutdown::fail_closed();
            }
         };
         // Even a failed assertion must settle coroutines before destroying their captured locals.
         auto terminal_guard = boost::scope::scope_exit{drain};

         acquired = boost::asio::co_spawn(context, blocked->acquire(), boost::asio::use_future);
         context.poll();
         BOOST_REQUIRE(acquired.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
         held = acquired.get();
         BOOST_REQUIRE(held.active());
         context.restart();
         auto tracked = teardown.track([blocked, &cancel_called] {
            ++cancel_called;
            blocked->close();
         });
         BOOST_REQUIRE(tracked.active());
         operation = boost::asio::co_spawn(
             context,
             [blocked, release_tracking, &operation_started, &cancel_observed, &tracking_released,
              tracked = std::move(tracked)]() mutable -> boost::asio::awaitable<void> {
                operation_started = true;
                try {
                   auto ticket = co_await blocked->acquire();
                } catch (const forge::asio::exceptions::rejected&) {
                   cancel_observed = true;
                }
                co_await release_tracking->async_wait(0);
                tracked.release();
                tracking_released = true;
             },
             boost::asio::use_future);
         if (!stop_before_run) {
            // Draining ready handlers with the first gate ticket held leaves acquire actually queued.
            context.poll();
         }
         const auto started_before_stop = operation_started;
         const auto canceled_before_stop = cancel_observed;
         const auto pending_before_stop = operation.wait_for(std::chrono::seconds{0}) == std::future_status::timeout;
         teardown.start({});
         const auto started_when_stop_returned = operation_started;
         stopped = boost::asio::co_spawn(context, teardown.wait(), boost::asio::use_future);
         context.poll();
         const auto cancellation_reached_operation = cancel_observed;
         const auto operation_pending_until_release =
             operation.wait_for(std::chrono::seconds{0}) == std::future_status::timeout;
         const auto teardown_pending_until_release =
             stopped.wait_for(std::chrono::seconds{0}) == std::future_status::timeout;
         const auto released_before_barrier = tracking_released;
         const auto first_cancel_count = cancel_called;
         teardown.start({});
         const auto repeated_cancel_count = cancel_called;

         drain();
         BOOST_CHECK_NO_THROW(operation.get());
         BOOST_CHECK_NO_THROW(stopped.get());
         BOOST_TEST(started_before_stop == !stop_before_run);
         BOOST_TEST(started_when_stop_returned == started_before_stop);
         BOOST_TEST(!canceled_before_stop);
         BOOST_TEST(pending_before_stop);
         BOOST_TEST(cancellation_reached_operation);
         BOOST_TEST(operation_pending_until_release);
         BOOST_TEST(teardown_pending_until_release);
         BOOST_TEST(!released_before_barrier);
         BOOST_TEST(tracking_released);
         BOOST_TEST(blocked->closed());
         BOOST_TEST(first_cancel_count == 1U);
         BOOST_TEST(repeated_cancel_count == 1U);
         BOOST_TEST(cancel_called == 1U);
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_noexcept_cancel_request_preserves_session_until_graceful_teardown) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<throwing_cancel_session>();
   auto candidate = forge::net::transport::detail::session_access::make(model);
   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   BOOST_CHECK_NO_THROW(detail::request_session_cancel(candidate));
   BOOST_TEST(model->cancel_calls() == 1U);
   BOOST_TEST(candidate.valid());

   auto ticket = teardown.track([&candidate] { detail::request_session_cancel(candidate); });
   BOOST_REQUIRE(ticket.active());

   teardown.start({});
   BOOST_TEST(model->cancel_calls() == 2U);
   BOOST_TEST(candidate.valid());

   auto stopped = boost::asio::co_spawn(runtime.context(), teardown.wait(), boost::asio::use_future);
   BOOST_CHECK(stopped.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout);

   forge::asio::blocking::run(runtime, candidate.async_close());
   BOOST_TEST(model->close_calls() == 1U);
   BOOST_TEST(!candidate.valid());
   ticket.release();

   const auto stopped_ready = stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
   BOOST_CHECK(stopped_ready);
   if (stopped_ready) {
      stopped.get();
   }
}

BOOST_AUTO_TEST_CASE(p2p_session_teardown_handles_concurrent_track_release_and_start) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (auto iteration = 0U; iteration < 1'000U; ++iteration) {
      auto teardown = detail::session_teardown{runtime.context().get_executor()};
      auto tracked = teardown.track();
      auto start = std::barrier{2};
      auto releaser = std::thread{[&] {
         start.arrive_and_wait();
         tracked.release();
      }};
      start.arrive_and_wait();
      teardown.start({});
      releaser.join();
      forge::asio::blocking::run(runtime, teardown.wait());
   }
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_preserves_shutdown_winner_past_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto deadline = operation_deadline{runtime.context(), std::chrono::milliseconds{10}};
   auto canceled = std::atomic_size_t{0};
   auto stopping = deadline.stopping();

   BOOST_REQUIRE(stopping.request_stop());
   deadline.arm([&] { canceled.fetch_add(1, std::memory_order_release); });
   std::this_thread::sleep_for(std::chrono::milliseconds{50});

   BOOST_TEST(deadline.stopped());
   BOOST_TEST(!deadline.timed_out());
   BOOST_TEST(canceled.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(deadline.finish());
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_shutdown_cancels_armed_operation_once) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto deadline = operation_deadline{runtime.context(), std::chrono::seconds{1}};
   auto canceled = std::atomic_size_t{0};
   auto stopping = deadline.stopping();
   deadline.arm([&] { canceled.fetch_add(1, std::memory_order_release); });

   BOOST_REQUIRE(stopping.request_stop());
   BOOST_TEST(!stopping.request_stop());
   BOOST_TEST(canceled.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(deadline.stopped());
   BOOST_TEST(!deadline.timed_out());
   BOOST_TEST(deadline.finish());
   BOOST_TEST(canceled.load(std::memory_order_acquire) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_races_arm_and_stop_without_losing_cancel) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (auto iteration = 0U; iteration < 128U; ++iteration) {
      auto deadline = operation_deadline{runtime.context(), std::chrono::seconds{1}};
      auto canceled = std::atomic_size_t{0};
      auto stopping = deadline.stopping();
      auto start = std::barrier{3};
      auto stop_won = std::atomic_bool{false};
      auto arm = std::thread{[&] {
         start.arrive_and_wait();
         deadline.arm([&] { canceled.fetch_add(1, std::memory_order_release); });
      }};
      auto stop = std::thread{[&] {
         start.arrive_and_wait();
         stop_won.store(stopping.request_stop(), std::memory_order_release);
      }};

      start.arrive_and_wait();
      arm.join();
      stop.join();

      BOOST_TEST(stop_won.load(std::memory_order_acquire));
      BOOST_TEST(deadline.stopped());
      BOOST_TEST(!deadline.timed_out());
      BOOST_TEST(canceled.load(std::memory_order_acquire) == 1U);
      BOOST_TEST(deadline.finish());
   }
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_races_finish_and_stop_with_one_terminal_winner) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (auto iteration = 0U; iteration < 128U; ++iteration) {
      auto deadline = operation_deadline{runtime.context(), std::chrono::seconds{1}};
      auto canceled = std::atomic_size_t{0};
      auto stopping = deadline.stopping();
      deadline.arm([&] { canceled.fetch_add(1, std::memory_order_release); });
      auto start = std::barrier{3};
      auto stop_won = std::atomic_bool{false};
      auto finish_result = std::atomic_bool{false};
      auto finish = std::thread{[&] {
         start.arrive_and_wait();
         finish_result.store(deadline.finish(), std::memory_order_release);
      }};
      auto stop = std::thread{[&] {
         start.arrive_and_wait();
         stop_won.store(stopping.request_stop(), std::memory_order_release);
      }};

      start.arrive_and_wait();
      finish.join();
      stop.join();

      const auto stopped = deadline.stopped();
      BOOST_TEST(finish_result.load(std::memory_order_acquire));
      BOOST_TEST(stop_won.load(std::memory_order_acquire) == stopped);
      BOOST_TEST(!deadline.timed_out());
      BOOST_TEST(canceled.load(std::memory_order_acquire) == (stopped ? 1U : 0U));
   }
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_races_timeout_and_stop_with_one_cancel) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (auto iteration = 0U; iteration < 64U; ++iteration) {
      auto deadline = operation_deadline{runtime.context(), std::chrono::milliseconds{1}};
      auto canceled = std::atomic_size_t{0};
      auto stopping = deadline.stopping();
      deadline.arm([&] { canceled.fetch_add(1, std::memory_order_release); });
      auto start = std::barrier{2};
      auto stop_won = std::atomic_bool{false};
      auto stop = std::thread{[&] {
         start.arrive_and_wait();
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
         stop_won.store(stopping.request_stop(), std::memory_order_release);
      }};

      start.arrive_and_wait();
      stop.join();
      BOOST_REQUIRE(wait_for_count(canceled, 1U));

      const auto stopped = deadline.stopped();
      const auto timed_out = deadline.timed_out();
      BOOST_TEST(stopped != timed_out);
      BOOST_TEST(stop_won.load(std::memory_order_acquire) == stopped);
      BOOST_TEST(canceled.load(std::memory_order_acquire) == 1U);
      BOOST_TEST(deadline.finish() == !timed_out);
   }
}

BOOST_AUTO_TEST_CASE(p2p_operation_deadline_preserves_timeout_winner_after_shutdown) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto deadline = operation_deadline{runtime.context(), std::chrono::milliseconds{10}};
   auto canceled = std::atomic_bool{false};
   auto stopping = deadline.stopping();
   deadline.arm([&] { canceled.store(true, std::memory_order_release); });

   const auto timeout_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!canceled.load(std::memory_order_acquire)) {
      BOOST_REQUIRE(std::chrono::steady_clock::now() < timeout_deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }

   BOOST_TEST(deadline.timed_out());
   BOOST_TEST(!deadline.stopped());
   BOOST_TEST(!stopping.request_stop());
   BOOST_TEST(!deadline.finish());
}

BOOST_AUTO_TEST_CASE(p2p_direct_transport_teardown_continues_after_profile_failure) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto options = node::options{};
   const auto identity = make_libp2p_identity_material(options);
   auto registry = direct::registry{runtime, options, identity, resource_manager{options.limits.resources}};
   auto failed_stop = std::atomic_size_t{0};
   auto next_stop = std::atomic_size_t{0};
   auto failed_async_stop = std::atomic_size_t{0};
   auto next_async_stop = std::atomic_size_t{0};

   const auto add_profile = [&](auto stop, auto async_stop) {
      registry.add(direct::profile{
          .supports = [](const endpoint&) { return false; },
          .listening = [] { return false; },
          .local_endpoints = [] { return std::vector<endpoint>{}; },
          .listen = [](endpoint value) { return value; },
          .stop = std::move(stop),
          .async_stop = std::move(async_stop),
          .async_connect = [](endpoint, const node::connect_options&, std::shared_ptr<cancellation_latch>,
                              std::shared_ptr<void>, direct::authenticated_admission_handler,
                              direct::tcp_transport_progress_handler)
              -> boost::asio::awaitable<direct::connection> {
             co_return direct::connection{};
          },
          .async_accept = [](endpoint) -> boost::asio::awaitable<direct::connection> {
             co_return direct::connection{};
          },
      });
   };

   add_profile(
       [&] {
          failed_stop.fetch_add(1, std::memory_order_release);
          throw std::runtime_error{"expected stop failure"};
       },
       [&]() -> boost::asio::awaitable<void> {
          failed_async_stop.fetch_add(1, std::memory_order_release);
          throw std::runtime_error{"expected async stop failure"};
          co_return;
       });
   add_profile([&] { next_stop.fetch_add(1, std::memory_order_release); },
               [&]() -> boost::asio::awaitable<void> {
                  next_async_stop.fetch_add(1, std::memory_order_release);
                  co_return;
               });

   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   auto operations = std::vector<detail::session_teardown::operation>{};
   operations.push_back(registry.teardown_operation());
   registry.stop();
   teardown.start(std::move(operations));
   forge::asio::blocking::run(runtime, teardown.wait());

   BOOST_TEST(failed_stop.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(next_stop.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(failed_async_stop.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(next_async_stop.load(std::memory_order_acquire) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_direct_listener_stop_wins_after_accept_begins) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto options = node::options{};
   const auto identity = make_libp2p_identity_material(options);
   auto registry = direct::registry{runtime, options, identity, resource_manager{options.limits.resources}};
   const auto requested = endpoint{.transport = {
                                       .host_type = endpoint::host_kind::ip4,
                                       .protocol = endpoint::protocol_kind::tcp,
                                       .host = "127.0.0.1",
                                       .port = 0,
                                   }};
   const auto local = registry.listen(requested);
   auto accepting = boost::asio::co_spawn(runtime.context(), registry.async_accept(local), boost::asio::use_future);
   auto accept_started = std::promise<void>{};
   auto accept_started_future = accept_started.get_future();
   boost::asio::post(runtime.context(), [&accept_started] { accept_started.set_value(); });
   const auto accept_entered = accept_started_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;

   auto blocker_entered = std::promise<void>{};
   auto blocker_entered_future = blocker_entered.get_future();
   auto release_blocker = std::promise<void>{};
   auto blocker_released = release_blocker.get_future().share();
   boost::asio::post(runtime.context(), [&blocker_entered, blocker_released] {
      blocker_entered.set_value();
      blocker_released.wait();
   });
   const auto runtime_blocked = blocker_entered_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready;

   auto client = boost::asio::ip::tcp::socket{runtime.context()};
   auto connect_error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(local.transport.host, connect_error);
   if (!connect_error) {
      client.connect(boost::asio::ip::tcp::endpoint{address, local.transport.port}, connect_error);
   }

   auto start = std::barrier{2};
   auto invalid_snapshot = std::atomic_bool{false};
   auto observer = std::thread{[&] {
      start.arrive_and_wait();
      for (auto remaining = 256U; remaining != 0U; --remaining) {
         const auto endpoints = registry.local_endpoints();
         if (endpoints.size() > 1U) {
            invalid_snapshot.store(true, std::memory_order_release);
         }
         static_cast<void>(registry.listening());
      }
   }};

   start.arrive_and_wait();
   registry.stop();
   release_blocker.set_value();
   observer.join();

   const auto accept_ready = accepting.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
   auto accept_closed = false;
   if (accept_ready) {
      try {
         auto unexpected = accepting.get();
         unexpected.session.cancel();
      } catch (const forge::exceptions::base& error) {
         const auto code = exceptions::code_of(error);
         accept_closed = code && *code == exceptions::code::closed;
      }
   }
   auto close_error = boost::system::error_code{};
   client.close(close_error);

   BOOST_TEST(accept_entered);
   BOOST_TEST(runtime_blocked);
   BOOST_TEST(!connect_error);
   BOOST_TEST(!invalid_snapshot.load(std::memory_order_acquire));
   BOOST_TEST(!registry.listening());
   BOOST_TEST(registry.local_endpoints().empty());
   BOOST_TEST(accept_ready);
   BOOST_TEST(accept_closed);

   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   auto operations = std::vector<detail::session_teardown::operation>{};
   operations.push_back(registry.teardown_operation());
   teardown.start(std::move(operations));
   forge::asio::blocking::run(runtime, teardown.wait());
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_transfers_active_map_node_without_replacement) {
   struct retained_session {
      detail::session_retirement retirement;
   };

   auto active_sessions = std::map<std::uint64_t, std::shared_ptr<retained_session>>{};
   auto retiring_sessions = std::map<std::uint64_t, std::shared_ptr<retained_session>>{};
   const auto session = std::make_shared<retained_session>();
   active_sessions.emplace(17, session);

   auto node = active_sessions.extract(17);
   const auto transferred = retiring_sessions.insert(std::move(node));

   BOOST_TEST(active_sessions.empty());
   BOOST_TEST(transferred.inserted);
   BOOST_TEST(transferred.position->second == session);
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_releases_terminal_tracking_once) {
   static_assert(noexcept(std::declval<detail::session_retirement&>().begin_close(false)));
   static_assert(noexcept(std::declval<detail::session_retirement&>().complete_terminal(
       std::declval<detail::session_teardown::ticket&>())));
   static_assert(noexcept(std::declval<detail::session_retirement&>().quarantine()));

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   auto retirement = detail::session_retirement{};

   BOOST_REQUIRE(retirement.track(teardown.track()));
   BOOST_TEST(!retirement.track(teardown.track()));
   BOOST_TEST(retirement.tracked());
   BOOST_TEST(static_cast<int>(retirement.begin_close(false)) ==
              static_cast<int>(detail::session_retirement::close_start::started));
   BOOST_TEST(static_cast<int>(retirement.begin_close(false)) ==
              static_cast<int>(detail::session_retirement::close_start::in_flight));
   auto terminal_ticket = detail::session_teardown::ticket{};
   BOOST_TEST(retirement.complete_terminal(terminal_ticket));
   BOOST_TEST(terminal_ticket.active());
   auto duplicate_ticket = detail::session_teardown::ticket{};
   BOOST_TEST(!retirement.complete_terminal(duplicate_ticket));
   BOOST_TEST(retirement.terminal());
   BOOST_TEST(!retirement.tracked());

   auto ownership_released = std::atomic_bool{false};
   teardown.start({});
   auto stopped = boost::asio::co_spawn(
       runtime.context(), wait_for_terminal_cleanup(&teardown, &ownership_released), boost::asio::use_future);
   BOOST_TEST(static_cast<int>(stopped.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));

   ownership_released.store(true, std::memory_order_release);
   terminal_ticket.release();
   BOOST_REQUIRE(static_cast<int>(stopped.wait_for(std::chrono::seconds{1})) ==
                 static_cast<int>(std::future_status::ready));
   BOOST_TEST(stopped.get());
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_quarantines_untracked_close_without_duplicate_attempt) {
   auto retirement = detail::session_retirement{};

   BOOST_TEST(static_cast<int>(retirement.begin_close(false)) ==
              static_cast<int>(detail::session_retirement::close_start::untracked));
   BOOST_TEST(static_cast<int>(retirement.begin_close(true)) ==
              static_cast<int>(detail::session_retirement::close_start::started));
   BOOST_TEST(static_cast<int>(retirement.begin_close(true)) ==
              static_cast<int>(detail::session_retirement::close_start::in_flight));
   retirement.quarantine();
   BOOST_TEST(static_cast<int>(retirement.begin_close(false)) ==
              static_cast<int>(detail::session_retirement::close_start::untracked));
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_wait_observes_completion_before_subscription) {
   auto context = boost::asio::io_context{};
   auto retirement = detail::session_retirement{};
   using close_start = detail::session_retirement::close_start;

   BOOST_REQUIRE(retirement.begin_close(true) == close_start::started);
   BOOST_REQUIRE(retirement.begin_close(true) == close_start::in_flight);
   // Creating an awaitable does not run its body. Completion in this gap must
   // remain observable even though no notification subscriber exists yet.
   auto wait = retirement.async_wait_not_in_flight();
   auto ticket = detail::session_teardown::ticket{};
   BOOST_REQUIRE(retirement.complete_terminal(ticket));
   auto completed = boost::asio::co_spawn(context, std::move(wait), boost::asio::use_future);
   context.poll();
   BOOST_REQUIRE(completed.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(completed.get());
   BOOST_CHECK(retirement.begin_close(true) == close_start::terminal);

   context.restart();
   auto late = boost::asio::co_spawn(context, retirement.async_wait_not_in_flight(), boost::asio::use_future);
   context.poll();
   BOOST_REQUIRE(late.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(late.get());
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_parallel_waiters_observe_cleanup_barrier) {
   auto context = boost::asio::io_context{};
   auto retirement = detail::session_retirement{};
   BOOST_REQUIRE(retirement.begin_close(true) == detail::session_retirement::close_start::started);

   auto native_closed = false;
   auto resources_released = false;
   auto registry_erased = false;
   auto wait = [&]() -> boost::asio::awaitable<bool> {
      co_await retirement.async_wait_not_in_flight();
      co_return retirement.terminal() && native_closed && resources_released && registry_erased;
   };
   auto waiters = std::vector<std::future<bool>>{};
   for (auto index = 0; index < 8; ++index) {
      waiters.push_back(boost::asio::co_spawn(context, wait(), boost::asio::use_future));
   }
   context.poll();
   native_closed = true;
   resources_released = true;
   registry_erased = true;
   context.poll();
   for (auto& waiter : waiters) {
      BOOST_CHECK(waiter.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
   }

   auto ticket = detail::session_teardown::ticket{};
   BOOST_REQUIRE(retirement.complete_terminal(ticket));
   context.poll();
   for (auto& waiter : waiters) {
      BOOST_REQUIRE(waiter.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
      BOOST_TEST(waiter.get());
   }
   BOOST_TEST(!retirement.complete_terminal(ticket));
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_wait_observes_quarantine_before_subscription) {
   auto context = boost::asio::io_context{};
   auto retirement = detail::session_retirement{};
   using close_start = detail::session_retirement::close_start;
   BOOST_REQUIRE(retirement.begin_close(true) == close_start::started);
   BOOST_REQUIRE(retirement.begin_close(true) == close_start::in_flight);
   auto wait = retirement.async_wait_not_in_flight();
   retirement.quarantine();
   auto completed = boost::asio::co_spawn(context, std::move(wait), boost::asio::use_future);
   context.poll();
   BOOST_REQUIRE(completed.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(completed.get());
   BOOST_TEST(!retirement.terminal());
   BOOST_CHECK(retirement.begin_close(true) == close_start::started);
   BOOST_CHECK(retirement.begin_close(true) == close_start::in_flight);
   auto ticket = detail::session_teardown::ticket{};
   BOOST_TEST(retirement.complete_terminal(ticket));
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_quarantine_wakes_subscribers_and_rechecks_new_owner) {
   auto context = boost::asio::io_context{};
   auto retirement = detail::session_retirement{};
   using close_start = detail::session_retirement::close_start;
   BOOST_REQUIRE(retirement.begin_close(true) == close_start::started);
   auto first = boost::asio::co_spawn(context, retirement.async_wait_not_in_flight(), boost::asio::use_future);
   context.poll();
   BOOST_CHECK(first.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
   retirement.quarantine();
   context.poll();
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(first.get());
   BOOST_TEST(!retirement.terminal());

   context.restart();
   BOOST_REQUIRE(retirement.begin_close(true) == close_start::started);
   auto second = boost::asio::co_spawn(context, retirement.async_wait_not_in_flight(), boost::asio::use_future);
   context.poll();
   retirement.quarantine();
   // A new owner can win before an already notified coroutine resumes.
   BOOST_REQUIRE(retirement.begin_close(true) == close_start::started);
   context.poll();
   BOOST_CHECK(second.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
   auto ticket = detail::session_teardown::ticket{};
   BOOST_REQUIRE(retirement.complete_terminal(ticket));
   context.poll();
   BOOST_REQUIRE(second.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(second.get());
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_quarantine_wakes_parallel_retries_with_one_owner) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   auto retirement = detail::session_retirement{};
   using close_start = detail::session_retirement::close_start;
   BOOST_REQUIRE(retirement.track(teardown.track()));
   BOOST_REQUIRE(retirement.begin_close(false) == close_start::started);

   auto entered = std::atomic_size_t{0};
   auto owners = std::atomic_size_t{0};
   auto finish_retry = forge::asio::notification{};
   auto retry = [&]() -> boost::asio::awaitable<bool> {
      entered.fetch_add(1, std::memory_order_release);
      for (;;) {
         const auto start = retirement.begin_close(true);
         if (start == close_start::in_flight) {
            co_await retirement.async_wait_not_in_flight();
            continue;
         }
         if (start == close_start::terminal) {
            co_return true;
         }
         if (start != close_start::started) {
            co_return false;
         }
         owners.fetch_add(1, std::memory_order_release);
         co_await finish_retry.async_wait(0);
         auto ticket = detail::session_teardown::ticket{};
         co_return retirement.complete_terminal(ticket);
      }
   };
   auto waiters = std::vector<std::future<bool>>{};
   for (auto index = 0; index < 8; ++index) {
      waiters.push_back(boost::asio::co_spawn(runtime.context(), retry(), boost::asio::use_future));
   }
   const auto all_entered = wait_for_count(entered, 8);
   retirement.quarantine();
   const auto retry_started = wait_for_count(owners, 1);
   BOOST_TEST(all_entered);
   BOOST_TEST(retry_started);
   BOOST_TEST(!retirement.tracked());
   BOOST_TEST(!retirement.terminal());
   for (auto& waiter : waiters) {
      BOOST_CHECK(waiter.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
   }
   finish_retry.notify();
   auto all_ready = true;
   for (auto& waiter : waiters) {
      if (waiter.wait_for(std::chrono::seconds{2}) != std::future_status::ready) {
         all_ready = false;
      }
   }
   // A lost-wakeup failure must not leave workers accessing destroyed locals.
   runtime.stop();
   BOOST_REQUIRE(all_ready);
   for (auto& waiter : waiters) {
      BOOST_TEST(waiter.get());
   }
   BOOST_TEST(owners.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(retirement.terminal());
}

BOOST_AUTO_TEST_CASE(p2p_staged_attempt_close_error_releases_resources_after_terminal_barrier) {
   struct staged_attempt_owner {
      detail::session_teardown::ticket terminal_ticket;
      resource_manager::session_reservation reservation;
      resource_manager::file_descriptor_reservation file_descriptor;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   auto resources = resource_manager{resource_manager::limits{.system = {.max_file_descriptors = 1, .max_connections = 1}}};
   auto admission = resources.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(admission);
   auto descriptor = admission->reserve_file_descriptors(1);
   BOOST_REQUIRE(descriptor);
   auto cancellation_requested = std::atomic_size_t{0};
   auto owner = std::make_shared<staged_attempt_owner>();
   owner->terminal_ticket = teardown.track([&cancellation_requested] {
      cancellation_requested.fetch_add(1U, std::memory_order_release);
   });
   BOOST_REQUIRE(owner->terminal_ticket.active());
   owner->reservation = std::move(*admission);
   owner->file_descriptor = std::move(*descriptor);

   auto terminal_owner = std::weak_ptr<staged_attempt_owner>{owner};
   auto model = std::make_shared<terminal_throwing_session>(owner);
   auto candidate = forge::net::transport::detail::session_access::make(model);
   owner.reset();
   candidate.request_cancel();
   BOOST_TEST(!candidate.valid());
   BOOST_TEST(model->cancel_calls() == 1U);

   teardown.start({});
   auto stopped = boost::asio::co_spawn(runtime.context(), teardown.wait(), boost::asio::use_future);
   BOOST_TEST(static_cast<int>(stopped.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_TEST(cancellation_requested.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(resources.current().system.outbound_connections == 1U);
   BOOST_TEST(resources.current().system.file_descriptors == 1U);
   BOOST_TEST(!resources.reserve_session(resource_manager::session_direction::outbound));

   auto closed = boost::asio::co_spawn(runtime.context(), candidate.async_close(), boost::asio::use_future);
   BOOST_REQUIRE(static_cast<int>(closed.wait_for(std::chrono::seconds{1})) ==
                 static_cast<int>(std::future_status::ready));
   BOOST_CHECK_THROW(closed.get(), std::runtime_error);
   BOOST_TEST(model->close_calls() == 1U);
   BOOST_TEST(!terminal_owner.lock());

   BOOST_REQUIRE(static_cast<int>(stopped.wait_for(std::chrono::seconds{1})) ==
                 static_cast<int>(std::future_status::ready));
   BOOST_TEST(resources.current().system.outbound_connections == 0U);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
   stopped.get();
   BOOST_REQUIRE(resources.reserve_session(resource_manager::session_direction::outbound));
}

BOOST_AUTO_TEST_CASE(p2p_unpublished_direct_discard_holds_admission_and_native_lifetime_to_terminal_close) {
   struct inbound_owner {
      detail::session_teardown::ticket teardown_ticket;
      resource_manager::file_descriptor_reservation file_descriptor;
   };

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto teardown = detail::session_teardown{runtime.context().get_executor()};
   auto resources = resource_manager{resource_manager::limits{.system = {.max_file_descriptors = 1, .max_connections = 1}}};
   auto admission = resources.reserve_session(resource_manager::session_direction::inbound);
   BOOST_REQUIRE(admission);
   auto descriptor = admission->reserve_file_descriptors(1);
   BOOST_REQUIRE(descriptor);

   auto owner = std::make_shared<inbound_owner>();
   owner->teardown_ticket = teardown.track();
   owner->file_descriptor = std::move(*descriptor);
   auto terminal_owner = std::weak_ptr<inbound_owner>{owner};
   auto state = std::make_shared<terminal_barrier_state>();
   auto model = std::make_shared<terminal_barrier_session>(state, owner);
   auto connection = direct::connection{
       .session = forge::net::transport::detail::session_access::make(model),
       .admission = std::move(*admission),
       .native_lifetime = owner,
   };
   model.reset();
   owner.reset();

   teardown.start({});
   auto stopped = boost::asio::co_spawn(runtime.context(), teardown.wait(), boost::asio::use_future);
   auto discarded = boost::asio::co_spawn(runtime.context(), direct::async_discard_unpublished(connection),
                                           boost::asio::use_future);
   BOOST_REQUIRE(wait_for_count(state->cancel_calls, 1U));
   const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!state->close_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < close_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   BOOST_REQUIRE(state->close_entered.load(std::memory_order_acquire));
   BOOST_TEST(static_cast<int>(discarded.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_TEST(static_cast<int>(stopped.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_TEST(resources.current().system.inbound_connections == 1U);
   BOOST_TEST(resources.current().system.file_descriptors == 1U);

   state->release_close.store(true, std::memory_order_release);
   state->changed.notify();
   BOOST_REQUIRE(static_cast<int>(discarded.wait_for(std::chrono::seconds{1})) ==
                 static_cast<int>(std::future_status::ready));
   discarded.get();
   BOOST_TEST(!terminal_owner.lock());
   BOOST_REQUIRE(static_cast<int>(stopped.wait_for(std::chrono::seconds{1})) ==
                 static_cast<int>(std::future_status::ready));
   stopped.get();
   BOOST_TEST(resources.current().system.inbound_connections == 0U);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
}

} // namespace
} // namespace forge::net::p2p
