module;
#include <memory>
#include <source_location>
#include <string>
#include <string_view>

export module forge.log.logger;

import forge.core.utility;
export import forge.log.record;

export namespace forge {
inline const std::string default_logger_name = "default";

class logger {
 public:
   static logger& default_logger();
   static logger get(const std::string& name = default_logger_name);
   static void update(const std::string& name, logger& log);

   logger();
   logger(const std::string& name, const logger& parent = nullptr);
   logger(std::nullptr_t);
   logger(const logger& c);
   logger(logger&& c) noexcept;
   ~logger();
   logger& operator=(const logger&);
   logger& operator=(logger&&) noexcept;
   friend bool operator==(const logger&, nullptr_t);
   friend bool operator!=(const logger&, nullptr_t);

   logger& set_log_level(log_level e);
   log_level get_log_level() const;
   logger& set_parent(const logger& l);
   logger get_parent() const;

   void set_name(const std::string& n);
   std::string get_name() const;

   void set_enabled(bool e);
   bool is_enabled(log_level e) const;
   bool is_enabled() const;
   void log(log_record record);
   void log(log_level level, std::string message, log_fields fields = {},
            std::source_location location = std::source_location::current());
   void debug(std::string message, log_fields fields = {},
              std::source_location location = std::source_location::current());
   void info(std::string message, log_fields fields = {},
             std::source_location location = std::source_location::current());
   void warn(std::string message, log_fields fields = {},
             std::source_location location = std::source_location::current());
   void error(std::string message, log_fields fields = {},
              std::source_location location = std::source_location::current());
   void add_sink(std::shared_ptr<sink> sink);
   void remove_sink(const std::shared_ptr<sink>& sink);

 private:
   class impl;
   std::shared_ptr<impl> my;
};

// Macro implementation hooks: every expression is evaluated once, after the
// originating logger's level filter where applicable.
[[nodiscard]] logger resolve_log_logger(const logger& value);
[[nodiscard]] logger resolve_log_logger(std::string_view name);
[[nodiscard]] logger resolve_log_logger(const std::string& name);
[[nodiscard]] logger resolve_log_logger(const char* name);
void emit_log(logger& route, log_level level, std::string message, std::source_location location);
void emit_log(logger& route, log_level level, std::string message, log_fields fields, std::source_location location);
void emit_log(logger& route, log_level level, log_record record, std::source_location location);

} // namespace forge
