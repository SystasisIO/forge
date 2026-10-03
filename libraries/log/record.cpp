module;

#if FORGE_HAS_STD_STACKTRACE
#include <stacktrace>
#elif FORGE_HAS_BOOST_STACKTRACE
#include <boost/stacktrace.hpp>
#endif

#define BOOST_DLL_USE_STD_FS
#include <boost/dll/runtime_symbol_info.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <stdexcept>
#include <utility>
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
#include <pthread.h>
#endif

module forge.log.record;

import forge.chrono.iso8601;
import forge.core.string;
import forge.log.exceptions;
import forge.variant.format;
import forge.variant.value;

namespace {

std::mutex& console_mutex() {
   // Like named logger state, console logging remains usable in static destructors.
   static auto* mutex = new std::mutex;
   return *mutex;
}

std::string sanitize_value(std::string value, bool redacted) {
   if (redacted) {
      return "<redacted>";
   }
   return value;
}

void write_json_string(std::ostream& out, std::string_view value) {
   auto escaped = std::string{value};
   forge::escape_str(escaped, forge::escape_control_chars::on);
   out << '"' << escaped << '"';
}

class locked_file {
 public:
   locked_file(std::filesystem::path path, bool append) {
      auto mode = std::ios::out;
      if (append) {
         mode |= std::ios::app;
      } else {
         mode |= std::ios::trunc;
      }
      stream.open(path, mode);
      if (!stream) {
         throw forge::log::exceptions::io_error{"failed to open log file: " + path.generic_string()};
      }
   }

   std::mutex mutex;
   std::ofstream stream;
};

} // namespace

