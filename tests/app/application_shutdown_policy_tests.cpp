#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <initializer_list>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

import forge.app.application;
import forge.app.application_shell;
import forge.app.events;
import forge.app.exceptions;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_lifecycle;
import forge.app.plugin_registry;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;

namespace {

constexpr auto hook_deadline = std::chrono::seconds{5};
constexpr auto result_deadline = std::chrono::seconds{6};

struct policy_state {
   forge::asio::notification initializing;
   forge::asio::notification initialized;
   forge::asio::notification stopping;
   forge::asio::notification cleaning;
   forge::asio::notification cleaned;
   std::atomic_bool hold_initialize = false;
   std::atomic_bool hold_cleanup = false;
   std::atomic_bool fail_cleanup = false;
   std::atomic_uint stops = 0;
   std::atomic_uint cleanups = 0;
   std::shared_ptr<boost::asio::cancellation_signal> cancel_on_stop;
   std::shared_ptr<boost::asio::cancellation_signal> cancel_on_cleanup;
};

class policy_plugin final : public forge::app::plugin {
 public:
   explicit policy_plugin(std::shared_ptr<policy_state> state) : state_{std::move(state)} {}
   forge::app::plugin_id id() const override { return {"shutdown-policy"}; }
   std::string version() const override { return "1"; }

 protected:
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override {
      const auto epoch = state_->initialized.epoch();
      state_->initializing.notify();
      if (state_->hold_initialize.load()) {
         const auto result = co_await state_->initialized.async_wait_until(
             epoch, std::chrono::steady_clock::now() + hook_deadline);
         if (result == epoch) {
            throw forge::app::exceptions::invalid_state{"test initialize deadline"};
         }
      }
   }
   boost::asio::awaitable<void> on_startup() override { co_return; }
   void on_request_stop() noexcept override {
      ++state_->stops;
      if (state_->cancel_on_stop) {
         state_->cancel_on_stop->emit(boost::asio::cancellation_type::terminal);
      }
      state_->stopping.notify();
   }
   boost::asio::awaitable<void> on_shutdown() override {
      ++state_->cleanups;
      const auto epoch = state_->cleaned.epoch();
      if (state_->cancel_on_cleanup) {
         state_->cancel_on_cleanup->emit(boost::asio::cancellation_type::terminal);
      }
      state_->cleaning.notify();
      if (state_->hold_cleanup.load()) {
         const auto result = co_await state_->cleaned.async_wait_until(
             epoch, std::chrono::steady_clock::now() + hook_deadline);
         if (result == epoch) {
            throw forge::app::exceptions::invalid_state{"test cleanup deadline"};
         }
      }
      co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
      if (state_->fail_cleanup.load()) {
         throw forge::app::exceptions::shutdown_failed{"test cleanup retry"};
      }
   }

 private:
   std::shared_ptr<policy_state> state_;
};

struct plugin_owner {
   explicit plugin_owner(std::shared_ptr<policy_state> state) : plugin{std::move(state)} {}
   forge::asio::runtime runtime_value{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::task::scheduler scheduler{runtime_value};
   forge::app::event_journal events{};
   forge::app::plugin_context context{scheduler, events};
   policy_plugin plugin;
   forge::asio::runtime& runtime() { return runtime_value; }
   boost::asio::awaitable<void> initialize() { return plugin.initialize(context); }
   boost::asio::awaitable<void> shutdown() { return plugin.shutdown(); }
   forge::app::lifecycle_observer lifecycle() { return plugin.lifecycle(); }
};

std::vector<std::unique_ptr<forge::app::plugin>> policy_plugins(std::shared_ptr<policy_state> state) {
   auto result = std::vector<std::unique_ptr<forge::app::plugin>>{};
   result.push_back(std::make_unique<policy_plugin>(std::move(state)));
   return result;
}

struct runtime_owner {
   explicit runtime_owner(std::shared_ptr<policy_state> state)
       : application{context, policy_plugins(std::move(state))} {}
   forge::asio::runtime runtime_value{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::task::scheduler scheduler{runtime_value};
   forge::app::event_journal events{};
   forge::app::plugin_context context{scheduler, events};
   forge::app::application_runtime application;
   forge::asio::runtime& runtime() { return runtime_value; }
   boost::asio::awaitable<void> initialize() { return application.initialize(); }
   boost::asio::awaitable<void> shutdown() { return application.shutdown(); }
   forge::app::lifecycle_observer lifecycle() { return application.lifecycle(); }
};

class shell_owner final : public forge::app::application_shell {
 public:
   explicit shell_owner(std::shared_ptr<policy_state> state)
       : application_shell{forge::app::application_shell_options{.runtime = {.worker_threads = 4}}},
         state_{std::move(state)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin({
         .id = {"shutdown-policy"},
         .factory = [state = state_] { return std::make_unique<policy_plugin>(state); },
      });
   }

