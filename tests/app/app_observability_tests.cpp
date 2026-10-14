#include <boost/test/unit_test.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <boost/asio/error.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.app.application;
import forge.app.application_shell;
import forge.app.daemon;
import forge.app.events;
import forge.app.exceptions;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.app.runner;
import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.schema.value_kind;
import forge.log.logger;

namespace {

constexpr auto wait_limit = std::chrono::seconds{3};

std::string field(const forge::log_record& record, const char* key) {
   for (const auto& value : record.fields) {
      if (value.key == key) {
         return value.value;
      }
   }
   return {};
}

std::size_t count_event(const std::vector<forge::log_record>& records, const char* event,
                       const char* operation = nullptr) {
   return std::count_if(records.begin(), records.end(), [&](const auto& record) {
      return field(record, "event") == event &&
             (operation == nullptr || field(record, "operation") == operation);
   });
}

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      const auto lock = std::scoped_lock{mutex};
      records.push_back(record);
   }
   std::vector<forge::log_record> snapshot() {
      const auto lock = std::scoped_lock{mutex};
      return records;
   }

 private:
   std::mutex mutex;
   std::vector<forge::log_record> records;
};

class failing_sink final : public forge::sink {
 public:
   void log(const forge::log_record&) override {
      ++calls;
      throw std::runtime_error{"sink-secret-marker"};
   }
   std::atomic_int calls = 0;
};

class reentrant_sink final : public forge::sink {
 public:
   void log(const forge::log_record&) override {
      if (observe) {
         observe();
      }
      ++calls;
   }
   std::function<void()> observe;
   std::atomic_int calls = 0;
};

struct log_capture {
   explicit log_capture(bool fail = false, bool reenter = false) {
      for (const auto* name : {"forge.app.application", "forge.app.plugin", "forge.app.runner", "forge.app.daemon"}) {
         auto route = forge::logger::get(name);
         levels.push_back(route.get_log_level());
         route.set_log_level(forge::log_level::debug);
         if (fail) {
            route.add_sink(failing);
         }
         if (reenter) {
            route.add_sink(reentrant);
         }
         route.add_sink(captured);
         routes.push_back(std::move(route));
      }
   }
   ~log_capture() {
      for (auto index = std::size_t{}; index < routes.size(); ++index) {
         routes[index].remove_sink(captured);
         routes[index].remove_sink(failing);
         routes[index].remove_sink(reentrant);
         routes[index].set_log_level(levels[index]);
      }
   }
   std::shared_ptr<capture_sink> captured = std::make_shared<capture_sink>();
   std::shared_ptr<failing_sink> failing = std::make_shared<failing_sink>();
   std::shared_ptr<reentrant_sink> reentrant = std::make_shared<reentrant_sink>();
   std::vector<forge::logger> routes;
   std::vector<forge::log_level> levels;
};

struct gate {
   std::atomic_bool blocked = false;
   forge::asio::notification entered;
   forge::asio::notification release;

   boost::asio::awaitable<void> wait() {
      const auto before = release.epoch();
      entered.notify();
      if (blocked.load() && co_await release.async_wait_until(
          before, std::chrono::steady_clock::now() + wait_limit) == before) {
         FORGE_THROW_EXCEPTION(forge::app::exceptions::invalid_state, "test gate expired");
      }
   }
};

struct observed_state {
   gate startup;
   gate cleanup;
   std::atomic_int startups = 0;
   std::atomic_int cleanups = 0;
   std::atomic_int cleanup_failures = 0;
   std::atomic_bool fail_startup = false;
   std::atomic_bool fail_invalid_startup = false;
   std::atomic_bool fail_configuration = false;
   std::atomic_bool range_configuration_error = false;
   std::atomic_bool configured_secret = false;
};

class observed_plugin final : public forge::app::plugin {
 public:
   explicit observed_plugin(std::shared_ptr<observed_state> state) : state_{std::move(state)} {}
   forge::app::plugin_id id() const override { return {"observability"}; }
   std::string version() const override { return "1"; }

