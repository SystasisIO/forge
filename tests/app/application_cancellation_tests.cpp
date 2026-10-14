#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/system_error.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
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

enum class cancelled_hook { configure, initialize, startup };

constexpr auto hook_timeout = std::chrono::seconds{5};
constexpr auto result_timeout = std::chrono::seconds{6};

struct cancellation_state {
   explicit cancellation_state(cancelled_hook input, bool fail_cleanup = false)
       : target{input}, fail_first_cleanup{fail_cleanup} {}

   cancelled_hook target;
   bool fail_first_cleanup;
   bool pause_enabled = true;
   forge::asio::notification entered;
   forge::asio::notification release;
   forge::asio::task::scheduler* scheduler = nullptr;
   std::atomic_bool store_live = false;
   std::atomic_bool partial_resource = false;
   std::atomic_bool cleanup_lost_dependency = false;
   std::atomic_bool cleanup_lost_scheduler = false;
   std::atomic_bool throw_identity = false;
   std::atomic_uint dependent_cleanups = 0;
   std::atomic_uint store_cleanups = 0;
   std::atomic_uint cleanup_work = 0;

   boost::asio::awaitable<void> pause() {
      const auto epoch = release.epoch();
      entered.notify();
      static_cast<void>(co_await release.async_wait_until(epoch, std::chrono::steady_clock::now() + hook_timeout));
   }
};

class cancellation_store final : public forge::app::plugin {
 public:
   explicit cancellation_store(std::shared_ptr<cancellation_state> state) : state_{std::move(state)} {}
   forge::app::plugin_id id() const override { return {"cancel.store"}; }
   std::string version() const override { return "1"; }

 protected:
   boost::asio::awaitable<void> on_configure(forge::config::core::component_view) override {
      state_->store_live.store(true);
      co_return;
   }
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override {
      state_->store_live.store(true);
      co_return;
   }
   boost::asio::awaitable<void> on_startup() override { co_return; }
   boost::asio::awaitable<void> on_shutdown() override {
      ++state_->store_cleanups;
      state_->store_live.store(false);
      co_return;
   }

 private:
   std::shared_ptr<cancellation_state> state_;
};

class cancellation_dependent final : public forge::app::plugin {
 public:
   explicit cancellation_dependent(std::shared_ptr<cancellation_state> state) : state_{std::move(state)} {}
   forge::app::plugin_id id() const override {
      if (state_->throw_identity.load()) {
         throw std::runtime_error{"diagnostic identity failed"};
      }
      return {"cancel.dependent"};
   }
   std::string version() const override { return "1"; }

 protected:
   boost::asio::awaitable<void> on_configure(forge::config::core::component_view) override {
      state_->partial_resource.store(true);
      if (state_->pause_enabled && state_->target == cancelled_hook::configure) {
         co_await state_->pause();
      }
   }
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override {
      state_->partial_resource.store(true);
      if (state_->pause_enabled && state_->target == cancelled_hook::initialize) {
         co_await state_->pause();
      }
   }
   boost::asio::awaitable<void> on_startup() override {
      if (state_->pause_enabled && state_->target == cancelled_hook::startup) {
         co_await state_->pause();
      }
   }
   boost::asio::awaitable<void> on_shutdown() override {
      const auto attempt = ++state_->dependent_cleanups;
      if (!state_->store_live.load()) {
         state_->cleanup_lost_dependency.store(true);
      }
      if (state_->scheduler->snapshot().stopped) {
         state_->cleanup_lost_scheduler.store(true);
      }
      if (state_->fail_first_cleanup && attempt == 1) {
         throw std::runtime_error{"dependent cleanup failed"};
      }
      auto work = state_->scheduler->submit(forge::asio::task::awaitable{
         .name = "application cancellation cleanup",
         .work = [state = state_](forge::asio::task::context&) -> boost::asio::awaitable<void> {
            ++state->cleanup_work;
            co_return;
         },
      });
      if (!work.accepted()) {
         state_->cleanup_lost_scheduler.store(true);
         throw std::runtime_error{"cleanup scheduler rejected work"};
      }
      co_await work.wait();
      state_->partial_resource.store(false);
   }

 private:
   std::shared_ptr<cancellation_state> state_;
};

class cancellation_application final : public forge::app::application_shell {
 public:
   explicit cancellation_application(std::shared_ptr<cancellation_state> state)
       : application_shell{forge::app::application_shell_options{.runtime = {.worker_threads = 4}}},
         state_{std::move(state)} {
      state_->scheduler = &scheduler();
   }

   boost::asio::awaitable<void> operation(cancelled_hook target) {
      switch (target) {
      case cancelled_hook::configure: return async_configure(forge::config::core::document{});
      case cancelled_hook::initialize: return initialize();
      case cancelled_hook::startup: return startup();
      }
      throw std::logic_error{"invalid cancellation test hook"};
   }

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin({
         .id = {"cancel.store"},
         .factory = [state = state_] { return std::make_unique<cancellation_store>(state); },
      });
      registry.register_plugin({
         .id = {"cancel.dependent"},
         .dependencies = {{"cancel.store"}},
         .factory = [state = state_] { return std::make_unique<cancellation_dependent>(state); },
      });
   }

 private:
   std::shared_ptr<cancellation_state> state_;
};

