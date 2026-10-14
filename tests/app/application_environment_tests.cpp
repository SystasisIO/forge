#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

import forge.app.application_builder;
import forge.app.application_environment;
import forge.app.application_shell;
import forge.app.daemon;
import forge.app.diagnostics;
import forge.app.exceptions;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.app.runner;
import forge.config.core.component;

namespace {

static_assert(std::is_same_v<decltype(std::declval<const forge::app::application_environment&>().data_dir()),
                             const std::filesystem::path&>);
static_assert(std::is_same_v<decltype(std::declval<const forge::app::application_environment&>().profile()),
                             const std::string&>);
static_assert(std::is_same_v<decltype(std::declval<const forge::app::plugin_context&>().environment()),
                             const forge::app::application_environment&>);
static_assert(std::is_same_v<decltype(std::declval<const forge::app::plugin_context&>().diagnostics()),
                             const forge::app::diagnostics_store*>);
static_assert(std::is_same_v<decltype(std::declval<forge::app::application_builder&>().environment(
                                forge::app::application_environment{})), forge::app::application_builder&>);

template <typename Context>
concept can_clear_diagnostics = requires(Context& context) {
   context.diagnostics()->clear_plugins();
};

static_assert(!can_clear_diagnostics<forge::app::plugin_context>);

struct working_directory_guard {
   std::filesystem::path original = std::filesystem::current_path();

   ~working_directory_guard() {
      auto error = std::error_code{};
      std::filesystem::current_path(original, error);
   }
};

struct environment_flow_state {
   forge::app::application_environment factory;
   forge::app::application_environment shell_options;
   forge::app::application_environment shell;
   forge::app::application_environment provided;
   forge::app::application_environment initialized;
   forge::app::application_environment after_initialized;
   forge::app::application_environment started;
   std::filesystem::path configured_data_dir;
   std::string configured_profile;
   std::filesystem::path plugin_path;
   bool diagnostics_available = false;
   std::vector<std::string> steps;
};

class environment_probe_plugin final : public forge::app::plugin {
 public:
   explicit environment_probe_plugin(std::shared_ptr<environment_flow_state> state) : state_{std::move(state)} {}

   forge::app::plugin_id id() const override {
      return {"environment-probe"};
   }

   std::string version() const override {
      return "1";
   }

 protected:
   boost::asio::awaitable<void> on_configure(forge::config::core::component_view) override {
      state_->steps.push_back("plugin.configure");
      co_return;
   }

   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override {
      state_->initialized = context.environment();
      state_->plugin_path = context.environment().resolve_data_path("plugin/cache/../state.db");
      state_->diagnostics_available = context.diagnostics() != nullptr;
      state_->steps.push_back("plugin.initialize");
      co_return;
   }

   boost::asio::awaitable<void> on_startup() override {
      state_->steps.push_back("plugin.startup");
      co_return;
   }

   boost::asio::awaitable<void> on_shutdown() override {
      state_->steps.push_back("plugin.shutdown");
      co_return;
   }

 private:
   std::shared_ptr<environment_flow_state> state_;
};

forge::app::plugin_descriptor environment_probe_descriptor(std::shared_ptr<environment_flow_state> state) {
   return {
      .id = {"environment-probe"},
      .factory = [state = std::move(state)] {
         return std::make_unique<environment_probe_plugin>(state);
      },
   };
}

void record_environment_configuration(forge::app::configure_context& context, environment_flow_state& state) {
   const auto daemon = context.view("daemon");
   state.configured_data_dir = daemon.get_or<std::string>("data-dir", {});
   state.configured_profile = daemon.get_or<std::string>("profile", {});
   state.steps.push_back("app.configure");
}

class environment_bootstrap_shell final : public forge::app::application_shell {
 public:
   environment_bootstrap_shell(forge::app::application_shell_options options,
                               std::shared_ptr<environment_flow_state> state)
       : forge::app::application_shell{std::move(options)}, state_{std::move(state)} {
      state_->shell = environment();
   }

 protected:
   boost::asio::awaitable<void> on_configure(forge::app::configure_context& context) override {
      record_environment_configuration(context, *state_);
      co_return;
   }

   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(environment_probe_descriptor(state_));
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      state_->provided = context.environment();
      state_->steps.push_back("app.provide");
      co_return;
   }

