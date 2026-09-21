#include <chrono>
#include <cstdint>
#include <limits>

import forge.chrono.iso8601;
import forge.chrono.timestamp;

int main() {
   const auto instant = std::chrono::sys_seconds{std::chrono::seconds{1}};
   const auto wide = forge::chrono::iso8601::parse_rfc3339_timestamp("9999-12-31T23:59:59.999999999Z");
   const auto minimum = std::chrono::sys_time<std::chrono::nanoseconds>{
       std::chrono::nanoseconds{std::numeric_limits<std::int64_t>::min()}};
   const auto negative = forge::chrono::timestamp{minimum};
   return forge::chrono::iso8601::format(instant) == "1970-01-01T00:00:01" &&
                  forge::chrono::iso8601::format_rfc3339(wide) == "9999-12-31T23:59:59.999999999Z" &&
                  forge::chrono::iso8601::parse_rfc3339(forge::chrono::iso8601::format_rfc3339(negative)) == minimum &&
                  negative < wide
              ? 0 : 1;
}
