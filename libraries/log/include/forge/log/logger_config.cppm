module;
#include <boost/describe.hpp>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

export module forge.log.logger_config;

import forge.log.logger;
export import forge.log.async_sink;
export import forge.log.record;
import forge.variant.described;
import forge.variant.value;

export namespace forge {

struct sink_config {
   std::string name;
   std::string type;
   variant args;
   bool asynchronous = true;
   std::size_t max_records = 8192;
   std::size_t max_bytes = 8 * 1024 * 1024;
};

struct logger_config {
   explicit logger_config(std::string name = {}) : name(std::move(name)) {}
   std::string name;
   std::optional<log_level> level;
   std::optional<bool> enabled;
   std::vector<std::string> sinks;
};

struct logging_config {
   static logging_config default_config();
   std::vector<sink_config> sinks;
   std::vector<logger_config> loggers;
};

struct log_config {
   static logger get_logger(const std::string& name);
   static void update_logger(const std::string& name, logger& log);
   static bool configure_logging(const logging_config& config);

 private:
   static log_config& get();
   friend class logger;
   std::mutex log_mutex;
   std::unordered_map<std::string, logger> logger_map;
};

bool configure_logging(const logging_config& config);

// Process-wide configured delivery. Direct synchronous sinks and OTLP retain
// their own lifecycle. Call shutdown after producers, not per application.
boost::asio::awaitable<bool> async_flush_logging(std::chrono::milliseconds timeout = std::chrono::seconds{5});
boost::asio::awaitable<bool> async_shutdown_logging(std::chrono::milliseconds timeout = std::chrono::seconds{5});
[[nodiscard]] async_sink_metrics logging_delivery_metrics();

BOOST_DESCRIBE_STRUCT(sink_config, (), (name, type, args, asynchronous, max_records, max_bytes))
BOOST_DESCRIBE_STRUCT(logger_config, (), (name, level, enabled, sinks))
BOOST_DESCRIBE_STRUCT(logging_config, (), (sinks, loggers))

} // namespace forge