   boost::asio::awaitable<void> on_after_initialize(const forge::app::application_context& context) override {
      state_->after_initialized = context.environment();
      state_->steps.push_back("app.after_initialize");
      co_return;
   }

 private:
   std::shared_ptr<environment_flow_state> state_;
};

int run_environment_daemon(forge::app::daemon_factory factory, std::vector<std::string> args,
                           std::shared_ptr<environment_flow_state> state) {
   auto argv = std::vector<char*>{};
   for (auto& arg : args) {
      argv.push_back(arg.data());
   }
   auto options = forge::app::daemon_options{
      .name = "environment-test",
      .default_profile = "default-must-not-win",
      .read_yaml = false,
      .read_dotenv = false,
      .read_process_env = false,
   };
   options.run.handle_sigint = false;
   options.run.handle_sigterm = false;
   options.shutdown_logging = false;
   options.run.shutdown_timeout = std::chrono::seconds{2};
   options.run.wait_for_stop = [state = std::move(state)](forge::app::application_shell& shell)
                                 -> boost::asio::awaitable<void> {
      state->started = shell.environment();
      state->steps.push_back("runner.wait");
      co_return;
   };
   return forge::app::run_daemon(std::move(factory), static_cast<int>(argv.size()), argv.data(), std::move(options));
}

void check_environment_snapshot(const forge::app::application_environment& environment,
                                const std::filesystem::path& base, const std::string& profile) {
   BOOST_REQUIRE(environment.data_dir().native() == base.native());
   BOOST_CHECK_EQUAL(environment.profile(), profile);
   BOOST_CHECK(environment.resolve_data_path("plugin/cache/../state.db") == base / "plugin" / "state.db");
}

void check_environment_flow(const environment_flow_state& state, const std::filesystem::path& base,
                            const std::string& profile) {
   check_environment_snapshot(state.factory, base, profile);
   check_environment_snapshot(state.shell_options, base, profile);
   check_environment_snapshot(state.shell, base, profile);
   check_environment_snapshot(state.provided, base, profile);
   check_environment_snapshot(state.initialized, base, profile);
   check_environment_snapshot(state.after_initialized, base, profile);
   check_environment_snapshot(state.started, base, profile);
   BOOST_CHECK(state.configured_data_dir.native() == base.native());
   BOOST_CHECK_EQUAL(state.configured_profile, profile);
   BOOST_CHECK(state.plugin_path == base / "plugin" / "state.db");
   BOOST_CHECK(state.diagnostics_available);
   const auto expected_steps = std::vector<std::string>{
      "app.configure", "plugin.configure", "app.provide", "plugin.initialize", "app.after_initialize",
      "plugin.startup", "runner.wait", "plugin.shutdown",
   };
   BOOST_TEST(state.steps == expected_steps, boost::test_tools::per_element());
}

} // namespace

BOOST_AUTO_TEST_SUITE(application_environment_tests)

BOOST_AUTO_TEST_CASE(absolute_data_path_is_returned_unchanged) {
   const auto absolute = std::filesystem::current_path().root_path() / "forge-app-environment-tests" /
                         "unused" / ".." / "absolute";
   const auto environment = forge::app::application_environment{};
   BOOST_CHECK(environment.resolve_data_path(absolute).native() == absolute.native());
   const auto base = absolute.root_path() / "forge-app-environment-tests" / "unused" / ".." / "data";
   const auto with_base = forge::app::application_environment{base};
   BOOST_CHECK(with_base.data_dir().native() == base.lexically_normal().native());
   BOOST_CHECK(with_base.resolve_data_path(absolute).native() == absolute.native());
}

BOOST_AUTO_TEST_CASE(relative_data_path_without_base_fails_closed) {
   const auto environment = forge::app::application_environment{};
   BOOST_CHECK(environment.data_dir().empty());
   BOOST_CHECK_THROW(static_cast<void>(environment.resolve_data_path("state.db")), forge::app::exceptions::config_failed);
   BOOST_CHECK_THROW(static_cast<void>(environment.resolve_data_path({})), forge::app::exceptions::config_failed);
}

