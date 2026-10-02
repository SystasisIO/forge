module;

#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module forge.log.logger_config;

import forge.log.exceptions;
import forge.variant.value;

namespace forge {
namespace {

void validate_sink_config(const sink_config& config) {
   const auto args = config.args.is_null() ? variant_object{} : config.args.get_object();
   if (config.type == "console") {
      if (args.contains("stream")) {
         const auto name = args["stream"].as_string();
         if (name != "std_error" && name != "std_out" && name != "by_level") {
            throw log::exceptions::invalid_config{"unknown console sink stream"};
         }
      }
   } else if (config.type == "file" || config.type == "jsonl") {
      if (!args.contains("path") || args["path"].as_string().empty()) {
         throw log::exceptions::invalid_config{"file sink requires a non-empty path"};
      }
      if (args.contains("append")) {
         static_cast<void>(args["append"].as_bool());
      }
   } else {
      throw log::exceptions::invalid_config{"unknown logging sink type"};
   }
}

void validate_config(const logging_config& config) {
   auto names = std::unordered_set<std::string>{};
   for (const auto& value : config.sinks) {
      if (value.name.empty() || !names.insert(value.name).second) {
         throw log::exceptions::invalid_config{"logging sink names must be non-empty and unique"};
      }
      validate_sink_config(value);
   }
   auto loggers = std::unordered_set<std::string>{};
   for (const auto& value : config.loggers) {
      if (value.name.empty() || !loggers.insert(value.name).second) {
         throw log::exceptions::invalid_config{"logger names must be non-empty and unique"};
      }
      if (value.level && (*value.level < log_level::all || *value.level > log_level::off)) {
         throw log::exceptions::invalid_config{"logger level is invalid"};
      }
      for (const auto& name : value.sinks) {
         if (!names.contains(name)) {
            throw log::exceptions::invalid_config{"logger references an unknown sink"};
         }
      }
   }
}

std::shared_ptr<sink> create_sink(const sink_config& config) {
   const auto args = config.args.is_null() ? variant_object{} : config.args.get_object();
   if (config.type == "console") {
      auto stream = console_stream::by_level;
      if (args.contains("stream")) {
         const auto name = args["stream"].as_string();
         if (name == "std_error") {
            stream = console_stream::standard_error;
         } else if (name == "std_out") {
            stream = console_stream::standard_out;
         } else if (name != "by_level") {
            throw log::exceptions::invalid_config{"unknown console sink stream"};
         }
      }
      return std::make_shared<console_sink>(stream);
   }
   if (config.type == "file" || config.type == "jsonl") {
      if (!args.contains("path")) {
         throw log::exceptions::invalid_config{"file sink requires path"};
      }
      const auto path = std::filesystem::path{args["path"].as_string()};
      const auto append = !args.contains("append") || args["append"].as_bool();
      if (config.type == "jsonl") {
         return std::make_shared<jsonl_sink>(path, append);
      }
      return std::make_shared<file_sink>(path, append);
   }
   throw log::exceptions::invalid_config{"unknown logging sink type"};
}

} // namespace

log_config& log_config::get() {
   // Keep named loggers usable during static object destruction.
   static auto* config = new log_config;
   return *config;
}

logger log_config::get_logger(const std::string& name) {
   std::lock_guard lock(get().log_mutex);
   auto& loggers = get().logger_map;
   if (const auto existing = loggers.find(name); existing != loggers.end()) {
      return existing->second;
   }
   auto result = logger{name};
   if (name != default_logger_name) {
      if (const auto parent = loggers.find(default_logger_name); parent != loggers.end()) {
         result.set_parent(parent->second);
         result.set_enabled(parent->second.is_enabled());
         result.set_log_level(parent->second.get_log_level());
      }
   }
   loggers.emplace(name, result);
   return result;
}

void log_config::update_logger(const std::string& name, logger& log) {
   std::lock_guard lock(get().log_mutex);
   if (log.get_name().empty()) {
      log.set_name(name);
   }
   if (name != default_logger_name && log.get_parent() == nullptr) {
      if (const auto parent = get().logger_map.find(default_logger_name); parent != get().logger_map.end()) {
         log.set_parent(parent->second);
      }
   }
   get().logger_map[name] = log;
   if (name == default_logger_name) {
      logger::default_logger() = log;
   }
}

bool configure_logging(const logging_config& config) {
   static_cast<void>(logger::get());
   return log_config::configure_logging(config);
}

bool log_config::configure_logging(const logging_config& config) {
   try {
      // Validate the whole document before constructors may open or truncate files.
      validate_config(config);
      auto sinks = std::unordered_map<std::string, std::shared_ptr<sink>>{};
      for (const auto& value : config.sinks) {
         sinks.emplace(value.name, create_sink(value));
      }

      auto loggers = std::unordered_map<std::string, logger>{};
      auto parent = logger{default_logger_name};
      const auto configure = [&](logger& route, const logger_config& value) {
         route.set_enabled(value.enabled.value_or(parent.is_enabled()));
         route.set_log_level(value.level.value_or(parent.get_log_level()));
         for (const auto& name : value.sinks) {
            const auto found = sinks.find(name);
            if (found == sinks.end()) {
               throw log::exceptions::invalid_config{"logger references an unknown sink"};
            }
            route.add_sink(found->second);
         }
      };
      for (const auto& value : config.loggers) {
         if (value.name == default_logger_name) {
            configure(parent, value);
         }
      }
      loggers.emplace(default_logger_name, parent);
      for (const auto& value : config.loggers) {
         if (value.name != default_logger_name) {
            auto route = logger{value.name, parent};
            configure(route, value);
            loggers.emplace(value.name, route);
         }
      }

      std::lock_guard lock(get().log_mutex);
      get().logger_map = std::move(loggers);
      logger::default_logger() = parent;
      return true;
   } catch (const std::exception& error) {
      std::cerr << "logging configuration failed: " << error.what() << '\n';
      return false;
   }
}

logging_config logging_config::default_config() {
   auto config = logging_config{};
   config.sinks.push_back(
       {.name = "stderr", .type = "console", .args = mutable_variant_object{}("stream", "std_error")});
   auto route = logger_config{default_logger_name};
   route.level = log_level::info;
   route.sinks.push_back("stderr");
   config.loggers.push_back(std::move(route));
   return config;
}

} // namespace forge