 protected:
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override { co_return; }
   boost::asio::awaitable<void> on_startup() override {
      ++state_->startups;
      co_await state_->startup.wait();
      if (state_->fail_invalid_startup.load()) {
         FORGE_THROW_EXCEPTION(forge::app::exceptions::invalid_state, "exception-secret-marker",
                               forge::exceptions::secret("credential", "context-secret-marker"));
      }
      if (state_->fail_startup.load()) {
         FORGE_THROW_EXCEPTION(forge::app::exceptions::startup_failed, "exception-secret-marker",
                               forge::exceptions::secret("credential", "context-secret-marker"));
      }
   }
   boost::asio::awaitable<void> on_shutdown() override {
      const auto attempt = ++state_->cleanups;
      co_await state_->cleanup.wait();
      if (attempt <= state_->cleanup_failures.load()) {
         // Even a cancellation-shaped cleanup error is a real cleanup failure.
         throw boost::system::system_error{boost::asio::error::operation_aborted};
      }
   }

 private:
   std::shared_ptr<observed_state> state_;
};

class observed_application final : public forge::app::application_shell {
 public:
   explicit observed_application(std::shared_ptr<observed_state> state)
       : application_shell{forge::app::application_shell_options{.runtime = {.worker_threads = 4}}},
         state_{std::move(state)} {}

 protected:
   void on_describe_config(forge::config::core::component_registry& registry) const override {
      registry.add({.section = "service", .fields = {{.name = "token", .has_default = true,
                                                     .default_value = std::string{}, .secret = true}}});
   }
   boost::asio::awaitable<void> on_configure(forge::app::configure_context& context) override {
      if (state_->fail_configuration.load()) {
         throw std::invalid_argument{"callback-secret-marker"};
      }
      if (state_->range_configuration_error.load()) {
         throw std::invalid_argument{"value is above the allowed maximum"};
      }
      state_->configured_secret.store(context.view("service").get_or<std::string>("token", {}) == "config-secret-marker");
      co_return;
   }
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin({.id = {"observability"},
                    .factory = [state = state_] { return std::make_unique<observed_plugin>(state); }});
   }

 private:
   std::shared_ptr<observed_state> state_;
};

struct runtime_guard {
   ~runtime_guard() { app->runtime().stop(); }
   forge::app::application_shell* app;
};

void ready(std::future<void>& result) {
   BOOST_REQUIRE(result.wait_for(wait_limit) == std::future_status::ready);
   result.get();
}

void entered(forge::app::application_shell& app, gate& value, std::uint64_t before) {
   auto result = boost::asio::co_spawn(
       app.runtime().context(), value.entered.async_wait_until(before, std::chrono::steady_clock::now() + wait_limit),
       boost::asio::use_future);
   BOOST_REQUIRE(result.wait_for(wait_limit) == std::future_status::ready);
   BOOST_REQUIRE(result.get() != before);
}

boost::asio::awaitable<void> barrier() { co_return; }

forge::app::run_options foreground_options() {
   auto options = forge::app::run_options{};
   options.handle_sigint = false;
   options.handle_sigterm = false;
   return options;
}

void assert_no_secrets(const std::vector<forge::log_record>& records) {
   for (const auto& record : records) {
      auto text = record.message + record.exception_chain;
      for (const auto& value : record.fields) {
         text += value.key + value.value;
      }
      for (const auto* secret : {"config-secret-marker", "exception-secret-marker", "context-secret-marker",
                                "sink-secret-marker", "path-secret-marker", "profile-secret-marker",
                                "callback-secret-marker"}) {
         BOOST_CHECK(text.find(secret) == std::string::npos);
      }
   }
}

} // namespace

