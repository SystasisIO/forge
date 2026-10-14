module;

#include "details/lifecycle_log.hxx"
#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module forge.app.daemon;

import forge.app.application_shell;
import forge.app.runner;
import forge.app.exceptions;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.log.logger;
import forge.log.logger_config;
import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.config.env;
import forge.config.program_options;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.scalar;
import forge.schema.enums;
import forge.codec.yaml;

#include "details/parsed_cli.hxx"

namespace forge::app {
namespace {

using detail::parsed_cli;

void log_daemon(const char* event, const char* operation, detail::lifecycle_log_level level,
                std::exception_ptr error = {}) noexcept {
   detail::emit_lifecycle_log({.logger = "forge.app.daemon", .event = event, .operation = operation,
                              .level = level, .error = error, .canceled_event = "daemon.canceled"});
}

const std::vector<forge::config::program_options::reserved_option>& daemon_reserved_options() {
   static const auto options = std::vector<forge::config::program_options::reserved_option>{
      {.name = "help", .path = "daemon.help", .kind = forge::schema::value_kind::boolean, .aliases = {"h", "daemon.help"}},
      {.name = "profile", .path = "daemon.profile", .kind = forge::schema::value_kind::string, .aliases = {"daemon.profile"}},
      {.name = "data-dir", .path = "daemon.data-dir", .kind = forge::schema::value_kind::string, .aliases = {"daemon.data-dir"}},
      {.name = "config", .path = "daemon.config", .kind = forge::schema::value_kind::string, .aliases = {"daemon.config"}},
      {.name = "dotenv", .path = "daemon.dotenv", .kind = forge::schema::value_kind::string, .aliases = {"daemon.dotenv"}},
      {.name = "runtime-threads", .path = "daemon.runtime-threads", .kind = forge::schema::value_kind::unsigned_integer, .aliases = {"daemon.runtime-threads"}},
      {.name = "scheduler-queue-depth", .path = "daemon.scheduler-queue-depth", .kind = forge::schema::value_kind::unsigned_integer, .aliases = {"daemon.scheduler-queue-depth"}},
      {.name = "shutdown-timeout-ms", .path = "daemon.shutdown-timeout-ms", .kind = forge::schema::value_kind::unsigned_integer, .aliases = {"daemon.shutdown-timeout-ms"}},
      {.name = "check-config", .path = "daemon.check-config", .kind = forge::schema::value_kind::boolean, .aliases = {"daemon.check-config"}},
      {.name = "print-effective-config", .path = "daemon.print-effective-config", .kind = forge::schema::value_kind::boolean, .aliases = {"daemon.print-effective-config"}},
      {.name = "configure", .path = "daemon.configure", .kind = forge::schema::value_kind::boolean, .aliases = {"daemon.configure"}},
   };
   return options;
}

bool document_bool(const forge::config::core::document& document, std::string_view path) {
   const auto* value = document.try_get(path);
   if (!value) {
      return false;
   }
   if (const auto* flag = std::get_if<bool>(&value->storage)) {
      return *flag;
   }
   return false;
}

parsed_cli read_daemon_cli(int argc, char** argv) {
   auto pointers = std::vector<const char*>{};
   pointers.reserve(static_cast<std::size_t>(std::max(argc, 0)));
   for (auto index = 0; index < argc; ++index) {
      pointers.push_back(argv == nullptr ? nullptr : argv[index]);
   }

   auto scanned = forge::config::program_options::pre_scan_reserved(static_cast<int>(pointers.size()), pointers.data(),
                                                          daemon_reserved_options());
   auto parsed = parsed_cli{};
   parsed.document = std::move(scanned.document);
   parsed.filtered_args = std::move(scanned.filtered_args);
   parsed.diagnostics = std::move(scanned.diagnostics);
   parsed.help = document_bool(parsed.document, "daemon.help");
   parsed.check_config = document_bool(parsed.document, "daemon.check-config");
   parsed.print_effective_config = document_bool(parsed.document, "daemon.print-effective-config");
   parsed.configure = document_bool(parsed.document, "daemon.configure");
   parsed.config_explicit = scanned.present("daemon.config");
   parsed.dotenv_explicit = scanned.present("daemon.dotenv");
   if (parsed.filtered_args.empty()) {
      parsed.filtered_args.push_back("forge-daemon");
   }
   return parsed;
}

std::filesystem::path default_data_dir(const daemon_options& options) {
   const auto name = options.default_data_dir_name.empty() ? options.name : options.default_data_dir_name;
   if (const auto home = forge::config::env::home_directory()) {
      return *home / ".forge" / name;
   }
   return std::filesystem::temp_directory_path() / "forge" / name;
}

forge::config::core::component_descriptor daemon_descriptor() {
   return forge::config::core::component_descriptor{
      .section = "daemon",
      .fields =
         {
            forge::config::core::field_descriptor{
               .name = "profile",
               .kind = forge::schema::value_kind::string,
               .has_default = true,
               .default_value = "dev_local",
               .description = "daemon profile name",
            },
            forge::config::core::field_descriptor{
               .name = "data-dir",
               .kind = forge::schema::value_kind::string,
               .description = "daemon data directory",
            },
            forge::config::core::field_descriptor{
               .name = "config",
               .kind = forge::schema::value_kind::string,
               .description = "daemon YAML config path",
            },
            forge::config::core::field_descriptor{
               .name = "dotenv",
               .kind = forge::schema::value_kind::string,
               .description = "daemon .env config path",
            },
            forge::config::core::field_descriptor{
               .name = "runtime-threads",
               .kind = forge::schema::value_kind::unsigned_integer,
               .has_default = true,
               .default_value = 1U,
               .description = "runtime worker thread count",
            },
            forge::config::core::field_descriptor{
               .name = "scheduler-queue-depth",
               .kind = forge::schema::value_kind::unsigned_integer,
               .has_default = true,
               .default_value = 4096U,
               .description = "maximum pending scheduler tasks",
            },
            forge::config::core::field_descriptor{
               .name = "shutdown-timeout-ms",
               .kind = forge::schema::value_kind::unsigned_integer,
               .has_default = true,
               .default_value = 10'000U,
               .description = "shutdown timeout in milliseconds",
            },
            forge::config::core::field_descriptor{
               .name = "help",
               .kind = forge::schema::value_kind::boolean,
               .has_default = true,
               .default_value = false,
               .description = "show daemon and application options",
            },
            forge::config::core::field_descriptor{
               .name = "check-config",
               .kind = forge::schema::value_kind::boolean,
               .has_default = true,
               .default_value = false,
               .description = "validate config and exit",
            },
            forge::config::core::field_descriptor{
               .name = "print-effective-config",
               .kind = forge::schema::value_kind::boolean,
               .has_default = true,
               .default_value = false,
               .description = "print redacted effective config and exit",
            },
            forge::config::core::field_descriptor{
               .name = "configure",
               .kind = forge::schema::value_kind::boolean,
               .has_default = true,
               .default_value = false,
               .description = "write example config and exit",
            },
         },
   };
}

forge::config::core::component_registry daemon_registry() {
   auto registry = forge::config::core::component_registry{};
   registry.add(daemon_descriptor());
   return registry;
}

forge::config::core::component_registry full_registry(const forge::config::core::component_registry& app_registry) {
   auto registry = daemon_registry();
   for (auto component : app_registry.components()) {
      registry.add(std::move(component));
   }
   return registry;
}

std::size_t blocking_budget(unsigned runtime_threads) {
   if (runtime_threads <= 1) {
      return 1;
   }
   return static_cast<std::size_t>(runtime_threads - 1);
}

forge::config::core::document dynamic_daemon_defaults(const daemon_options& options) {
   auto output = forge::config::core::document{};
   const auto data_dir = default_data_dir(options);
   output.set("daemon.profile", options.default_profile);
   output.set("daemon.data-dir", data_dir.string());
   output.set("daemon.runtime-threads", 1U);
   output.set("daemon.scheduler-queue-depth", 4096U);
   output.set("daemon.shutdown-timeout-ms", 10'000U);
   output.set("daemon.help", false);
   output.set("daemon.check-config", false);
   output.set("daemon.print-effective-config", false);
   output.set("daemon.configure", false);
   return output;
}

std::string string_field(const forge::config::core::document& document, std::string_view path, std::string fallback = {}) {
   const auto* value = document.try_get(path);
   if (!value) {
      return fallback;
   }
   if (const auto* text = std::get_if<std::string>(&value->storage)) {
      return *text;
   }
   FORGE_THROW_EXCEPTION(exceptions::config_failed, "daemon config value must be a string",
                         forge::exceptions::ctx("path", path));
}

std::uint64_t unsigned_field(const forge::config::core::document& document, std::string_view path, std::uint64_t fallback) {
   const auto* value = document.try_get(path);
   if (!value) {
      return fallback;
   }
   if (const auto* number = std::get_if<std::uint64_t>(&value->storage)) {
      return *number;
   }
   if (const auto* signed_number = std::get_if<std::int64_t>(&value->storage); signed_number && *signed_number >= 0) {
      return static_cast<std::uint64_t>(*signed_number);
   }
   FORGE_THROW_EXCEPTION(exceptions::config_failed, "daemon config value must be an unsigned integer",
                         forge::exceptions::ctx("path", path));
}

bool bool_field(const forge::config::core::document& document, std::string_view path, bool fallback) {
   const auto* value = document.try_get(path);
   if (!value) {
      return fallback;
   }
   if (const auto* flag = std::get_if<bool>(&value->storage)) {
      return *flag;
   }
   if (const auto* text = std::get_if<std::string>(&value->storage)) {
      auto parsed = false;
      if (forge::schema::parse_bool_text(*text, parsed)) {
         return parsed;
      }
      FORGE_THROW_EXCEPTION(exceptions::config_failed, "boolean has invalid syntax",
                            forge::exceptions::ctx("path", path));
   }
   FORGE_THROW_EXCEPTION(exceptions::config_failed, "daemon config value must be a boolean",
                         forge::exceptions::ctx("path", path));
}

std::vector<const char*> argv_view(const std::vector<std::string>& arguments) {
   auto output = std::vector<const char*>{};
   output.reserve(arguments.size());
   for (const auto& argument : arguments) {
      output.push_back(argument.c_str());
   }
   return output;
}

void append_diagnostics(std::vector<forge::schema::diagnostic>& target,
                        const std::vector<forge::schema::diagnostic>& source) {
   target.insert(target.end(), source.begin(), source.end());
}

void print_diagnostics(const std::vector<forge::schema::diagnostic>& diagnostics) {
   for (const auto& entry : diagnostics) {
      const auto severity = entry.level == forge::schema::severity::error ? "error" : "warning";
      std::cerr << severity << ": " << entry.code;
      if (!entry.path.empty()) {
         std::cerr << " at " << entry.path;
      }
      if (!entry.message.empty()) {
         std::cerr << ": " << entry.message;
      }
      std::cerr << '\n';
   }
}

int fail_with_diagnostics(const std::vector<forge::schema::diagnostic>& diagnostics) {
   log_daemon("daemon.config.failed", "config", detail::lifecycle_log_level::error);
   print_diagnostics(diagnostics);
   return 1;
}

bool has_errors(const std::vector<forge::schema::diagnostic>& diagnostics) {
   for (const auto& entry : diagnostics) {
      if (entry.level == forge::schema::severity::error) {
         return true;
      }
   }
   return false;
}

const char* known_validation_reason(const std::exception& error) noexcept {
   auto message = std::string_view{};
   if (const auto* typed = dynamic_cast<const forge::exceptions::base*>(&error)) {
      message = typed->message();
   } else if (dynamic_cast<const std::invalid_argument*>(&error)) {
      // Legacy configure callbacks sometimes forward only a schema reason as
      // invalid_argument. Recognize exact fixed vocabulary, never echo what().
      message = error.what();
   } else {
      return nullptr;
   }
   for (const auto* reason : {"daemon config value must be a string",
                             "daemon config value must be an unsigned integer",
                             "daemon config value must be a boolean",
                             "boolean has invalid syntax", "integer has invalid syntax",
                             "integer is outside target type range",
                             "config value has incompatible type",
                             "value is above the allowed maximum",
                             "value is below the allowed minimum"}) {
      if (message == reason) {
         return reason;
      }
   }
   return nullptr;
}

void append_bootstrap_diagnostic(std::vector<forge::schema::diagnostic>& diagnostics, const std::exception& error) {
   auto path = std::string{"daemon"};
   const auto* reason = known_validation_reason(error);
   if (reason) {
      if (const auto* typed = dynamic_cast<const forge::exceptions::base*>(&error)) {
         for (const auto& context : typed->context()) {
            if (context.key != "path" || context.redacted) {
               continue;
            }
            for (const auto& option : daemon_reserved_options()) {
               if (context.value == option.path) {
                  path = context.value;
               }
            }
         }
      }
   }
   diagnostics.push_back(forge::schema::diagnostic{
      .path = std::move(path),
      .code = "daemon.bootstrap",
      .level = forge::schema::severity::error,
      .message = reason ? reason : "daemon bootstrap failed",
   });
}

int fail_with_bootstrap_diagnostic(std::vector<forge::schema::diagnostic>& diagnostics, const std::exception& error) {
   append_bootstrap_diagnostic(diagnostics, error);
   return fail_with_diagnostics(diagnostics);
}

forge::config::core::document read_yaml_layer(const std::filesystem::path& path, bool required,
                                      std::vector<forge::schema::diagnostic>& diagnostics) {
   if (path.empty() || !std::filesystem::exists(path)) {
      if (required) {
         diagnostics.push_back(forge::schema::diagnostic{
             .path = "daemon.config",
             .code = "daemon.config_missing",
             .level = forge::schema::severity::error,
             .message = "config file does not exist: " + path.string(),
         });
      }
      return {};
   }
   auto parsed = forge::codec::yaml::load_document(path, forge::codec::yaml::read_options{.source_name = path.string()});
   append_diagnostics(diagnostics, parsed.diagnostics);
   return parsed.ok() ? std::move(parsed.value) : forge::config::core::document{};
}

forge::config::core::document read_dotenv_layer(const std::filesystem::path& path,
                                        const forge::config::core::component_registry& registry,
                                        const daemon_options& options, bool required,
                                        std::vector<forge::schema::diagnostic>& diagnostics) {
   if (!options.read_dotenv || options.env_prefix.empty()) {
      return {};
   }
   if (path.empty() || !std::filesystem::exists(path)) {
      if (required) {
         diagnostics.push_back(forge::schema::diagnostic{
            .path = "daemon.dotenv",
            .code = "daemon.dotenv_missing",
            .level = forge::schema::severity::error,
            .message = ".env file does not exist: " + path.string(),
         });
      }
      return {};
   }
   auto parsed = forge::config::env::load_document(
      path,
      registry,
      forge::config::env::read_options{
         .prefix = options.env_prefix,
         .source_name = path.string(),
      });
   append_diagnostics(diagnostics, parsed.diagnostics);
   return parsed.ok() ? std::move(parsed.value) : forge::config::core::document{};
}

forge::config::core::document read_process_env_layer(const forge::config::core::component_registry& registry,
                                             const daemon_options& options,
                                             forge::config::env::unknown_variable_policy unknown_variables,
                                             std::vector<forge::schema::diagnostic>& diagnostics) {
   if (!options.read_process_env || options.env_prefix.empty()) {
      return {};
   }
   auto parsed = forge::config::env::read_process_document(registry, forge::config::env::read_options{
                                                              .prefix = options.env_prefix,
                                                              .unknown_variables = unknown_variables,
                                                           });
   append_diagnostics(diagnostics, parsed.diagnostics);
   return parsed.ok() ? std::move(parsed.value) : forge::config::core::document{};
}

forge::config::core::document read_product_cli_layer(const std::vector<std::string>& filtered_args,
                                             const forge::config::core::component_registry& registry,
                                             const daemon_options& options,
                                             std::vector<forge::schema::diagnostic>& diagnostics) {
   if (!options.read_cli) {
      return {};
   }
   const auto pointers = argv_view(filtered_args);
   auto parsed = forge::config::program_options::parse(static_cast<int>(pointers.size()), pointers.data(), registry);
   append_diagnostics(diagnostics, parsed.diagnostics);
   return parsed.ok() ? std::move(parsed.document) : forge::config::core::document{};
}

daemon_context context_from_document(const daemon_options& options, const forge::config::core::document& document,
                                     const std::filesystem::path& working_dir) {
   auto context = daemon_context{};
   const auto config_filename = options.config_filename.empty() ? std::filesystem::path{"config.yml"}
                                                                : std::filesystem::path{options.config_filename};
   const auto dotenv_filename = options.dotenv_filename.empty() ? std::filesystem::path{".env"}
                                                                : std::filesystem::path{options.dotenv_filename};
   context.name = options.name.empty() ? "forge-daemon" : options.name;
   auto data_dir = std::filesystem::path{string_field(document, "daemon.data-dir", default_data_dir(options).string())};
   if (!data_dir.empty() && data_dir.is_relative()) {
      data_dir = working_dir / data_dir;
   }
   context.environment = application_environment{std::move(data_dir),
                                                 string_field(document, "daemon.profile", options.default_profile)};
   context.config_path = document.try_get("daemon.config")
                            ? std::filesystem::path{string_field(document, "daemon.config")}
                            : context.environment.resolve_data_path(config_filename);
   context.dotenv_path = document.try_get("daemon.dotenv")
                            ? std::filesystem::path{string_field(document, "daemon.dotenv")}
                            : context.environment.resolve_data_path(dotenv_filename);

   const auto runtime_threads = unsigned_field(document, "daemon.runtime-threads", 1);
   const auto queue_depth = unsigned_field(document, "daemon.scheduler-queue-depth", 4096);
   context.shell = forge::app::application_shell_options{
      .name = context.name,
      .runtime =
         forge::asio::runtime_options{
            .worker_threads = static_cast<std::size_t>(runtime_threads),
            .thread_name = context.name,
         },
      .scheduler =
         forge::asio::task::scheduler::options{
            .max_blocking_tasks = blocking_budget(runtime_threads),
            .max_pending_tasks = static_cast<std::size_t>(queue_depth),
         },
   };
   context.shell.environment = context.environment;
   return context;
}

forge::config::core::document resolve_daemon_paths(forge::config::core::document document, const daemon_options& options,
                                                  const std::filesystem::path& working_dir) {
   const auto context = context_from_document(options, document, working_dir);
   document.set("daemon.data-dir", context.environment.data_dir().string());
   if (!document.try_get("daemon.config")) {
      document.set("daemon.config", context.config_path.string());
   }
   if (!document.try_get("daemon.dotenv")) {
      document.set("daemon.dotenv", context.dotenv_path.string());
   }
   return document;
}

void reset_daemon_action_flags(forge::config::core::document& document) {
   document.set("daemon.help", false);
   document.set("daemon.check-config", false);
   document.set("daemon.print-effective-config", false);
   document.set("daemon.configure", false);
}

void apply_run_options_from_document(forge::app::run_options& options, const forge::config::core::document& document) {
   const auto timeout_ms = unsigned_field(document, "daemon.shutdown-timeout-ms", 10'000);
   const auto clamped = timeout_ms > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                           ? std::numeric_limits<int>::max()
                           : static_cast<int>(timeout_ms);
   options.shutdown_timeout = std::chrono::milliseconds{clamped};
}

std::string daemon_help(const daemon_options& options, const forge::config::core::component_registry& registry) {
   auto output = std::string{};
   output += options.display_name.empty() ? options.name : options.display_name;
   output += "\n\nDaemon options:\n";
   output += "  --help, -h\n";
   output += "  --profile <name>\n";
   output += "  --data-dir <path>\n";
   output += "  --config <path>\n";
   output += "  --dotenv <path>\n";
   output += "  --runtime-threads <count>\n";
   output += "  --scheduler-queue-depth <count>\n";
   output += "  --shutdown-timeout-ms <milliseconds>\n";
   output += "  --check-config\n";
   output += "  --print-effective-config\n";
   output += "  --configure\n\n";
   output += forge::config::program_options::help(registry, "Application and plugin options");
   return output;
}

int run_daemon_impl(daemon_factory make_app, int argc, char** argv, daemon_options options) {
   log_daemon("daemon.config.begin", "config", detail::lifecycle_log_level::debug);
   if (!make_app) {
      log_daemon("daemon.config.failed", "factory", detail::lifecycle_log_level::error);
      std::cerr << "error: daemon factory is empty\n";
      return 1;
   }

   auto diagnostics = std::vector<forge::schema::diagnostic>{};
   auto daemon_cli = read_daemon_cli(argc, argv);
   append_diagnostics(diagnostics, daemon_cli.diagnostics);
   if (has_errors(diagnostics)) {
      return fail_with_diagnostics(diagnostics);
   }

   try {
      if (options.logging_shutdown_timeout < std::chrono::milliseconds::zero()) {
         FORGE_THROW_EXCEPTION(exceptions::config_failed, "logging shutdown timeout must not be negative");
      }
      // Factories and hooks must not change the base used by later config layers.
      const auto working_dir = std::filesystem::current_path();
      const auto daemon_defaults = dynamic_daemon_defaults(options);
      const auto daemon_only_registry = daemon_registry();

      if (daemon_cli.help) {
         const auto context = context_from_document(
            options, forge::config::core::merge({daemon_defaults, daemon_cli.document}), working_dir);
         auto app = make_app(context);
         if (!app) {
            log_daemon("daemon.config.failed", "factory", detail::lifecycle_log_level::error);
            std::cerr << "error: daemon factory returned null application\n";
            return 1;
         }
         std::cout << daemon_help(options, app->describe_config());
         log_daemon("daemon.mode.complete", "help", detail::lifecycle_log_level::debug);
         return 0;
      }

      auto early_process_env =
         read_process_env_layer(daemon_only_registry, options, forge::config::env::unknown_variable_policy::ignore, diagnostics);
      if (has_errors(diagnostics)) {
         return fail_with_diagnostics(diagnostics);
      }

      auto early_without_yaml = forge::config::core::merge({daemon_defaults, early_process_env, daemon_cli.document});
      auto early_context = context_from_document(options, early_without_yaml, working_dir);
      auto yaml = options.read_yaml ? read_yaml_layer(
                                         early_context.config_path,
                                         daemon_cli.config_explicit && !daemon_cli.configure && !daemon_cli.help,
                                         diagnostics)
                                    : forge::config::core::document{};
      if (has_errors(diagnostics)) {
         return fail_with_diagnostics(diagnostics);
      }

      auto early_paths = resolve_daemon_paths(
         forge::config::core::merge({daemon_defaults, yaml, early_process_env, daemon_cli.document}), options, working_dir);
      auto early_path_context = context_from_document(options, early_paths, working_dir);
      auto early_dotenv = read_dotenv_layer(
         early_path_context.dotenv_path,
         daemon_only_registry,
         options,
         daemon_cli.dotenv_explicit && !daemon_cli.configure && !daemon_cli.help,
         diagnostics);
      if (has_errors(diagnostics)) {
         return fail_with_diagnostics(diagnostics);
      }

      auto early_effective = resolve_daemon_paths(
         forge::config::core::merge({daemon_defaults, yaml, early_dotenv, early_process_env, daemon_cli.document}), options,
         working_dir);
      auto context = context_from_document(options, early_effective, working_dir);
      auto app = make_app(context);
      if (!app) {
         log_daemon("daemon.config.failed", "factory", detail::lifecycle_log_level::error);
         std::cerr << "error: daemon factory returned null application\n";
         return 1;
      }

      const auto app_registry = app->describe_config();
      auto registry = full_registry(app_registry);

      auto dotenv = read_dotenv_layer(
         context.dotenv_path,
         registry,
         options,
         daemon_cli.dotenv_explicit && !daemon_cli.configure && !daemon_cli.help,
         diagnostics);
      auto process_env =
         read_process_env_layer(registry, options, forge::config::env::unknown_variable_policy::warn, diagnostics);
      auto product_cli = read_product_cli_layer(daemon_cli.filtered_args, app_registry, options, diagnostics);
      if (has_errors(diagnostics)) {
         return fail_with_diagnostics(diagnostics);
      }

      auto effective = resolve_daemon_paths(
         forge::config::core::merge({
            forge::config::core::defaults_for(registry),
            daemon_defaults,
            yaml,
            dotenv,
            process_env,
            daemon_cli.document,
            product_cli,
         }),
         options, working_dir);

      const auto help = bool_field(effective, "daemon.help", false);
      const auto check_config = bool_field(effective, "daemon.check-config", false);
      const auto print_effective_config = bool_field(effective, "daemon.print-effective-config", false);
      const auto configure = bool_field(effective, "daemon.configure", false);

      if (help) {
         std::cout << daemon_help(options, app_registry);
         log_daemon("daemon.mode.complete", "help", detail::lifecycle_log_level::debug);
         return 0;
      }

      append_diagnostics(diagnostics, forge::config::core::validate_ingestion(effective, registry));
      if (has_errors(diagnostics)) {
         return fail_with_diagnostics(diagnostics);
      }

      log_daemon("daemon.config.accepted", "config", detail::lifecycle_log_level::debug);

      if (configure) {
         if (!options.allow_configure) {
            log_daemon("daemon.config.failed", "generate_config", detail::lifecycle_log_level::error);
            std::cerr << "error: --configure is disabled for this daemon\n";
            return 1;
         }
         const auto config_path = string_field(effective, "daemon.config", context.config_path.string());
         if (std::filesystem::exists(config_path)) {
            log_daemon("daemon.config.failed", "generate_config", detail::lifecycle_log_level::error);
            std::cerr << "error: refusing to overwrite existing config: " << config_path << '\n';
            return 1;
         }
         const auto parent = std::filesystem::path{config_path}.parent_path();
         if (!parent.empty()) {
            std::filesystem::create_directories(parent);
         }
         auto generated =
            forge::config::core::merge({forge::config::core::defaults_for(registry), daemon_defaults, early_process_env,
                                daemon_cli.document});
         reset_daemon_action_flags(generated);
         generated = resolve_daemon_paths(std::move(generated), options, working_dir);
         auto saved = forge::codec::yaml::save_document(config_path, forge::config::core::redact(std::move(generated), registry));
         append_diagnostics(diagnostics, saved.diagnostics);
         if (!saved.ok()) {
            return fail_with_diagnostics(diagnostics);
         }
         log_daemon("daemon.mode.complete", "generate_config", detail::lifecycle_log_level::debug);
         return 0;
      }
      if (print_effective_config) {
         if (!options.allow_print_effective_config) {
            log_daemon("daemon.config.failed", "print_config", detail::lifecycle_log_level::error);
            std::cerr << "error: --print-effective-config is disabled for this daemon\n";
            return 1;
         }
         auto written = forge::codec::yaml::write_document(forge::config::core::redact(effective, registry));
         append_diagnostics(diagnostics, written.diagnostics);
         if (!written.ok()) {
            return fail_with_diagnostics(diagnostics);
         }
         std::cout << written.text;
         log_daemon("daemon.mode.complete", "print_config", detail::lifecycle_log_level::debug);
         return 0;
      }
      if (check_config) {
         if (!options.allow_check_config) {
            log_daemon("daemon.config.failed", "check_config", detail::lifecycle_log_level::error);
            std::cerr << "error: --check-config is disabled for this daemon\n";
            return 1;
         }
         try {
            app->configure(effective);
         } catch (const std::exception& error) {
            log_daemon("daemon.config.failed", "check_config", detail::lifecycle_log_level::error,
                       std::current_exception());
            const auto* reason = known_validation_reason(error);
            std::cerr << "error: config.validation at application.configure: "
                      << (reason ? reason : "application configuration failed") << '\n';
            return 1;
         }
         log_daemon("daemon.mode.complete", "check_config", detail::lifecycle_log_level::debug);
         return 0;
      }

      auto run_options = options.run;
      apply_run_options_from_document(run_options, effective);
      try {
         if (options.shutdown_logging) {
            // The borrowed runner waits cleanup even after an app timeout.
            // Keep app ownership here so all destructor logs precede drain.
            return forge::app::run_application(*app, effective, std::move(run_options));
         }
         return forge::app::run_application(std::move(app), effective, std::move(run_options));
      } catch (const std::exception&) {
         log_daemon("daemon.run.failed", "run", detail::lifecycle_log_level::error, std::current_exception());
         std::cerr << "error: application run failed\n";
         return 1;
      }
   } catch (const std::exception& error) {
      log_daemon("daemon.config.failed", "bootstrap", detail::lifecycle_log_level::error, std::current_exception());
      return fail_with_bootstrap_diagnostic(diagnostics, error);
   }
}

bool shutdown_daemon_logging(std::chrono::milliseconds timeout) noexcept {
   try {
      // This runtime only awaits logging fences. Blocking sink callbacks stay
      // on the configured logging owner's dedicated compute workers.
      auto runtime = forge::asio::runtime{
          {.worker_threads = 1, .thread_name = "forge-log-shutdown"}};
      const auto drained = forge::asio::blocking::run(runtime, forge::async_shutdown_logging(timeout));
      if (!drained) {
         forge::report_log_failure();
      }
      return drained;
   } catch (...) {
      // stderr may itself be the blocked output; never print or log here and
      // never resurrect a logger after configured delivery admission closes.
      forge::report_log_failure();
      return false;
   }
}

} // namespace

int run_daemon(daemon_factory make_app, int argc, char** argv, daemon_options options) {
   const auto shutdown_logging = options.shutdown_logging;
   // The body rejects negative configuration. Cleanup still makes a bounded,
   // nonwaiting attempt if that validation (or an earlier path) fails.
   const auto timeout = options.logging_shutdown_timeout < std::chrono::milliseconds::zero()
                            ? std::chrono::milliseconds::zero() : options.logging_shutdown_timeout;
   auto exit_code = 0;
   auto failure = std::exception_ptr{};
   try {
      // The body destroys its application before returning to the drain boundary.
      exit_code = run_daemon_impl(std::move(make_app), argc, argv, std::move(options));
   } catch (...) {
      failure = std::current_exception();
   }

   // Moved-from std::function handles may still retain logging captures.
   make_app = {};
   options.run.wait_for_stop = {};
   const auto drained = !shutdown_logging || shutdown_daemon_logging(timeout);
   if (failure) {
      std::rethrow_exception(failure);
   }
   return exit_code == 0 && !drained ? 1 : exit_code;
}

} // namespace forge::app