 private:
   std::shared_ptr<policy_state> state_;
};

struct filter_counts {
   unsigned input = 0;
   unsigned output = 0;
};

struct counting_filter {
   std::shared_ptr<filter_counts> counts;
   bool input;
   boost::asio::cancellation_type_t operator()(boost::asio::cancellation_type_t type) const noexcept {
      if (input) {
         ++counts->input;
         return boost::asio::enable_partial_cancellation{}(type);
      }
      ++counts->output;
      return boost::asio::enable_terminal_cancellation{}(type);
   }
};

struct policy_result {
   std::exception_ptr failure;
   boost::asio::cancellation_type_t cancelled = boost::asio::cancellation_type::none;
   bool throws = false;
   bool next_await_cancelled = false;
   bool partial_retained = false;
   unsigned input_calls = 0;
   unsigned output_calls = 0;
};

bool aborted(const std::exception_ptr& error) {
   if (!error) {
      return false;
   }
   try {
      std::rethrow_exception(error);
   } catch (const boost::system::system_error& value) {
      return value.code() == boost::asio::error::operation_aborted;
   } catch (...) {
      return false;
   }
}

boost::asio::awaitable<policy_result> inspect_policy(boost::asio::awaitable<void> operation,
    std::shared_ptr<boost::asio::cancellation_signal> signal, bool throws, bool pre_cancel = false) {
   const auto filters = std::make_shared<filter_counts>();
   co_await boost::asio::this_coro::reset_cancellation_state(
       counting_filter{filters, true}, counting_filter{filters, false});
   co_await boost::asio::this_coro::throw_if_cancelled(throws);
   if (pre_cancel) {
      signal->emit(boost::asio::cancellation_type::terminal);
   }
   auto result = policy_result{};
   try {
      co_await std::move(operation);
   } catch (...) {
      result.failure = std::current_exception();
   }
   const auto state = co_await boost::asio::this_coro::cancellation_state;
   result.cancelled = state.cancelled();
   result.throws = co_await boost::asio::this_coro::throw_if_cancelled();
   try {
      co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
   } catch (...) {
      result.next_await_cancelled = aborted(std::current_exception());
   }
   signal->emit(boost::asio::cancellation_type::partial);
   result.partial_retained = state.cancelled() == boost::asio::cancellation_type::partial;
   result.input_calls = filters->input;
   result.output_calls = filters->output;
   co_return result;
}

boost::asio::awaitable<std::exception_ptr> capture_policy_error(boost::asio::awaitable<void> operation) {
   try {
      co_await std::move(operation);
   } catch (...) {
      co_return std::current_exception();
   }
   co_return std::exception_ptr{};
}

boost::asio::awaitable<void> finish_core_cleanup(forge::app::plugin_lifecycle lifecycle) {
   auto operation = co_await lifecycle.begin(forge::app::lifecycle_operation::shutdown);
   if (const auto failure = operation.complete()) {
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<void> join_explicit_cleanup(forge::app::plugin_lifecycle lifecycle,
                                                std::stop_token stop) {
   auto operation = co_await lifecycle.begin(forge::app::lifecycle_operation::shutdown, std::move(stop));
   if (const auto failure = operation.complete()) {
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<std::exception_ptr> finish_then_cancel(forge::app::plugin_lifecycle::operation owner,
    std::exception_ptr failure, std::stop_source* stop, boost::asio::cancellation_signal* signal) {
   const auto result = owner.complete(std::move(failure));
   static_cast<void>(stop->request_stop());
   signal->emit(boost::asio::cancellation_type::terminal);
   co_return result;
}

boost::asio::awaitable<forge::app::lifecycle_snapshot>
wait_cleanup_reserved(forge::app::lifecycle_observer observer) {
   auto state = observer.snapshot();
   while (!state.in_progress) {
      state = co_await observer.async_wait_until(state.epoch,
          std::chrono::steady_clock::now() + hook_deadline);
   }
   co_return state;
}

template <typename T> T ready_policy(std::future<T>& result) {
   BOOST_REQUIRE(result.wait_for(result_deadline) == std::future_status::ready);
   return result.get();
}

template <typename Owner> struct policy_fixture {
   std::shared_ptr<policy_state> state = std::make_shared<policy_state>();
   std::shared_ptr<boost::asio::cancellation_signal> signal = std::make_shared<boost::asio::cancellation_signal>();
   Owner owner{state};
   boost::asio::strand<boost::asio::io_context::executor_type> serial{
       boost::asio::make_strand(owner.runtime().context())};

   ~policy_fixture() {
      state->initialized.notify();
      state->cleaned.notify();
      // Join worker execution before owner/signal destruction, also on failure.
      owner.runtime().stop();
   }

   void wait_event(forge::asio::notification& event, std::uint64_t before) {
      auto wait = boost::asio::co_spawn(serial, event.async_wait_until(
          before, std::chrono::steady_clock::now() + hook_deadline), boost::asio::use_future);
      BOOST_REQUIRE(ready_policy(wait) != before);
   }

   std::future<policy_result> inspect(boost::asio::awaitable<void> operation,
                                     bool throws = true, bool pre_cancel = false) {
      return boost::asio::co_spawn(serial, inspect_policy(std::move(operation), signal, throws, pre_cancel),
          boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_future));
   }

   void cancel() {
      auto delivered = boost::asio::post(serial, boost::asio::use_future([signal = signal] {
         signal->emit(boost::asio::cancellation_type::terminal);
      }));
      BOOST_REQUIRE(delivered.wait_for(result_deadline) == std::future_status::ready);
      delivered.get();
   }
};

void check_policy(const policy_result& result, bool throws) {
   BOOST_CHECK(result.cancelled == boost::asio::cancellation_type::terminal);
   BOOST_CHECK_EQUAL(result.throws, throws);
   BOOST_CHECK_EQUAL(result.next_await_cancelled, throws);
   BOOST_CHECK(result.partial_retained);
   BOOST_CHECK_EQUAL(result.input_calls, 2U);
   BOOST_CHECK_EQUAL(result.output_calls, 2U);
}

template <typename Owner> void immediate_cleanup_policy() {
   for (const auto throws : {false, true}) {
      for (const auto fail : {false, true}) {
         for (const auto at_stop : {false, true}) {
            auto fixture = policy_fixture<Owner>{};
            auto initialize = boost::asio::co_spawn(fixture.serial, capture_policy_error(fixture.owner.initialize()),
                                                   boost::asio::use_future);
            BOOST_REQUIRE(!ready_policy(initialize));
            if (at_stop) {
               fixture.state->cancel_on_stop = fixture.signal;
            } else {
               fixture.state->cancel_on_cleanup = fixture.signal;
            }
            fixture.state->fail_cleanup.store(fail);
            auto shutdown = fixture.inspect(fixture.owner.shutdown(), throws);
            const auto result = ready_policy(shutdown);
            check_policy(result, throws);
            BOOST_CHECK_EQUAL(static_cast<bool>(result.failure), fail);
            if (fail) {
               BOOST_CHECK(!aborted(result.failure));
            }
            BOOST_CHECK_EQUAL(fixture.state->stops.load(), 1U);
            BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), 1U);
            BOOST_CHECK(fixture.owner.lifecycle().snapshot().phase == (fail
                ? forge::app::lifecycle_phase::stopping : forge::app::lifecycle_phase::stopped));
            fixture.state->fail_cleanup.store(false);
            auto retry = boost::asio::co_spawn(fixture.serial, capture_policy_error(fixture.owner.shutdown()),
                                              boost::asio::use_future);
            BOOST_CHECK(!ready_policy(retry));
            BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), fail ? 2U : 1U);
         }
      }
   }
}

template <typename Owner> void waiting_cleanup_policy() {
   auto fixture = policy_fixture<Owner>{};
   fixture.state->hold_initialize.store(true);
   const auto before_initialize = fixture.state->initializing.epoch();
   auto initialize = boost::asio::co_spawn(fixture.serial, capture_policy_error(fixture.owner.initialize()),
                                         boost::asio::use_future);
   fixture.wait_event(fixture.state->initializing, before_initialize);
   const auto before_stop = fixture.state->stopping.epoch();
   auto shutdown = fixture.inspect(fixture.owner.shutdown());
   fixture.wait_event(fixture.state->stopping, before_stop);
   fixture.cancel();
   BOOST_CHECK(shutdown.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), 0U);
   fixture.state->initialized.notify();
   BOOST_REQUIRE(ready_policy(initialize));
   const auto result = ready_policy(shutdown);
   BOOST_CHECK(!result.failure);
   check_policy(result, true);
   BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), 1U);
}