BOOST_AUTO_TEST_CASE(app_observability_normal_run_has_single_completion_and_orderly_stop_not_error) {
   auto logs = log_capture{};
   auto state = std::make_shared<observed_state>();
   auto app = observed_application{state};
   auto guard = runtime_guard{&app};
   auto document = forge::config::core::document{};
   document.set("service.token", "config-secret-marker");
   const auto before = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
   BOOST_CHECK_EQUAL(forge::app::run_application(app, document, foreground_options()), 0);
   const auto after = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
   BOOST_CHECK(state->configured_secret.load());
   BOOST_CHECK_EQUAL(state->startups.load(), 1);
   BOOST_CHECK_EQUAL(state->cleanups.load(), 1);
   const auto records = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(records, "runner.start"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "runner.exit"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.ready"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.cleanup.complete"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.stop.requested"), 1U);
   for (const auto& record : records) {
      BOOST_CHECK(record.timestamp >= before);
      BOOST_CHECK(record.timestamp <= after);
      BOOST_CHECK(!record.thread_id.empty());
      BOOST_CHECK(record.level.value != forge::log_level::error);
      BOOST_CHECK(record.level.value != forge::log_level::warn);
      if ((field(record, "event") == "app.operation.complete" ||
           field(record, "event") == "plugin.operation.complete") &&
          (field(record, "operation") == "provide" || field(record, "operation") == "after_initialize")) {
         BOOST_CHECK(record.level.value == forge::log_level::debug);
      }
   }
   assert_no_secrets(records);
}

BOOST_AUTO_TEST_CASE(app_observability_joined_startup_shutdown_do_not_duplicate_completion) {
   auto logs = log_capture{};
   auto state = std::make_shared<observed_state>();
   state->startup.blocked.store(true);
   state->cleanup.blocked.store(true);
   auto app = observed_application{state};
   auto guard = runtime_guard{&app};
   auto serial = boost::asio::make_strand(app.runtime().context());
   const auto before_start = state->startup.entered.epoch();
   auto first = boost::asio::co_spawn(serial, app.startup(), boost::asio::use_future);
   entered(app, state->startup, before_start);
   auto joined = boost::asio::co_spawn(serial, app.startup(), boost::asio::use_future);
   auto ordered = boost::asio::co_spawn(serial, barrier(), boost::asio::use_future);
   ready(ordered);
   state->startup.release.notify();
   ready(first);
   ready(joined);

   const auto before_cleanup = state->cleanup.entered.epoch();
   auto cleanup = boost::asio::co_spawn(serial, app.shutdown(), boost::asio::use_future);
   entered(app, state->cleanup, before_cleanup);
   auto cleanup_joined = boost::asio::co_spawn(serial, app.shutdown(), boost::asio::use_future);
   auto cleanup_ordered = boost::asio::co_spawn(serial, barrier(), boost::asio::use_future);
   ready(cleanup_ordered);
   state->cleanup.release.notify();
   ready(cleanup);
   ready(cleanup_joined);
   const auto records = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(records, "app.ready"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.cleanup.complete"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.operation.complete", "provide"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.operation.complete", "after_initialize"), 1U);
   BOOST_CHECK_EQUAL(state->cleanups.load(), 1);
}

BOOST_AUTO_TEST_CASE(app_observability_startup_error_redacts_config_exception_context_and_preserves_failure) {
   auto logs = log_capture{};
   auto state = std::make_shared<observed_state>();
   state->fail_startup.store(true);
   auto app = observed_application{state};
   auto guard = runtime_guard{&app};
   auto document = forge::config::core::document{};
   document.set("service.token", "config-secret-marker");
   BOOST_CHECK_THROW(forge::app::run_application(app, document, foreground_options()), forge::app::exceptions::startup_failed);
   const auto records = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(records, "app.ready"), 0U);
   BOOST_CHECK_EQUAL(count_event(records, "app.operation.failed", "startup"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "runner.failed", "startup"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.cleanup.complete"), 1U);
   BOOST_CHECK(app.state() == forge::app::lifecycle_phase::stopped);
   assert_no_secrets(records);
}

BOOST_AUTO_TEST_CASE(app_observability_hook_invalid_state_stays_error_after_rollback_stop) {
   auto logs = log_capture{};
   auto state = std::make_shared<observed_state>();
   state->fail_invalid_startup.store(true);
   auto app = observed_application{state};
   auto guard = runtime_guard{&app};
   BOOST_CHECK_THROW(forge::app::run_application(app, {}, foreground_options()), forge::app::exceptions::invalid_state);
   BOOST_CHECK(app.state() == forge::app::lifecycle_phase::stopped);
   const auto records = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(records, "app.operation.failed", "startup"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "runner.failed", "startup"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "runner.canceled", "startup"), 0U);
   for (const auto& record : records) {
      if (field(record, "event") == "runner.failed" || field(record, "event") == "app.operation.failed") {
         BOOST_CHECK(record.level.value == forge::log_level::error);
      }
   }
   assert_no_secrets(records);
}

BOOST_AUTO_TEST_CASE(app_observability_late_start_after_stop_never_logs_ready_or_normal_stop_error) {
   auto logs = log_capture{};
   auto state = std::make_shared<observed_state>();
   state->startup.blocked.store(true);
   auto app = observed_application{state};
   auto guard = runtime_guard{&app};
   const auto before = state->startup.entered.epoch();
   auto starting = boost::asio::co_spawn(app.runtime().context(), app.startup(), boost::asio::use_future);
   entered(app, state->startup, before);
   app.request_stop();
   state->startup.release.notify();
   BOOST_REQUIRE(starting.wait_for(wait_limit) == std::future_status::ready);
   BOOST_CHECK_THROW(starting.get(), forge::app::exceptions::invalid_state);
   const auto records = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(records, "app.ready"), 0U);
   BOOST_CHECK_EQUAL(count_event(records, "app.operation.canceled", "startup"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.cleanup.complete"), 1U);
   for (const auto& record : records) {
      if (field(record, "event") == "app.stop.requested") {
         BOOST_CHECK(record.level.value == forge::log_level::info);
      }
      BOOST_CHECK(record.level.value != forge::log_level::error);
   }
}

BOOST_AUTO_TEST_CASE(app_observability_cleanup_cancellation_is_error_and_retry_keeps_real_state) {
   auto logs = log_capture{};
   auto state = std::make_shared<observed_state>();
   state->cleanup_failures.store(1);
   auto app = observed_application{state};
   auto guard = runtime_guard{&app};
   forge::asio::blocking::run(app.runtime(), app.startup());
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.shutdown()), boost::system::system_error);
   BOOST_CHECK(app.state() == forge::app::lifecycle_phase::stopping);
   const auto failed = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(failed, "app.cleanup.complete"), 0U);
   BOOST_CHECK_EQUAL(count_event(failed, "app.operation.failed", "shutdown"), 1U);
   for (const auto& record : failed) {
      if (field(record, "event") == "app.operation.failed" && field(record, "operation") == "shutdown") {
         BOOST_CHECK(record.level.value == forge::log_level::error);
      }
   }
   forge::asio::blocking::run(app.runtime(), app.shutdown());
   const auto final = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(final, "app.cleanup.retry"), 1U);
   BOOST_CHECK_EQUAL(count_event(final, "app.cleanup.complete"), 1U);
}

