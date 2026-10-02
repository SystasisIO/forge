module;
#include <memory>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

module forge.log.logger;

import forge.log.record;
import forge.log.logger_config;
import forge.core.utility;
import forge.variant.value;

namespace forge {

namespace {

std::string current_thread_id() {
   auto out = std::ostringstream{};
   out << std::this_thread::get_id();
   return out.str();
}

void deliver(const std::vector<std::shared_ptr<sink>>& sinks, const log_record& record) {
   for (const auto& current_sink : sinks) {
      try {
         current_sink->log(record);
      } catch (const std::exception& error) {
         std::cerr << "ERROR: logger::log sink std::exception: " << error.what() << std::endl;
      } catch (...) {
         std::cerr << "ERROR: logger::log sink unknown exception" << std::endl;
      }
   }
}

} // namespace

static void ensure_default_logging_configured() {
   static const bool configured = log_config::configure_logging(logging_config::default_config());
   (void)configured;
}

class logger::impl {
 public:
   impl() : _parent(nullptr), _enabled(true), _level(log_level::warn) {}
   std::string _name;
   logger _parent;
   bool _enabled;
   log_level _level;

   std::vector<std::shared_ptr<sink>> _sinks;

   [[nodiscard]] std::vector<std::shared_ptr<sink>> targets() const {
      auto result = std::vector<std::shared_ptr<sink>>{};
      auto seen_loggers = std::unordered_set<const impl*>{};
      auto seen_sinks = std::unordered_set<sink*>{};

      for (auto current = this; current != nullptr && seen_loggers.insert(current).second;
           current = current->_parent.my.get()) {
         for (const auto& current_sink : current->_sinks) {
            if (current_sink && seen_sinks.insert(current_sink.get()).second) {
               result.push_back(current_sink);
            }
         }
      }
      return result;
   }
};

logger::logger() : my(new impl()) {}

logger::logger(nullptr_t) {}

logger::logger(const std::string& name, const logger& parent) : my(new impl()) {
   my->_name = name;
   my->_parent = parent;
}

logger::logger(const logger& l) : my(l.my) {}

logger::logger(logger&& l) noexcept : my(std::move(l.my)) {}

logger::~logger() {}

logger& logger::operator=(const logger& l) {
   my = l.my;
   return *this;
}
logger& logger::operator=(logger&& l) noexcept {
   forge_swap(my, l.my);
   return *this;
}
bool operator==(const logger& l, std::nullptr_t) {
   return !l.my;
}
bool operator!=(const logger& l, std::nullptr_t) {
   return !!l.my;
}

void logger::set_enabled(bool e) {
   my->_enabled = e;
}
bool logger::is_enabled() const {
   return my && my->_enabled;
}
bool logger::is_enabled(log_level e) const {
   return my && my->_enabled && e >= my->_level && e < log_level::off;
}

void logger::log(log_record record) {
   if (!is_enabled(record.level)) {
      return;
   }

   std::unique_lock g(log_config::get().log_mutex);
   record.logger = my->_name;
   const auto targets = my->targets();
   g.unlock();
   for (auto& field : record.fields) {
      if (field.redacted) {
         field.value = "<redacted>";
      }
   }
   record.message = interpolate_log_message(record.message, record.fields);
   deliver(targets, record);
}

void logger::log(log_level level, std::string message, log_fields fields, std::source_location location) {
   if (!is_enabled(level)) {
      return;
   }

   auto record = log_record{
       .level = level,
       .message = std::move(message),
       .fields = std::move(fields),
       .timestamp = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now()),
       .thread_id = current_thread_id(),
       .thread_name = forge::get_thread_name(),
       .location = location,
   };
   if (static_cast<int>(level) >= static_cast<int>(log_level::error)) {
      record.stacktrace = capture_stacktrace(1);
   }
   log(std::move(record));
}

void logger::set_name(const std::string& n) {
   my->_name = n;
}

void logger::debug(std::string message, log_fields fields, std::source_location location) {
   log(log_level::debug, std::move(message), std::move(fields), location);
}
void logger::info(std::string message, log_fields fields, std::source_location location) {
   log(log_level::info, std::move(message), std::move(fields), location);
}
void logger::warn(std::string message, log_fields fields, std::source_location location) {
   log(log_level::warn, std::move(message), std::move(fields), location);
}
void logger::error(std::string message, log_fields fields, std::source_location location) {
   log(log_level::error, std::move(message), std::move(fields), location);
}
std::string logger::get_name() const {
   return my->_name;
}

logger logger::get(const std::string& s) {
   ensure_default_logging_configured();
   return log_config::get_logger(s);
}

logger& logger::default_logger() {
   static logger* the_default_logger = new logger;
   return *the_default_logger;
}

void logger::update(const std::string& name, logger& log) {
   ensure_default_logging_configured();
   log_config::update_logger(name, log);
}

logger logger::get_parent() const {
   return my->_parent;
}
logger& logger::set_parent(const logger& p) {
   my->_parent = p;
   return *this;
}

log_level logger::get_log_level() const {
   return my->_level;
}
logger& logger::set_log_level(log_level ll) {
   my->_level = ll;
   return *this;
}

void logger::add_sink(std::shared_ptr<sink> sink) {
   if (!sink) {
      throw std::invalid_argument{"cannot add null log sink"};
   }
   std::lock_guard g(log_config::get().log_mutex);
   my->_sinks.push_back(std::move(sink));
}

void logger::remove_sink(const std::shared_ptr<sink>& sink) {
   if (!sink) {
      return;
   }
   std::lock_guard g(log_config::get().log_mutex);
   my->_sinks.erase(std::remove(my->_sinks.begin(), my->_sinks.end(), sink), my->_sinks.end());
}

logger resolve_log_logger(const logger& value) {
   return value;
}
logger resolve_log_logger(std::string_view name) {
   return logger::get(std::string{name});
}
logger resolve_log_logger(const std::string& name) {
   return logger::get(name);
}
logger resolve_log_logger(const char* name) {
   return logger::get(name ? name : default_logger_name);
}

void emit_log(logger& route, log_level level, std::string message, std::source_location location) {
   route.log(level, std::move(message), {}, location);
}

void emit_log(logger& route, log_level level, std::string message, log_fields fields, std::source_location location) {
   route.log(level, std::move(message), std::move(fields), location);
}

void emit_log(logger& route, log_level level, log_record record, std::source_location location) {
   record.level = level;
   if (record.location.line() == 0) {
      record.location = location;
   }
   route.log(std::move(record));
}

} // namespace forge