template <typename Owner> void rollback_policy() {
   for (const auto throws : {false, true}) {
      for (const auto fail : {false, true}) {
         auto fixture = policy_fixture<Owner>{};
         fixture.state->hold_initialize.store(true);
         fixture.state->fail_cleanup.store(fail);
         const auto entered = fixture.state->initializing.epoch();
         auto initialize = fixture.inspect(fixture.owner.initialize(), throws);
         fixture.wait_event(fixture.state->initializing, entered);
         fixture.cancel();
         const auto result = ready_policy(initialize);
         BOOST_REQUIRE(result.failure);
         BOOST_CHECK_EQUAL(aborted(result.failure), !fail);
         check_policy(result, throws);
         BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), 1U);
         BOOST_CHECK(fixture.owner.lifecycle().snapshot().phase == (fail
             ? forge::app::lifecycle_phase::stopping : forge::app::lifecycle_phase::stopped));
         fixture.state->fail_cleanup.store(false);
         auto retry = boost::asio::co_spawn(fixture.serial, capture_policy_error(fixture.owner.shutdown()),
                                          boost::asio::use_future);
         BOOST_CHECK(!ready_policy(retry));
         BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), fail ? 2U : 1U);
      }
   }
}

template <typename Owner> void joiner_policy() {
   auto fixture = policy_fixture<Owner>{};
   auto initialize = boost::asio::co_spawn(fixture.serial, capture_policy_error(fixture.owner.initialize()),
                                         boost::asio::use_future);
   BOOST_REQUIRE(!ready_policy(initialize));
   fixture.state->hold_cleanup.store(true);
   const auto entered = fixture.state->cleaning.epoch();
   auto owner = boost::asio::co_spawn(fixture.serial, capture_policy_error(fixture.owner.shutdown()),
                                    boost::asio::use_future);
   fixture.wait_event(fixture.state->cleaning, entered);
   auto joiner = fixture.inspect(fixture.owner.shutdown());
   fixture.cancel();
   const auto result = ready_policy(joiner);
   BOOST_CHECK(aborted(result.failure));
   check_policy(result, true);
   BOOST_CHECK(owner.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), 1U);
   BOOST_CHECK(fixture.owner.lifecycle().snapshot().phase == forge::app::lifecycle_phase::stopping);
   fixture.state->cleaned.notify();
   BOOST_CHECK(!ready_policy(owner));
   BOOST_CHECK(fixture.owner.lifecycle().snapshot().phase == forge::app::lifecycle_phase::stopped);
}

} // namespace

