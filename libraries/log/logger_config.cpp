module;

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
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
import forge.asio.compute;
import forge.asio.notification;

#include "details/configured_sink_runtime.hxx"

namespace forge {
namespace {

void validate_sink_config(const sink_config& config) {
   if (config.max_records == 0 || config.max_bytes == 0) {
      throw log::exceptions::invalid_config{"logging queue limits must be positive"};
   }
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

} // namespace

log_config& log_config::get() {
   // Keep named loggers usable during static object destruction.
   static auto* config = new log_config;
   return *config;
}

logger log_config::get_logger(const std::string& name) {
   return logger::get(name);
}

void log_config::update_logger(const std::string& name, logger& log) {
   logger::update(name, log);
}

bool configure_logging(const logging_config& config) {
   if (detail::configured_sink_runtime::terminated()) {
      return false;
   }
   static_cast<void>(logger::get());
   return log_config::configure_logging(config);
}

bool log_config::configure_logging(const logging_config& config) {
   try {
      if (detail::configured_sink_runtime::terminated()) {
         return false;
      }
      // Validate the whole document before constructors may open or truncate files.
      validate_config(config);
      // Declared before every prepared route/output: its destructor reports
      // completion only after publication or complete rollback/teardown.
      auto operation = detail::configured_sink_runtime::get().begin_configuration(config.sinks);
      auto sinks = operation.prepare(config.sinks);

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

      auto retired_default = logger{parent};
      {
         // Lock order is runtime -> registry. Stop and publication share the
         // same boundary; slow preparation and retired teardown hold neither.
         auto publication = operation.acquire_publication();
         std::lock_guard lock(get().log_mutex);
         get().logger_map.swap(loggers);
         retired_default = std::move(logger::default_logger());
      }
      // Retired routes and their sinks must be destroyed after unlocking.
      return true;
   } catch (...) {
      report_log_failure();
      return false;
   }
}

namespace {
boost::asio::awaitable<bool> delivery_unavailable() {
   co_return false;
}
} // namespace

boost::asio::awaitable<bool> async_flush_logging(std::chrono::milliseconds timeout) {
   if (detail::configured_sink_runtime::terminated()) {
      return delivery_unavailable();
   }
   return detail::configured_sink_runtime::get().flush(timeout);
}

boost::asio::awaitable<bool> async_shutdown_logging(std::chrono::milliseconds timeout) {
   if (detail::configured_sink_runtime::terminated()) {
      return delivery_unavailable();
   }
   return detail::configured_sink_runtime::get().shutdown(timeout);
}

async_sink_metrics logging_delivery_metrics() {
   if (detail::configured_sink_runtime::terminated()) {
      return {.closed = true};
   }
   return detail::configured_sink_runtime::get().snapshot();
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