BOOST_AUTO_TEST_CASE(app_observability_throwing_and_reentrant_sinks_cannot_change_run_result) {
   auto logs = log_capture{true, true};
   auto state = std::make_shared<observed_state>();
   auto app = observed_application{state};
   auto guard = runtime_guard{&app};
   logs.reentrant->observe = [&] { static_cast<void>(app.lifecycle().snapshot()); };
   BOOST_CHECK_EQUAL(forge::app::run_application(app, {}, foreground_options()), 0);
   logs.reentrant->observe = {};
   BOOST_CHECK_GT(logs.failing->calls.load(), 0);
   BOOST_CHECK_GT(logs.reentrant->calls.load(), 0);
   BOOST_CHECK_EQUAL(state->cleanups.load(), 1);
   assert_no_secrets(logs.captured->snapshot());
}

BOOST_AUTO_TEST_CASE(app_observability_standalone_runtime_uses_same_taxonomy_without_shell_duplicates) {
   auto logs = log_capture{};
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto journal = forge::app::event_journal{};
   auto context = forge::app::plugin_context{scheduler, journal};
   auto state = std::make_shared<observed_state>();
   auto plugins = std::vector<std::unique_ptr<forge::app::plugin>>{};
   plugins.push_back(std::make_unique<observed_plugin>(state));
   auto application = forge::app::application_runtime{context, std::move(plugins)};
   forge::asio::blocking::run(runtime, application.startup());
   forge::asio::blocking::run(runtime, application.shutdown());
   const auto records = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(records, "app.ready"), 1U);
   BOOST_CHECK_EQUAL(count_event(records, "app.cleanup.complete"), 1U);
}