BOOST_AUTO_TEST_CASE(plugin_shutdown_immediate_stop_cancellation_preserves_policy_and_retry) {
   immediate_cleanup_policy<plugin_owner>();
}
BOOST_AUTO_TEST_CASE(application_runtime_shutdown_immediate_stop_cancellation_preserves_policy_and_retry) {
   immediate_cleanup_policy<runtime_owner>();
}
BOOST_AUTO_TEST_CASE(application_shell_shutdown_immediate_stop_cancellation_preserves_policy_and_retry) {
   immediate_cleanup_policy<shell_owner>();
}
BOOST_AUTO_TEST_CASE(plugin_shutdown_draining_initialize_preserves_caller_policy) {
   waiting_cleanup_policy<plugin_owner>();
}
BOOST_AUTO_TEST_CASE(application_runtime_shutdown_draining_initialize_preserves_caller_policy) {
   waiting_cleanup_policy<runtime_owner>();
}
BOOST_AUTO_TEST_CASE(application_shell_shutdown_draining_initialize_preserves_caller_policy) {
   waiting_cleanup_policy<shell_owner>();
}
BOOST_AUTO_TEST_CASE(application_runtime_rollback_retains_caller_cancellation_and_throw_policy) {
   rollback_policy<runtime_owner>();
}
BOOST_AUTO_TEST_CASE(application_shell_rollback_retains_caller_cancellation_and_throw_policy) {
   rollback_policy<shell_owner>();
}
BOOST_AUTO_TEST_CASE(plugin_shutdown_cancelled_joiner_preserves_policy_without_cancelling_owner) {
   joiner_policy<plugin_owner>();
}
BOOST_AUTO_TEST_CASE(application_runtime_shutdown_cancelled_joiner_preserves_policy_without_cancelling_owner) {
   joiner_policy<runtime_owner>();
}
BOOST_AUTO_TEST_CASE(application_shell_shutdown_cancelled_joiner_preserves_policy_without_cancelling_owner) {
   joiner_policy<shell_owner>();
}
BOOST_AUTO_TEST_CASE(plugin_lifecycle_lowlevel_joiner_preserves_associated_cancellation_policy) {
   auto fixture = policy_fixture<plugin_owner>{};
   auto lifecycle = forge::app::plugin_lifecycle{};
   auto beginning = boost::asio::co_spawn(fixture.serial,
       lifecycle.begin(forge::app::lifecycle_operation::shutdown), boost::asio::use_future);
   auto owner = ready_policy(beginning);
   BOOST_REQUIRE(owner);
   auto joiner = fixture.inspect(finish_core_cleanup(lifecycle));
   fixture.cancel();
   const auto result = ready_policy(joiner);
   BOOST_CHECK(aborted(result.failure));
   check_policy(result, true);
   BOOST_CHECK(!lifecycle.snapshot().failure);
   BOOST_CHECK(lifecycle.snapshot().phase == forge::app::lifecycle_phase::stopping);
   BOOST_CHECK(!owner.complete());
   BOOST_CHECK(lifecycle.snapshot().phase == forge::app::lifecycle_phase::stopped);
}
BOOST_AUTO_TEST_CASE(plugin_lifecycle_pending_stop_cleanup_owner_preserves_caller_policy) {
   auto fixture = policy_fixture<plugin_owner>{};
   auto lifecycle = forge::app::plugin_lifecycle{};
   BOOST_REQUIRE(lifecycle.request_stop());
   auto cleanup = fixture.inspect(finish_core_cleanup(lifecycle));
   auto reserved = boost::asio::co_spawn(fixture.serial, wait_cleanup_reserved(lifecycle.observer()),
                                       boost::asio::use_future);
   BOOST_CHECK(ready_policy(reserved).in_progress == forge::app::lifecycle_operation::shutdown);
   fixture.cancel();
   BOOST_CHECK(cleanup.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   lifecycle.complete_stop();
   const auto result = ready_policy(cleanup);
   BOOST_CHECK(!result.failure);
   check_policy(result, true);
   BOOST_CHECK(lifecycle.snapshot().phase == forge::app::lifecycle_phase::stopped);
}
BOOST_AUTO_TEST_CASE(plugin_lifecycle_published_joined_result_wins_explicit_and_associated_cancellation) {
   for (const auto explicit_stop : {false, true}) {
      for (const auto fails : {false, true}) {
         for (auto iteration = 0; iteration != 8; ++iteration) {
            auto fixture = policy_fixture<plugin_owner>{};
            auto lifecycle = forge::app::plugin_lifecycle{};
            auto stop = std::stop_source{};
            auto beginning = boost::asio::co_spawn(fixture.serial,
                lifecycle.begin(forge::app::lifecycle_operation::shutdown), boost::asio::use_future);
            auto owner = ready_policy(beginning);
            const auto expected = fails
                ? std::make_exception_ptr(forge::app::exceptions::shutdown_failed{"test terminal failure"})
                : std::exception_ptr{};
            auto joined = boost::asio::co_spawn(fixture.serial, capture_policy_error(explicit_stop
                ? join_explicit_cleanup(lifecycle, stop.get_token()) : finish_core_cleanup(lifecycle)),
                boost::asio::bind_cancellation_slot(fixture.signal->slot(), boost::asio::use_future));
            auto waiting = boost::asio::post(fixture.serial, boost::asio::use_future);
            BOOST_REQUIRE(waiting.wait_for(result_deadline) == std::future_status::ready);
            waiting.get();
            BOOST_CHECK(joined.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
            // The serial executor prevents waiter resumption between result
            // publication and cancellation. No sleeps or internal hooks needed.
            auto completed = boost::asio::co_spawn(fixture.serial,
                finish_then_cancel(std::move(owner), expected, &stop, fixture.signal.get()), boost::asio::use_future);
            BOOST_CHECK(ready_policy(completed) == expected);
            BOOST_CHECK(ready_policy(joined) == expected);
            const auto final = lifecycle.snapshot();
            BOOST_CHECK(final.phase == (fails
                ? forge::app::lifecycle_phase::stopping : forge::app::lifecycle_phase::stopped));
            if (fails) {
               BOOST_REQUIRE(final.failure);
               BOOST_CHECK(final.failure->exception == expected);
            }
         }
      }
   }
}
BOOST_AUTO_TEST_CASE(plugin_lifecycle_destroyed_owner_releases_reserved_cleanup_and_joiner) {
   for (const auto active_hook : {false, true}) {
      auto fixture = policy_fixture<plugin_owner>{};
      auto lifecycle = forge::app::plugin_lifecycle{};
      auto active = forge::app::plugin_lifecycle::operation{};
      if (active_hook) {
         auto beginning = boost::asio::co_spawn(fixture.serial,
             lifecycle.begin(forge::app::lifecycle_operation::initialize), boost::asio::use_future);
         active = ready_policy(beginning);
      }
      BOOST_REQUIRE(lifecycle.request_stop());
      if (active_hook) {
         lifecycle.complete_stop();
      }
      const auto before = lifecycle.snapshot().epoch;
      auto cleanup = boost::asio::co_spawn(fixture.serial, capture_policy_error(finish_core_cleanup(lifecycle)),
                                         boost::asio::use_future);
      auto reserved = boost::asio::co_spawn(fixture.serial, lifecycle.observer().async_wait_until(
          before, std::chrono::steady_clock::now() + hook_deadline), boost::asio::use_future);
      BOOST_REQUIRE(ready_policy(reserved).in_progress);
      auto joiner = boost::asio::co_spawn(fixture.serial, capture_policy_error(finish_core_cleanup(lifecycle)),
                                        boost::asio::use_future);
      auto joined = boost::asio::post(fixture.serial, boost::asio::use_future);
      BOOST_REQUIRE(joined.wait_for(result_deadline) == std::future_status::ready);
      joined.get();
      lifecycle.abandon_owner();
      const auto failure = ready_policy(cleanup);
      BOOST_REQUIRE(failure);
      BOOST_CHECK(ready_policy(joiner) == failure);
      const auto final = lifecycle.snapshot();
      BOOST_CHECK(!final.owner_alive);
      BOOST_CHECK(final.phase == forge::app::lifecycle_phase::stopping);
      BOOST_CHECK(!final.completed(forge::app::lifecycle_operation::shutdown));
      BOOST_REQUIRE(final.failure);
      BOOST_CHECK(final.failure->exception == failure);
   }
}
BOOST_AUTO_TEST_CASE(plugin_shutdown_lazy_construction_and_asio_pre_entry_cancellation) {
   auto fixture = policy_fixture<plugin_owner>{};
   auto lazy = fixture.owner.shutdown();
   BOOST_CHECK_EQUAL(fixture.state->stops.load(), 0U);
   BOOST_CHECK(!fixture.owner.lifecycle().snapshot().stop_requested);
   auto unreachable = fixture.inspect(std::move(lazy), true, true);
   const auto rejected = ready_policy(unreachable);
   BOOST_CHECK(aborted(rejected.failure));
   check_policy(rejected, true);
   BOOST_CHECK_EQUAL(fixture.state->stops.load(), 0U);
   BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), 0U);

   auto reachable = fixture.inspect(fixture.owner.shutdown(), false, true);
   const auto accepted = ready_policy(reachable);
   BOOST_CHECK(!accepted.failure);
   check_policy(accepted, false);
   BOOST_CHECK_EQUAL(fixture.state->stops.load(), 1U);
   BOOST_CHECK_EQUAL(fixture.state->cleanups.load(), 1U);
}
