module;
#include <chrono>
#include <string>
#include <string_view>

export module forge.chrono.iso8601;

import forge.chrono.timestamp;

export namespace forge::chrono::iso8601 {

[[nodiscard]] std::string format(std::chrono::sys_seconds value);
[[nodiscard]] std::string format(std::chrono::sys_time<std::chrono::microseconds> value);
[[nodiscard]] std::string format_compact(std::chrono::sys_seconds value);
[[nodiscard]] std::chrono::sys_seconds parse_seconds(std::string_view value);
[[nodiscard]] std::chrono::sys_time<std::chrono::microseconds> parse_microseconds(std::string_view value);

[[nodiscard]] std::string format_rfc3339(std::chrono::sys_time<std::chrono::nanoseconds> value);
[[nodiscard]] std::string format_rfc3339(forge::chrono::timestamp value);
// Wide RFC3339Nano parsing truncates fractional digits beyond nanoseconds.
[[nodiscard]] forge::chrono::timestamp parse_rfc3339_timestamp(std::string_view value);
[[nodiscard]] std::chrono::sys_time<std::chrono::nanoseconds> parse_rfc3339(std::string_view value);

} // namespace forge::chrono::iso8601