BOOST_AUTO_TEST_CASE(app_observability_daemon_dry_modes_have_no_ready_or_service_stop_info) {
   auto logs = log_capture{};
   auto output = std::ostringstream{};
   auto* previous = std::cout.rdbuf(output.rdbuf());
   struct restore_output {
      ~restore_output() { std::cout.rdbuf(previous); }
      std::streambuf* previous;
   } restore{previous};
   for (const auto* mode : {"--help", "--check-config"}) {
      auto state = std::make_shared<observed_state>();
      auto options = forge::app::daemon_options{.name = "observability", .read_yaml = false,
                                               .read_dotenv = false, .read_process_env = false};
      auto args = std::vector<std::string>{"observability", mode, "--profile=profile-secret-marker",
                                           "--data-dir=/path-secret-marker"};
      auto argv = std::vector<char*>{};
      for (auto& argument : args) { argv.push_back(argument.data()); }
      BOOST_CHECK_EQUAL(forge::app::run_daemon(
          [state](const auto&) { return std::make_unique<observed_application>(state); },
          static_cast<int>(argv.size()), argv.data(), options), 0);
      BOOST_CHECK_EQUAL(state->startups.load(), 0);
   }
   const auto records = logs.captured->snapshot();
   BOOST_CHECK_EQUAL(count_event(records, "app.ready"), 0U);
   for (const auto& record : records) {
      if (field(record, "event") == "app.stop.requested" || field(record, "event") == "plugin.stop.requested") {
         BOOST_CHECK(record.level.value == forge::log_level::debug);
      }
   }
   assert_no_secrets(records);
}

BOOST_AUTO_TEST_CASE(app_observability_cli_known_validation_reason_survives_without_echoing_callback_secret) {
   auto logs = log_capture{};
   auto errors = std::ostringstream{};
   auto* previous = std::cerr.rdbuf(errors.rdbuf());
   struct restore_errors {
      ~restore_errors() { std::cerr.rdbuf(previous); }
      std::streambuf* previous;
   } restore{previous};
   for (const auto known_reason : {true, false}) {
      auto state = std::make_shared<observed_state>();
      state->range_configuration_error.store(known_reason);
      state->fail_configuration.store(!known_reason);
      auto options = forge::app::daemon_options{.name = "observability", .read_yaml = false,
                                               .read_dotenv = false, .read_process_env = false};
      auto args = std::vector<std::string>{"observability", "--check-config", "--service.token=config-secret-marker"};
      auto argv = std::vector<char*>{};
      for (auto& argument : args) { argv.push_back(argument.data()); }
      errors.str({});
      BOOST_CHECK_EQUAL(forge::app::run_daemon(
          [state](const auto&) { return std::make_unique<observed_application>(state); },
          static_cast<int>(argv.size()), argv.data(), options), 1);
      BOOST_CHECK(errors.str().find("application.configure") != std::string::npos);
      BOOST_CHECK(errors.str().find(known_reason ? "allowed maximum" : "application configuration failed") != std::string::npos);
      BOOST_CHECK(errors.str().find("callback-secret-marker") == std::string::npos);
      BOOST_CHECK(errors.str().find("config-secret-marker") == std::string::npos);
   }
   assert_no_secrets(logs.captured->snapshot());
}
