module;
#include <memory>
#include <iostream>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

module forge.log.logger;

import forge.log.record;
import forge.log.logger_config;
import forge.core.utility;
import forge.variant.value;

#include "details/sink_delivery.hxx"
#include "details/logger_impl.hxx"

namespace forge {

namespace {

std::string current_thread_id() {
   auto out = std::ostringstream{};
   out << std::this_thread::get_id();
   return out.str();
}

} // namespace

static void ensure_default_logging_configured() {
   static const bool configured = log_config::configure_logging(logging_config::default_config());
   (void)configured;
}

logger::logger() : my(std::make_shared<impl>()) {}

logger::logger(nullptr_t) {}

logger::logger(const std::string& name, const logger& parent) : my(std::make_shared<impl>(name, parent)) {}

logger::logger(const logger& l) : my(std::atomic_load_explicit(&l.my, std::memory_order_acquire)) {}

logger::logger(logger&& l) noexcept
    : my(std::atomic_exchange_explicit(&l.my, std::shared_ptr<impl>{}, std::memory_order_acq_rel)) {}

logger::~logger() {}

logger& logger::operator=(const logger& l) {
   if (this == &l) {
      return *this;
   }
   std::atomic_store_explicit(&my, std::atomic_load_explicit(&l.my, std::memory_order_acquire),
                              std::memory_order_release);
   return *this;
}
logger& logger::operator=(logger&& l) noexcept {
   if (this != &l) {
      // Preserve the existing move-assignment swap semantics. Each handle
      // replacement is atomic; the two-object swap is not a transaction.
      auto outgoing = std::atomic_load_explicit(&my, std::memory_order_acquire);
      auto incoming = std::atomic_exchange_explicit(&l.my, std::move(outgoing), std::memory_order_acq_rel);
      std::atomic_store_explicit(&my, std::move(incoming), std::memory_order_release);
   }
   return *this;
}
bool operator==(const logger& l, std::nullptr_t) {
   return !std::atomic_load_explicit(&l.my, std::memory_order_acquire);
}
bool operator!=(const logger& l, std::nullptr_t) {
   return !!std::atomic_load_explicit(&l.my, std::memory_order_acquire);
}

void logger::set_enabled(bool e) {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   if (state) {
      std::lock_guard g(log_config::get().log_mutex);
      state->_enabled = e;
   }
}
bool logger::is_enabled() const {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   std::lock_guard g(log_config::get().log_mutex);
   return state && state->_enabled;
}
bool logger::is_enabled(log_level e) const {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   std::lock_guard g(log_config::get().log_mutex);
   return state && state->_enabled && e >= state->_level && e < log_level::off;
}

void logger::log(log_record record) {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   if (!state) {
      return;
   }

   auto targets = std::vector<std::shared_ptr<sink>>{};
   {
      std::lock_guard g(log_config::get().log_mutex);
      if (!state->_enabled || record.level < state->_level || record.level >= log_level::off) {
         return;
      }
      record.logger = state->_name;
      targets = state->targets();
   }
   for (auto& field : record.fields) {
      if (field.redacted) {
         field.value = "<redacted>";
      }
   }
   record.message = interpolate_log_message(record.message, record.fields);
   detail::sink_delivery::deliver(targets, record);
}

void logger::log(log_level level, std::string message, log_fields fields, std::source_location location) {
   // Keep the same impl through filtering, preparation and delivery even if
   // this handle (notably default_logger()) is concurrently replaced.
   auto route = logger{*this};
   if (!route.is_enabled(level)) {
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
   route.log(std::move(record));
}

void logger::set_name(const std::string& n) {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   if (state) {
      std::lock_guard g(log_config::get().log_mutex);
      state->_name = n;
   }
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
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   std::lock_guard g(log_config::get().log_mutex);
   return state ? state->_name : std::string{};
}

logger logger::get(const std::string& s) {
   ensure_default_logging_configured();
   std::lock_guard lock(log_config::get().log_mutex);
   auto& loggers = log_config::get().logger_map;
   if (const auto existing = loggers.find(s); existing != loggers.end()) {
      return existing->second;
   }
   auto result = logger{s};
   if (s != default_logger_name) {
      if (const auto parent = loggers.find(default_logger_name); parent != loggers.end()) {
         // Already under the registry lock; do not call locking getters.
         const auto state = std::atomic_load_explicit(&result.my, std::memory_order_acquire);
         const auto source = std::atomic_load_explicit(&parent->second.my, std::memory_order_acquire);
         state->_parent = parent->second;
         state->_enabled = source->_enabled;
         state->_level = source->_level;
      }
   }
   loggers.emplace(s, result);
   return result;
}

logger& logger::default_logger() {
   static logger* the_default_logger = new logger;
   return *the_default_logger;
}

void logger::update(const std::string& name, logger& log) {
   ensure_default_logging_configured();
   auto route = logger{log};
   const auto state = std::atomic_load_explicit(&route.my, std::memory_order_acquire);
   if (!state) {
      throw std::invalid_argument{"cannot register null logger"};
   }
   auto retired = logger{route};
   auto retired_default = logger{route};
   {
      std::lock_guard lock(log_config::get().log_mutex);
      auto& loggers = log_config::get().logger_map;
      if (state->_name.empty()) {
         state->_name = name;
      }
      if (name != default_logger_name && state->_parent == nullptr) {
         if (const auto parent = loggers.find(default_logger_name); parent != loggers.end()) {
            state->_parent = parent->second;
         }
      }
      const auto [entry, inserted] = loggers.try_emplace(name, route);
      if (!inserted) {
         retired = std::move(entry->second);
      }
      if (name == default_logger_name) {
         retired_default = std::move(default_logger());
      }
   }
}

logger logger::get_parent() const {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   std::lock_guard g(log_config::get().log_mutex);
   return state ? state->_parent : logger{nullptr};
}
logger& logger::set_parent(const logger& p) {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   auto retired = logger{p};
   if (state) {
      std::lock_guard g(log_config::get().log_mutex);
      retired = std::move(state->_parent);
   }
   return *this;
}

log_level logger::get_log_level() const {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   std::lock_guard g(log_config::get().log_mutex);
   return state ? state->_level : log_level{log_level::off};
}
logger& logger::set_log_level(log_level ll) {
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   if (state) {
      std::lock_guard g(log_config::get().log_mutex);
      state->_level = ll;
   }
   return *this;
}

void logger::add_sink(std::shared_ptr<sink> sink) {
   if (!sink) {
      throw std::invalid_argument{"cannot add null log sink"};
   }
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   if (state) {
      std::lock_guard g(log_config::get().log_mutex);
      state->_sinks.push_back(std::move(sink));
   }
}

void logger::remove_sink(const std::shared_ptr<sink>& sink) {
   if (!sink) {
      return;
   }
   const auto state = std::atomic_load_explicit(&my, std::memory_order_acquire);
   auto retired = std::vector<std::shared_ptr<forge::sink>>{};
   if (state) {
      std::lock_guard g(log_config::get().log_mutex);
      retired.reserve(state->_sinks.size());
      auto out = state->_sinks.begin();
      for (auto in = state->_sinks.begin(); in != state->_sinks.end(); ++in) {
         if (*in == sink) {
            retired.push_back(std::move(*in));
         } else {
            if (out != in) {
               *out = std::move(*in);
            }
            ++out;
         }
      }
      state->_sinks.erase(out, state->_sinks.end());
   }
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

void report_log_failure() noexcept {
   // Allocation-free, non-recursive and bounded even when the sink repeatedly
   // fails. Exception text can contain secrets and must not reach this path.
   static auto reported = std::atomic_flag{};
   if (!reported.test_and_set(std::memory_order_relaxed)) {
      static_cast<void>(std::fputs("Forge logging failed; records may be lost\n", stderr));
   }
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
