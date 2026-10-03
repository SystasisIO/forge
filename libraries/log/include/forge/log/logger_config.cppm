module;
#include <boost/describe.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

export module forge.log.logger_config;

import forge.log.logger;
export import forge.log.record;
import forge.variant.described;
import forge.variant.value;

export namespace forge {

struct sink_config {
   std::string name;
   std::string type;
   variant args;
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

BOOST_DESCRIBE_STRUCT(sink_config, (), (name, type, args))
BOOST_DESCRIBE_STRUCT(logger_config, (), (name, level, enabled, sinks))
BOOST_DESCRIBE_STRUCT(logging_config, (), (sinks, loggers))

} // namespace forge