std::vector<std::unique_ptr<forge::app::plugin>> cancellation_plugins(const std::shared_ptr<cancellation_state>& state) {
   auto plugins = std::vector<std::unique_ptr<forge::app::plugin>>{};
   plugins.push_back(std::make_unique<cancellation_store>(state));
   plugins.push_back(std::make_unique<cancellation_dependent>(state));
   return plugins;
}

struct cancellation_runtime {
   explicit cancellation_runtime(std::shared_ptr<cancellation_state> state)
       : application{context, cancellation_plugins(state)} {
      state->scheduler = &scheduler_value;
   }

   forge::asio::runtime& runtime() { return runtime_value; }
   forge::asio::task::scheduler& scheduler() { return scheduler_value; }
   forge::app::lifecycle_phase state() const { return application.state(); }
   forge::app::lifecycle_observer lifecycle() const { return application.lifecycle(); }
   boost::asio::awaitable<void> shutdown() { return application.shutdown(); }
   boost::asio::awaitable<void> startup() { return application.startup(); }
   boost::asio::awaitable<void> operation(cancelled_hook target) {
      switch (target) {
      case cancelled_hook::configure: return application.configure(document);
      case cancelled_hook::initialize: return application.initialize();
      case cancelled_hook::startup: return application.startup();
      }
      throw std::logic_error{"invalid cancellation test hook"};
   }

   forge::asio::runtime runtime_value{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::task::scheduler scheduler_value{runtime_value};
   forge::app::event_journal events;
   forge::app::plugin_context context{scheduler_value, events};
   forge::config::core::document document;
   forge::app::application_runtime application;
};

boost::asio::awaitable<std::exception_ptr> capture_error(boost::asio::awaitable<void> operation) {
   try {
      co_await std::move(operation);
   } catch (...) {
      co_return std::current_exception();
   }
   co_return std::exception_ptr{};
}

template <typename Owner, typename T>
T ready(Owner& owner, cancellation_state& state, std::future<T>& future) {
   const auto status = future.wait_for(result_timeout);
   if (status != std::future_status::ready) {
      state.release.notify();
      // Test cleanup tasks are finite. Drain them while workers still run,
      // then join the runtime here, never from an owner-held coroutine frame.
      owner.scheduler().stop();
      owner.runtime().stop();
   }
   BOOST_REQUIRE(status == std::future_status::ready);
   return future.get();
}

bool was_cancelled(const std::exception_ptr& failure) {
   if (!failure) {
      return false;
   }
   try {
      std::rethrow_exception(failure);
   } catch (const boost::system::system_error& error) {
      return error.code() == boost::asio::error::operation_aborted;
   } catch (...) {
      return false;
   }
}

bool has_nested_cleanup_failure(const std::exception_ptr& failure) {
   if (!failure) {
      return false;
   }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::app::exceptions::shutdown_failed& error) {
      try {
         std::rethrow_if_nested(error);
      } catch (const std::runtime_error& nested) {
         return std::string{nested.what()} == "dependent cleanup failed";
      } catch (...) {
      }
   } catch (...) {
   }
   return false;
}

template <typename Owner> void cancel_hook(cancelled_hook target, bool fail_cleanup = false) {
   auto state = std::make_shared<cancellation_state>(target, fail_cleanup);
   auto owner = std::make_unique<Owner>(state);
   auto serial = boost::asio::make_strand(owner->runtime().context());
   auto signal = std::make_shared<boost::asio::cancellation_signal>();
   // Retain the sole owner on the test thread and join workers before releasing
   // it, including fatal-assertion unwinding while a hook is still in flight.
   auto teardown = boost::scope::scope_exit{[&] {
      state->release.notify();
      owner->scheduler().stop();
      owner->runtime().stop();
   }};
   const auto epoch = state->entered.epoch();
   auto entered = boost::asio::co_spawn(owner->runtime().context(), state->entered.async_wait_until(
      epoch, std::chrono::steady_clock::now() + hook_timeout), boost::asio::use_future);
   auto running = boost::asio::co_spawn(serial, capture_error(owner->operation(target)),
      boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_future));
   BOOST_REQUIRE(ready(*owner, *state, entered) != epoch);
   BOOST_REQUIRE(state->partial_resource.load());
   boost::asio::post(serial, [signal] { signal->emit(boost::asio::cancellation_type::terminal); });
   const auto failure = ready(*owner, *state, running);
   BOOST_REQUIRE(failure);
   BOOST_CHECK_EQUAL(state->dependent_cleanups.load(), 1U);
   BOOST_CHECK(!state->cleanup_lost_dependency.load());
   BOOST_CHECK(!state->cleanup_lost_scheduler.load());

   if (fail_cleanup) {
      BOOST_CHECK(has_nested_cleanup_failure(failure));
      BOOST_CHECK(owner->state() == forge::app::lifecycle_phase::stopping);
      BOOST_CHECK(state->store_live.load());
      BOOST_CHECK(state->partial_resource.load());
      BOOST_CHECK_EQUAL(state->store_cleanups.load(), 0U);
      BOOST_CHECK(!owner->scheduler().snapshot().stopped);
      const auto snapshot = owner->lifecycle().snapshot();
      BOOST_REQUIRE(snapshot.failure);
      BOOST_CHECK(snapshot.failure->operation == forge::app::lifecycle_operation::shutdown);
   } else {
      BOOST_CHECK(was_cancelled(failure));
      BOOST_CHECK(owner->state() == forge::app::lifecycle_phase::stopped);
      BOOST_CHECK_EQUAL(state->store_cleanups.load(), 1U);
      BOOST_CHECK(!state->partial_resource.load());
   }

   auto cleanup = boost::asio::co_spawn(serial, capture_error(owner->shutdown()), boost::asio::use_future);
   BOOST_CHECK(!ready(*owner, *state, cleanup));
   BOOST_CHECK(owner->state() == forge::app::lifecycle_phase::stopped);
   BOOST_CHECK_EQUAL(state->dependent_cleanups.load(), fail_cleanup ? 2U : 1U);
   BOOST_CHECK_EQUAL(state->store_cleanups.load(), 1U);
   BOOST_CHECK_EQUAL(state->cleanup_work.load(), 1U);
   BOOST_CHECK(!state->store_live.load());
   BOOST_CHECK(!state->partial_resource.load());
   BOOST_CHECK(!state->cleanup_lost_dependency.load());
   BOOST_CHECK(!state->cleanup_lost_scheduler.load());
}