BOOST_AUTO_TEST_CASE(relative_base_is_normalized_and_frozen_before_working_directory_changes) {
   const auto guard = working_directory_guard{};
   const auto relative_base = std::filesystem::path{"forge-app-environment-tests"} / "unused" / ".." / "data";
   const auto expected_base = (guard.original / relative_base).lexically_normal();
   const auto environment = forge::app::application_environment{relative_base};
   BOOST_REQUIRE(environment.data_dir().is_absolute());
   BOOST_CHECK(environment.data_dir().native() == expected_base.native());

   const auto alternate = guard.original != guard.original.root_path() ? guard.original.root_path()
                                                                      : std::filesystem::temp_directory_path();
   BOOST_REQUIRE(alternate != guard.original);
   std::filesystem::current_path(alternate);

   BOOST_CHECK(environment.data_dir().native() == expected_base.native());
   BOOST_CHECK(environment.resolve_data_path("cache/../state.db") == expected_base / "state.db");
   const auto copy = environment;
   BOOST_CHECK(copy.resolve_data_path("state.db") == expected_base / "state.db");
}

BOOST_AUTO_TEST_CASE(profile_is_owned_and_preserved_by_copy) {
   auto profile = std::string{"custom_profile"};
   const auto environment = forge::app::application_environment{{}, profile};
   profile = "changed";
   const auto copy = environment;
   BOOST_CHECK_EQUAL(environment.profile(), "custom_profile");
   BOOST_CHECK_EQUAL(copy.profile(), "custom_profile");
   BOOST_CHECK(copy.data_dir().empty());
}

BOOST_AUTO_TEST_CASE(daemon_shell_forwards_environment_through_actual_plugin_lifecycle) {
   const auto guard = working_directory_guard{};
   const auto relative_base = std::filesystem::path{"forge-app-environment-tests/unused/../daemon"};
   const auto expected_base = (guard.original / relative_base).lexically_normal();
   const auto alternate = guard.original != guard.original.root_path() ? guard.original.root_path()
                                                                      : std::filesystem::temp_directory_path();
   BOOST_REQUIRE(alternate != guard.original);
   const auto state = std::make_shared<environment_flow_state>();
   const auto exit_code = run_environment_daemon(
      [state, alternate](const forge::app::daemon_context& context) {
         state->factory = context.environment;
         state->shell_options = context.shell.environment;
         // Change the process base after daemon bootstrap, before shell construction.
         std::filesystem::current_path(alternate);
         return std::make_unique<environment_bootstrap_shell>(context.shell, state);
      },
      {"environment-test", "--data-dir", relative_base.string(), "--profile", "daemon_profile",
       "--shutdown-timeout-ms", "2000"},
      state);
   BOOST_REQUIRE_EQUAL(exit_code, 0);
   check_environment_flow(*state, expected_base, "daemon_profile");
}

BOOST_AUTO_TEST_CASE(daemon_builder_forwards_environment_through_actual_plugin_lifecycle) {
   const auto guard = working_directory_guard{};
   const auto relative_base = std::filesystem::path{"forge-app-environment-tests/unused/../builder"};
   const auto expected_base = (guard.original / relative_base).lexically_normal();
   const auto alternate = guard.original != guard.original.root_path() ? guard.original.root_path()
                                                                      : std::filesystem::temp_directory_path();
   BOOST_REQUIRE(alternate != guard.original);
   const auto state = std::make_shared<environment_flow_state>();
   const auto exit_code = run_environment_daemon(
      [state, alternate](const forge::app::daemon_context& context) {
         state->factory = context.environment;
         state->shell_options = context.shell.environment;
         std::filesystem::current_path(alternate);
         auto builder = forge::app::application_builder{};
         builder.name(context.shell.name)
            .environment(context.environment)
            .runtime(context.shell.runtime)
            .scheduler(context.shell.scheduler)
            .plugin(environment_probe_descriptor(state))
            .configure([state](forge::app::configure_context& configure) {
               record_environment_configuration(configure, *state);
            })
            .provide([state](forge::app::application_context& provide) {
               state->provided = provide.environment();
               state->steps.push_back("app.provide");
            })
            .after_initialize([state](const forge::app::application_context& initialized) {
               state->after_initialized = initialized.environment();
               state->steps.push_back("app.after_initialize");
            });
         auto shell = std::move(builder).build();
         state->shell = shell->environment();
         return shell;
      },
      {"environment-test", "--data-dir", relative_base.string(), "--profile", "builder_profile",
       "--shutdown-timeout-ms", "2000"},
      state);
   BOOST_REQUIRE_EQUAL(exit_code, 0);
   check_environment_flow(*state, expected_base, "builder_profile");
}

BOOST_AUTO_TEST_SUITE_END()
