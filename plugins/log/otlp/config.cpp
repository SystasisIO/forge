module;

#include <forge/exceptions/macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>

module forge.plugins.log.otlp.plugin;

import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.value;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.net.http.types;
import forge.log.log_message;
import forge.otlp.options;
import forge.otlp.crash;
import forge.plugins.log.otlp.exceptions;
import forge.plugins.log.otlp.types;
import forge.variant.value;

#include "details/config.hxx"

namespace forge::plugins::log::otlp {
namespace {

std::chrono::milliseconds ms(std::uint64_t value) {
   return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(value)};
}

forge::otlp::attribute to_otlp_attribute(const attribute& value) {
   return forge::otlp::attribute{.key = value.key, .value = value.value};
}

void validate_endpoint(const std::string& value) {
   try {
      const auto parsed = forge::net::http::parse_base_url(value);
      if (parsed.scheme != "http" && parsed.scheme != "https") {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP logs endpoint must use http or https");
      }
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP logs endpoint is invalid",
                            forge::exceptions::ctx("error", error.what()));
   }
}

void validate_logs_path(const std::string& value) {
   if (value.empty() || value.front() != '/') {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP logs path must be absolute");
   }
   for (const auto ch : value) {
      if (ch == '\r' || ch == '\n' || ch == '\0') {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP logs path contains an unsafe control byte");
      }
   }
}

void validate_logger_name(const std::string& value) {
   for (const auto ch : value) {
      const auto byte = static_cast<unsigned char>(ch);
      if (byte < 0x20U || byte == 0x7fU) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP logs logger name contains an unsafe control byte");
      }
   }
}

void validate_header(const header& value, const forge::config::core::value::object_type& source) {
   const auto has_literal = source.contains("value");
   const auto has_secret_id = source.contains("secret-id");
   const auto has_purpose = source.contains("purpose");
   if (has_secret_id != has_purpose || has_literal == has_secret_id) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "OTLP header must use exactly one source: value or secret-id with purpose",
                            forge::exceptions::ctx("header", value.name));
   }
   if (has_secret_id && (!value.secret_id || !value.purpose || value.secret_id->empty() || value.purpose->empty())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP secret header requires non-empty secret-id and purpose",
                            forge::exceptions::ctx("header", value.name));
   }
   if (has_literal) {
      validate_header_value(value.name, value.value);
   }
}

} // namespace

void validate_header_value(std::string_view name, std::string_view value) {
   try {
      const auto candidate = std::array<forge::net::http::header_entry, 1>{
          forge::net::http::header_entry{.name = std::string{name}, .text = std::string{value}}};
      forge::net::http::validate_provider_headers(candidate);
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP header is invalid",
                            forge::exceptions::ctx("header", name), forge::exceptions::ctx("reason", error.what()));
   }
}

forge::log_level parse_log_level(std::string_view value) {
   try {
      return forge::variant{std::string{value}}.as<forge::log_level>();
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP logs logger level is invalid",
                            forge::exceptions::ctx("level", value), forge::exceptions::ctx("error", error.what()));
   }
}

config decode_config(const forge::config::core::component_view& view) {
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, forge::config::core::format_decode_diagnostics(
                                                            "invalid OTLP logs config", decoded.diagnostics));
   }
   validate_endpoint(decoded.value.endpoint);
   validate_logs_path(decoded.value.logs_path);
   if (!decoded.value.headers.empty()) {
      const auto* source = view.try_get("headers");
      const auto* headers = source ? source->as_array() : nullptr;
      if (!headers || headers->size() != decoded.value.headers.size()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP headers did not preserve their config shape");
      }
      for (std::size_t index = 0; index < decoded.value.headers.size(); ++index) {
         const auto* header_source = (*headers)[index].as_object();
         if (!header_source) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "OTLP header must be an object");
         }
         validate_header(decoded.value.headers[index], *header_source);
      }
   }
   for (const auto& route : decoded.value.loggers) {
      validate_logger_name(route.name);
      (void)parse_log_level(route.level);
   }
   return std::move(decoded.value);
}

forge::otlp::log_exporter_options make_exporter_options(const config& value) {
   auto options = forge::otlp::log_exporter_options{};
   options.endpoint = value.endpoint;
   options.logs_path = value.logs_path;
   options.resource.attributes.reserve(value.resource.attributes.size());
   for (const auto& item : value.resource.attributes) {
      options.resource.attributes.push_back(to_otlp_attribute(item));
   }
   options.batch.max_records = static_cast<std::size_t>(value.batch.max_records);
   options.batch.max_bytes = static_cast<std::size_t>(value.batch.max_bytes);
   options.batch.flush_interval = ms(value.batch.flush_interval_ms);
   options.queue.max_records = static_cast<std::size_t>(value.queue.max_records);
   options.queue.max_bytes = static_cast<std::size_t>(value.queue.max_bytes);
   options.retry.max_attempts = static_cast<std::size_t>(value.retry.max_attempts);
   options.retry.base_delay = ms(value.retry.base_delay_ms);
   options.retry.max_delay = ms(value.retry.max_delay_ms);
   options.request_timeout = ms(value.request_timeout_ms);
   options.shutdown_timeout = ms(value.shutdown_timeout_ms);
   return options;
}

forge::otlp::crash_spool_options make_crash_spool_options(const config& value) {
   auto options = forge::otlp::crash_spool_options{};
   options.directory = std::filesystem::path{value.crash_spool.directory};
   return options;
}

} // namespace forge::plugins::log::otlp
