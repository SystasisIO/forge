#pragma once

#include <string_view>

namespace forge::plugins::log::otlp {

[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
[[nodiscard]] forge::log_level parse_log_level(std::string_view value);
void validate_header_value(std::string_view name, std::string_view value);
[[nodiscard]] forge::otlp::log_exporter_options make_exporter_options(const config& value);
[[nodiscard]] forge::otlp::crash_spool_options make_crash_spool_options(const config& value);

} // namespace forge::plugins::log::otlp