template <typename Owner> void throwing_identity_does_not_veto_shutdown() {
   auto state = std::make_shared<cancellation_state>(cancelled_hook::configure);
   state->pause_enabled = false;
   auto owner = std::make_unique<Owner>(state);
   auto teardown = boost::scope::scope_exit{[&] {
      state->release.notify();
      owner->scheduler().stop();
      owner->runtime().stop();
   }};
   auto start = boost::asio::co_spawn(owner->runtime().context(), capture_error(owner->startup()),
                                    boost::asio::use_future);
   BOOST_REQUIRE(!ready(*owner, *state, start));
   state->throw_identity.store(true);
   auto cleanup = boost::asio::co_spawn(owner->runtime().context(), capture_error(owner->shutdown()),
                                      boost::asio::use_future);
   BOOST_CHECK(!ready(*owner, *state, cleanup));
   BOOST_CHECK(owner->state() == forge::app::lifecycle_phase::stopped);
   BOOST_CHECK_EQUAL(state->dependent_cleanups.load(), 1U);
   BOOST_CHECK_EQUAL(state->store_cleanups.load(), 1U);
   BOOST_CHECK_EQUAL(state->cleanup_work.load(), 1U);
}

} // namespace

BOOST_AUTO_TEST_CASE(application_shell_cancelled_configure_runs_setup_cleanup) {
   cancel_hook<cancellation_application>(cancelled_hook::configure);
}
BOOST_AUTO_TEST_CASE(application_shell_cancelled_initialize_cleans_partial_plugin) {
   cancel_hook<cancellation_application>(cancelled_hook::initialize);
}
BOOST_AUTO_TEST_CASE(application_shell_cancelled_startup_preserves_cancellation_after_cleanup) {
   cancel_hook<cancellation_application>(cancelled_hook::startup);
}
BOOST_AUTO_TEST_CASE(application_runtime_cancelled_configure_runs_setup_cleanup) {
   cancel_hook<cancellation_runtime>(cancelled_hook::configure);
}
BOOST_AUTO_TEST_CASE(application_runtime_cancelled_initialize_cleans_partial_plugin) {
   cancel_hook<cancellation_runtime>(cancelled_hook::initialize);
}
BOOST_AUTO_TEST_CASE(application_runtime_cancelled_startup_preserves_cancellation_after_cleanup) {
   cancel_hook<cancellation_runtime>(cancelled_hook::startup);
}
BOOST_AUTO_TEST_CASE(application_shell_cancelled_hooks_retain_dependencies_after_cleanup_failure) {
   for (const auto target : {cancelled_hook::configure, cancelled_hook::initialize, cancelled_hook::startup}) {
      cancel_hook<cancellation_application>(target, true);
   }
}
BOOST_AUTO_TEST_CASE(application_runtime_cancelled_hooks_retain_dependencies_after_cleanup_failure) {
   for (const auto target : {cancelled_hook::configure, cancelled_hook::initialize, cancelled_hook::startup}) {
      cancel_hook<cancellation_runtime>(target, true);
   }
}
BOOST_AUTO_TEST_CASE(application_shell_throwing_identity_does_not_veto_shutdown) {
   throwing_identity_does_not_veto_shutdown<cancellation_application>();
}
BOOST_AUTO_TEST_CASE(application_runtime_throwing_identity_does_not_veto_shutdown) {
   throwing_identity_does_not_veto_shutdown<cancellation_runtime>();
}
