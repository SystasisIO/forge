#include <boost/test/unit_test.hpp>

#include "readme_example.hxx"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

import forge.app.diagnostics;
import forge.app.events;
import forge.app.plugin_lifecycle;
import forge.app.runner;
import forge.asio.blocking;
import forge.config.core.document;

namespace {

forge::app::daemon_context example_context() {
   auto context = forge::app::daemon_context{};
   context.name = "service";
   context.environment = forge::app::application_environment{
      std::filesystem::current_path() / "readme-example-data", "example-test"};
   context.shell.name = context.name;
   context.shell.environment = context.environment;
   context.shell.runtime.worker_threads = 1;
   return context;
}

forge::app::run_options immediate_stop() {
   return {
      .handle_sigint = false,
      .handle_sigterm = false,
      .shutdown_timeout = std::chrono::seconds{2},
      .wait_for_stop = [](forge::app::application_shell&) -> boost::asio::awaitable<void> { co_return; },
   };
}

void check_stopped(forge::app::application_shell& app) {
   BOOST_CHECK(app.state() == forge::app::lifecycle_phase::stopped);
   BOOST_CHECK(!app.services().try_get<service_example::service_settings>());
   const auto snapshot = app.diagnostics().snapshot(app.events());
   BOOST_REQUIRE_EQUAL(snapshot.plugins.size(), 1U);
   BOOST_CHECK(snapshot.plugins.front().lifecycle.phase == forge::app::lifecycle_phase::stopped);
}

} // namespace

BOOST_AUTO_TEST_SUITE(app_readme_example)

BOOST_AUTO_TEST_CASE(builder_runner_preserves_config_environment_and_cleanup) {
   const auto context = example_context();
   auto app = service_example::make_application(context);
   auto document = forge::config::core::document{};
   document.set("service.batch-size", 48U);
   document.set("status.batch-limit", 4U);
   auto options = immediate_stop();
   options.wait_for_stop = [&context](forge::app::application_shell& shell) -> boost::asio::awaitable<void> {
      BOOST_CHECK(shell.state() == forge::app::lifecycle_phase::started);
      const auto settings = shell.services().get<service_example::service_settings>();
      BOOST_CHECK_EQUAL(settings->batch_size, 4U);
      BOOST_CHECK(settings->data_path == context.environment.resolve_data_path("service"));
      BOOST_CHECK_EQUAL(settings->profile, "example-test");
      BOOST_CHECK(shell.environment().data_dir() == context.environment.data_dir());
      const auto status = shell.apis().get<service_example::status_api>(service_example::status_api::ref());
      BOOST_CHECK_EQUAL(status->batch_limit(), 4U);
      const auto diagnostics = shell.diagnostics().snapshot(shell.events());
      BOOST_REQUIRE_EQUAL(diagnostics.plugins.size(), 1U);
      BOOST_CHECK(diagnostics.plugins.front().lifecycle.phase == forge::app::lifecycle_phase::started);
      co_return;
   };
   BOOST_CHECK_EQUAL(forge::app::run_application(*app, document, std::move(options)), 0);
   check_stopped(*app);
}

BOOST_AUTO_TEST_CASE(runner_cleans_up_before_propagating_wait_failure) {
   auto app = service_example::make_application(example_context());
   auto options = immediate_stop();
   options.wait_for_stop = [](forge::app::application_shell&) -> boost::asio::awaitable<void> {
      throw std::runtime_error{"synthetic stop-wait failure"};
      co_return;
   };
   BOOST_CHECK_THROW(forge::app::run_application(*app, {}, std::move(options)), std::runtime_error);
   check_stopped(*app);
}

BOOST_AUTO_TEST_CASE(async_configuration_runs_on_the_single_runtime_worker) {
   auto app = service_example::make_application(example_context());
   auto lifecycle = [](forge::app::application_shell& shell) -> boost::asio::awaitable<void> {
      auto document = forge::config::core::document{};
      document.set("service.batch-size", 8U);
      co_await shell.async_configure(std::move(document));
      co_await shell.startup();
      BOOST_CHECK_EQUAL(shell.services().get<service_example::service_settings>()->batch_size, 8U);
      shell.request_stop();
      co_await shell.shutdown();
   };
   forge::asio::blocking::run(app->runtime(), lifecycle(*app));
   check_stopped(*app);
}

BOOST_AUTO_TEST_CASE(selection_disables_the_descriptor_without_plugin_side_effects) {
   auto app = service_example::make_application(example_context());
   auto document = forge::config::core::document{};
   document.set("plugins.status.enabled", false);
   auto options = immediate_stop();
   options.wait_for_stop = [](forge::app::application_shell&) -> boost::asio::awaitable<void> {
      BOOST_FAIL("missing required local API must fail before foreground wait");
      co_return;
   };
   BOOST_CHECK_THROW(forge::app::run_application(*app, document, std::move(options)),
                     forge::api::core::exceptions::protocol_error);
   BOOST_CHECK(app->state() == forge::app::lifecycle_phase::stopped);
   BOOST_CHECK(app->diagnostics().snapshot(app->events()).plugins.empty());
}

BOOST_AUTO_TEST_CASE(complete_daemon_uses_the_same_factory_and_cli_descriptors) {
   auto options = service_example::daemon_options();
   options.shutdown_logging = false;
   options.read_yaml = false;
   options.read_dotenv = false;
   options.read_process_env = false;
   options.run = immediate_stop();
   auto waited = false;
   auto settings = std::weak_ptr<service_example::service_settings>{};
   auto observer = std::optional<forge::app::lifecycle_observer>{};
   options.run.wait_for_stop = [&](forge::app::application_shell& shell) -> boost::asio::awaitable<void> {
      waited = true;
      observer = shell.lifecycle();
      const auto service = shell.services().get<service_example::service_settings>();
      settings = service;
      BOOST_CHECK_EQUAL(service->batch_size, 7U);
      BOOST_CHECK_EQUAL(service->profile, "daemon-test");
      BOOST_CHECK(service->data_path == shell.environment().resolve_data_path("service"));
      const auto diagnostics = shell.diagnostics().snapshot(shell.events());
      BOOST_REQUIRE_EQUAL(diagnostics.plugins.size(), 1U);
      BOOST_CHECK(diagnostics.plugins.front().lifecycle.phase == forge::app::lifecycle_phase::started);
      co_return;
   };
   auto args = std::vector<std::string>{
      "service", "--data-dir", "readme-example-daemon", "--profile", "daemon-test",
      "--service.batch-size", "7",
   };
   auto argv = std::vector<char*>{};
   for (auto& arg : args) {
      argv.push_back(arg.data());
   }
   BOOST_REQUIRE_EQUAL(forge::app::run_daemon(service_example::make_application,
      static_cast<int>(argv.size()), argv.data(), std::move(options)), 0);
   BOOST_CHECK(waited);
   BOOST_CHECK(settings.expired());
   BOOST_REQUIRE(observer.has_value());
   BOOST_CHECK(observer->snapshot().phase == forge::app::lifecycle_phase::stopped);
   BOOST_CHECK(!observer->snapshot().owner_alive);
}

BOOST_AUTO_TEST_SUITE_END()