namespace forge {

static thread_local std::string thread_name;

void set_thread_name(const std::string& name) {
   thread_name = name;
#if defined(__linux__) || defined(__FreeBSD__)
   pthread_setname_np(pthread_self(), name.c_str());
#elif defined(__APPLE__)
   pthread_setname_np(name.c_str());
#endif
}

const std::string& get_thread_name() {
   if (thread_name.empty()) {
      try {
         thread_name = boost::dll::program_location().filename().generic_string();
      } catch (...) {
         thread_name = "unknown";
      }
   }
   return thread_name;
}

void to_variant(log_level e, variant& v) {
   switch (e) {
   case log_level::all:
      v = "all";
      return;
   case log_level::debug:
      v = "debug";
      return;
   case log_level::info:
      v = "info";
      return;
   case log_level::warn:
      v = "warn";
      return;
   case log_level::error:
      v = "error";
      return;
   case log_level::off:
      v = "off";
      return;
   }
}
void from_variant(const variant& v, log_level& e) {
   try {
      if (v.as_string() == "all")
         e = log_level::all;
      else if (v.as_string() == "debug")
         e = log_level::debug;
      else if (v.as_string() == "info")
         e = log_level::info;
      else if (v.as_string() == "warn")
         e = log_level::warn;
      else if (v.as_string() == "error")
         e = log_level::error;
      else if (v.as_string() == "off")
         e = log_level::off;
      else
         throw std::invalid_argument("Failed to cast from Variant to log_level");
   } catch (const std::exception&) {
      throw std::invalid_argument("Expected 'all|debug|info|warn|error|off'");
   }
}

std::string log_level::to_string() const {
   switch (value) {
   case log_level::all:
      return "all";
   case log_level::debug:
      return "debug";
   case log_level::info:
      return "info";
   case log_level::warn:
      return "warn";
   case log_level::error:
      return "error";
   case log_level::off:
      return "off";
   }
   return "unknown";
}

std::string format_log_value(const variant& value) {
   return format_string("${value}", mutable_variant_object{}("value", value));
}

log_fields log_field_builder::take() {
   return std::move(_fields);
}

std::string interpolate_log_message(const std::string& message, const log_fields& fields) {
   auto values = mutable_variant_object{};
   for (const auto& field : fields) {
      values(field.key, sanitize_value(field.value, field.redacted));
   }
   return format_string(message, values);
}

void append_log_field(log_fields& fields, log_field field) {
   if (!field.key.empty()) {
      field.value = sanitize_value(std::move(field.value), field.redacted);
      fields.push_back(std::move(field));
   }
}

stacktrace_snapshot capture_stacktrace(std::size_t skip, std::size_t max_frames) {
   auto result = stacktrace_snapshot{};
#if FORGE_HAS_STD_STACKTRACE
   result.backend = "std::stacktrace";
   const auto trace = std::stacktrace::current(skip + 1, max_frames);
   result.frames.reserve(trace.size());
   for (std::size_t index = 0; index < trace.size(); ++index) {
      const auto& frame = trace[index];
      result.frames.push_back(stacktrace_frame{
          .index = index,
          .address = reinterpret_cast<std::uintptr_t>(frame.native_handle()),
          .name = frame.description(),
          .source_file = frame.source_file(),
          .source_line = frame.source_line(),
      });
   }
#elif FORGE_HAS_BOOST_STACKTRACE
   result.backend = "boost::stacktrace";
   const auto trace = boost::stacktrace::stacktrace(skip + 1, max_frames);
   result.frames.reserve(trace.size());
   for (std::size_t index = 0; index < trace.size(); ++index) {
      const auto& frame = trace[index];
      result.frames.push_back(stacktrace_frame{
          .index = index,
          .address = reinterpret_cast<std::uintptr_t>(frame.address()),
          .name = frame.name(),
          .source_file = frame.source_file(),
          .source_line = static_cast<std::uint64_t>(frame.source_line()),
      });
   }
#else
   static_cast<void>(skip);
   static_cast<void>(max_frames);
   result.backend = "none";
   result.unavailable_reason = "stacktrace_unavailable";
#endif
   if (result.frames.empty() && result.unavailable_reason.empty()) {
      result.unavailable_reason = "stacktrace_unavailable";
   }
   return result;
}

std::string format_stacktrace(const stacktrace_snapshot& stacktrace) {
   if (!stacktrace.available()) {
      return stacktrace.unavailable_reason.empty() ? "stacktrace_unavailable" : stacktrace.unavailable_reason;
   }

   auto out = std::ostringstream{};
   out << stacktrace.backend;
   for (const auto& frame : stacktrace.frames) {
      out << "\n#" << frame.index << ' ';
      if (!frame.name.empty()) {
         out << frame.name;
      } else {
         out << "0x" << std::hex << frame.address << std::dec;
      }
      if (!frame.source_file.empty()) {
         out << " at " << frame.source_file << ':' << frame.source_line;
      }
   }
   return out.str();
}

std::string format_text_log_record(const log_record& record) {
   auto out = std::ostringstream{};
   out << '[' << forge::chrono::iso8601::format(record.timestamp) << "] ";
   out << record.level.to_string() << ' ';
   if (!record.logger.empty()) {
      out << record.logger << ' ';
   }
   if (!record.component.empty()) {
      out << record.component << ' ';
   }
   out << record.message;
   for (const auto& field : record.fields) {
      out << ' ' << field.key << '=' << sanitize_value(field.value, field.redacted);
   }
   if (!record.exception_chain.empty()) {
      out << " exception=" << record.exception_chain;
   }
   if (record.stacktrace) {
      out << "\n" << format_stacktrace(*record.stacktrace);
   }
   return out.str();
}

std::string format_json_log_record(const log_record& record) {
   auto out = std::ostringstream{};
   out << '{';
   out << "\"timestamp\":";
   write_json_string(out, forge::chrono::iso8601::format(record.timestamp));
   out << ",\"level\":";
   write_json_string(out, record.level.to_string());
   out << ",\"logger\":";
   write_json_string(out, record.logger);
   out << ",\"component\":";
   write_json_string(out, record.component);
   out << ",\"message\":";
   write_json_string(out, record.message);
   out << ",\"thread_id\":";
   write_json_string(out, record.thread_id);
   out << ",\"thread\":";
   write_json_string(out, record.thread_name);
   out << ",\"file\":";
   write_json_string(out, record.location.file_name());
   out << ",\"line\":" << record.location.line();
   out << ",\"fields\":{";
   for (std::size_t index = 0; index < record.fields.size(); ++index) {
      if (index != 0) {
         out << ',';
      }
      write_json_string(out, record.fields[index].key);
      out << ':';
      write_json_string(out, sanitize_value(record.fields[index].value, record.fields[index].redacted));
   }
   out << '}';
   if (!record.exception_chain.empty()) {
      out << ",\"exception\":";
      write_json_string(out, record.exception_chain);
   }
   if (record.stacktrace) {
      out << ",\"stacktrace\":{\"backend\":";
      write_json_string(out, record.stacktrace->backend);
      out << ",\"available\":" << (record.stacktrace->available() ? "true" : "false");
      if (!record.stacktrace->unavailable_reason.empty()) {
         out << ",\"reason\":";
         write_json_string(out, record.stacktrace->unavailable_reason);
      }
      out << ",\"frames\":[";
      for (std::size_t index = 0; index < record.stacktrace->frames.size(); ++index) {
         const auto& frame = record.stacktrace->frames[index];
         if (index != 0) {
            out << ',';
         }
         out << "{\"index\":" << frame.index << ",\"name\":";
         write_json_string(out, frame.name);
         out << ",\"file\":";
         write_json_string(out, frame.source_file);
         out << ",\"line\":" << frame.source_line << '}';
      }
      out << "]}";
   }
   out << '}';
   return out.str();
}

console_sink::console_sink(bool stderr_for_warnings)
    : _stream(stderr_for_warnings ? console_stream::by_level : console_stream::standard_out) {}
console_sink::console_sink(console_stream stream) : _stream(stream) {}
console_sink::~console_sink() = default;

void console_sink::log(const log_record& record) {
   const auto use_stderr = _stream == console_stream::standard_error ||
                           (_stream == console_stream::by_level && record.level >= log_level::warn);
   auto& out = use_stderr ? std::cerr : std::cout;
   const auto text = format_text_log_record(record);
   std::lock_guard lock(console_mutex());
   out << text << '\n';
}

class file_sink::impl : public locked_file {
 public:
   impl(std::filesystem::path path, bool append) : locked_file(std::move(path), append) {}
};

file_sink::file_sink(std::filesystem::path path, bool append)
    : impl_(std::make_unique<impl>(std::move(path), append)) {}
file_sink::~file_sink() = default;

void file_sink::log(const log_record& record) {
   const auto lock = std::scoped_lock{impl_->mutex};
   impl_->stream << format_text_log_record(record) << '\n';
   impl_->stream.flush();
}

class jsonl_sink::impl : public locked_file {
 public:
   impl(std::filesystem::path path, bool append) : locked_file(std::move(path), append) {}
};

jsonl_sink::jsonl_sink(std::filesystem::path path, bool append)
    : impl_(std::make_unique<impl>(std::move(path), append)) {}
jsonl_sink::~jsonl_sink() = default;

void jsonl_sink::log(const log_record& record) {
   const auto lock = std::scoped_lock{impl_->mutex};
   impl_->stream << format_json_log_record(record) << '\n';
   impl_->stream.flush();
}

} // namespace forge
