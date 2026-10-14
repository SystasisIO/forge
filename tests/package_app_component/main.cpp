#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <concepts>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>

import forge.app.application_environment;
import forge.app.plugin_lifecycle;
import forge.app.plugin;
import forge.app.diagnostics;
import forge.app.events;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.app.daemon;
import forge.log.logger;
import forge.log.logger_config;

template <typename T>
concept can_stop = requires(T value) { value.request_stop(); };

static_assert(!can_stop<forge::app::lifecycle_observer>);
static_assert(can_stop<forge::app::plugin_lifecycle>);
static_assert(!std::copy_constructible<forge::app::plugin_lifecycle::operation>);
static_assert(std::same_as<decltype(forge::app::daemon_options{}.shutdown_logging), bool>);
static_assert(std::same_as<decltype(forge::app::daemon_options{}.logging_shutdown_timeout), std::chrono::milliseconds>);

int main() {
   auto environment = forge::app::application_environment{
      std::filesystem::temp_directory_path() / "forge-package-app", "test"};
   if (environment.resolve_data_path("snapshots") != environment.data_dir() / "snapshots") {
      return 1;
   }
   auto owner = forge::app::plugin_lifecycle{};
   auto diagnostics = forge::app::diagnostics_store{};
   auto events = forge::app::event_journal{};
   diagnostics.bind_application(owner.observer());
   diagnostics.bind_plugin("sample", "1.0", owner.observer());
   if (!owner.request_stop()) {
      return 2;
   }
   owner.complete_stop();
   const auto snapshot = diagnostics.snapshot(events);
   if (snapshot.lifecycle.phase != forge::app::lifecycle_phase::stopping ||
       snapshot.plugins.size() != 1 || !snapshot.plugins.front().lifecycle.stop_requested) {
      return 3;
   }
   auto options = forge::app::daemon_options{.name = "package-app", .read_yaml = false,
                                             .read_dotenv = false, .read_process_env = false};
   if (!options.shutdown_logging || options.logging_shutdown_timeout != std::chrono::seconds{5}) {
      return 4;
   }
   options.run.handle_sigint = false;
   options.run.handle_sigterm = false;
   options.logging_shutdown_timeout = std::chrono::seconds{2};
   char name[] = "package-app";
   char* argv[] = {name};
   auto factory = [](const forge::app::daemon_context& context) {
      auto builder = forge::app::application_builder{};
      builder.name(context.shell.name).environment(context.environment).runtime(context.shell.runtime)
         .run_foreground([](forge::app::application_shell&) {
            forge::logger::get("package.app").info("installed consumer final record");
            return 0;
         });
      return std::move(builder).build();
   };
   if (forge::app::run_daemon(factory, 1, argv, std::move(options)) != 0) {
      return 5;
   }
   const auto delivered = forge::logging_delivery_metrics();
   return delivered.closed && delivered.accepted > 0 && delivered.queued_records == 0 &&
          delivered.queued_bytes == 0 && delivered.failed == 0 && delivered.dropped == 0 &&
          delivered.accepted == delivered.delivered ? 0 : 6;
}
