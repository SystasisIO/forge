module;
#include <chrono>
#include <boost/describe.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module forge.log.record;

import forge.variant.value;
import forge.variant.schema;

export namespace forge {

class log_level {
 public:
   enum values { all, debug, info, warn, error, off };
   BOOST_DESCRIBE_NESTED_ENUM(values, all, debug, info, warn, error, off)
   log_level(values v = off) : value(v) {}
   explicit log_level(int v) : value(static_cast<values>(v)) {}
   operator int() const {
      return value;
   }
   std::string to_string() const;
   values value;
};

void to_variant(log_level level, variant& value);
void from_variant(const variant& value, log_level& level);
void set_thread_name(const std::string& name);
const std::string& get_thread_name();

struct log_field {
   std::string key;
   std::string value;
   bool redacted = false;
};

using log_fields = std::vector<log_field>;

[[nodiscard]] std::string format_log_value(const variant& value);

template <typename T> log_field log_ctx(std::string_view key, const T& value) {
   return {std::string{key}, format_log_value(variant_schema::encode_diagnostic(value)), false};
}

template <typename T> log_field log_secret(std::string_view key, const T&) {
   return {std::string{key}, "<redacted>", true};
}

void append_log_field(log_fields& fields, log_field field);

class log_field_builder {
 public:
   template <typename T> log_field_builder& operator()(std::string_view key, const T& value) {
      append_log_field(_fields, log_ctx(key, value));
      return *this;
   }
   [[nodiscard]] log_fields take();

 private:
   log_fields _fields;
};

[[nodiscard]] std::string interpolate_log_message(const std::string& message, const log_fields& fields);

struct stacktrace_frame {
   std::size_t index = 0;
   std::uintptr_t address = 0;
   std::string name;
   std::string source_file;
   std::uint64_t source_line = 0;
};

struct stacktrace_snapshot {
   std::string backend;
   std::string unavailable_reason;
   std::vector<stacktrace_frame> frames;

   [[nodiscard]] bool available() const noexcept {
      return !frames.empty();
   }
};

[[nodiscard]] stacktrace_snapshot capture_stacktrace(std::size_t skip = 0, std::size_t max_frames = 64);
[[nodiscard]] std::string format_stacktrace(const stacktrace_snapshot& stacktrace);

struct log_record {
   log_level level = log_level::info;
   std::string logger;
   std::string component;
   std::string message;
   log_fields fields;
   std::chrono::sys_time<std::chrono::microseconds> timestamp;
   std::string thread_id;
   std::string thread_name;
   std::source_location location;
   std::optional<stacktrace_snapshot> stacktrace;
   std::string exception_chain;
};

[[nodiscard]] std::string format_text_log_record(const log_record& record);
[[nodiscard]] std::string format_json_log_record(const log_record& record);

class sink {
 public:
   virtual ~sink() = default;
   virtual void log(const log_record& record) = 0;
};

enum class console_stream { by_level, standard_out, standard_error };

class console_sink final : public sink {
 public:
   explicit console_sink(bool stderr_for_warnings = true);
   explicit console_sink(console_stream stream);
   ~console_sink() override;
   void log(const log_record& record) override;

 private:
   console_stream _stream = console_stream::by_level;
};

class file_sink final : public sink {
 public:
   explicit file_sink(std::filesystem::path path, bool append = true);
   ~file_sink() override;
   void log(const log_record& record) override;

 private:
   class impl;
   std::unique_ptr<impl> impl_;
};

class jsonl_sink final : public sink {
 public:
   explicit jsonl_sink(std::filesystem::path path, bool append = true);
   ~jsonl_sink() override;
   void log(const log_record& record) override;

 private:
   class impl;
   std::unique_ptr<impl> impl_;
};

} // namespace forge

export namespace forge {
BOOST_DESCRIBE_STRUCT(log_level, (), (value))
}
