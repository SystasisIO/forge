module;
#include <boost/date_time/posix_time/posix_time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

module forge.chrono.iso8601;

import forge.chrono.timestamp;

namespace forge::chrono::iso8601 {
namespace {

constexpr auto nanoseconds_per_second = std::int64_t{1'000'000'000};
constexpr auto microseconds_per_second = std::int64_t{1'000'000};
constexpr auto legacy_boost_first_second = std::chrono::sys_seconds{std::chrono::sys_days{
    std::chrono::year_month_day{std::chrono::year{1400}, std::chrono::month{1}, std::chrono::day{1}}}};
constexpr auto legacy_boost_last_second = std::chrono::sys_seconds{std::chrono::sys_days{std::chrono::year_month_day{
                                              std::chrono::year{9999}, std::chrono::month{12}, std::chrono::day{31}}}} +
                                          std::chrono::hours{23} + std::chrono::minutes{59} + std::chrono::seconds{59};

[[nodiscard]] boost::posix_time::ptime epoch() {
   return boost::posix_time::from_time_t(0);
}

[[nodiscard]] boost::posix_time::ptime parse_iso_time(std::string_view value) {
   const auto text = std::string{value};
   if (text.size() >= 5 && text.at(4) == '-') {
      return boost::date_time::parse_delimited_time<boost::posix_time::ptime>(text, 'T');
   }
   return boost::posix_time::from_iso_string(text);
}

[[noreturn]] void throw_invalid_rfc3339(std::string_view reason) {
   throw std::invalid_argument{"invalid RFC3339 timestamp: " + std::string{reason}};
}

void append_fixed_decimal(std::string& output, unsigned value, unsigned width) {
   auto buffer = std::array<char, 10>{};
   const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
   if (result.ec != std::errc{} || static_cast<unsigned>(result.ptr - buffer.data()) > width) {
      throw std::out_of_range{"RFC3339 timestamp is outside the supported range"};
   }
   output.append(width - static_cast<unsigned>(result.ptr - buffer.data()), '0');
   output.append(buffer.data(), result.ptr);
}

[[nodiscard]] unsigned parse_decimal(std::string_view value, std::size_t offset, std::size_t count) {
   if (count == 0 || offset > value.size() || count > value.size() - offset) {
      throw_invalid_rfc3339("truncated decimal field");
   }

   auto parsed = unsigned{};
   const auto first = value.data() + static_cast<std::ptrdiff_t>(offset);
   const auto last = first + static_cast<std::ptrdiff_t>(count);
   const auto result = std::from_chars(first, last, parsed);
   if (result.ec != std::errc{} || result.ptr != last) {
      throw_invalid_rfc3339("invalid decimal field");
   }
   return parsed;
}

[[nodiscard]] constexpr std::pair<std::int64_t, std::int64_t> decompose_nanoseconds(std::int64_t nanoseconds) noexcept {
   auto seconds = nanoseconds / nanoseconds_per_second;
   auto fraction = nanoseconds % nanoseconds_per_second;
   if (fraction < 0) {
      --seconds;
      fraction += nanoseconds_per_second;
   }
   return {seconds, fraction};
}

[[nodiscard]] constexpr std::pair<std::int64_t, std::int64_t> decompose_microseconds(std::int64_t microseconds) noexcept {
   auto seconds = microseconds / microseconds_per_second;
   auto fraction = microseconds % microseconds_per_second;
   if (fraction < 0) {
      --seconds;
      fraction += microseconds_per_second;
   }
   return {seconds, fraction};
}

void require_legacy_boost_range(std::int64_t seconds) {
   if (seconds < legacy_boost_first_second.time_since_epoch().count() ||
       seconds > legacy_boost_last_second.time_since_epoch().count()) {
      throw std::out_of_range{"legacy ISO timestamp is outside the supported Boost Gregorian range"};
   }
}

[[nodiscard]] std::int64_t checked_nanoseconds(std::int64_t seconds, std::int64_t fraction) {
   constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
   constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
   constexpr auto minimum_parts = decompose_nanoseconds(minimum);
   constexpr auto maximum_parts = decompose_nanoseconds(maximum);
   if (fraction < 0 || fraction >= nanoseconds_per_second || seconds < minimum_parts.first ||
       seconds > maximum_parts.first || (seconds == minimum_parts.first && fraction < minimum_parts.second) ||
       (seconds == maximum_parts.first && fraction > maximum_parts.second)) {
      throw_invalid_rfc3339("timestamp is outside the nanosecond range");
   }

   if (seconds >= 0) {
      return (seconds * nanoseconds_per_second) + fraction;
   }

   if (seconds == minimum_parts.first && fraction == minimum_parts.second) {
      return minimum;
   }

   const auto magnitude_seconds = -seconds;
   if (fraction == 0) {
      return -(magnitude_seconds * nanoseconds_per_second);
   }

   const auto magnitude = ((magnitude_seconds - 1) * nanoseconds_per_second) + (nanoseconds_per_second - fraction);
   return -magnitude;
}

} // namespace

std::string format_compact(std::chrono::sys_seconds value) {
   require_legacy_boost_range(value.time_since_epoch().count());
   const auto ptime = epoch() + boost::posix_time::seconds{value.time_since_epoch().count()};
   return boost::posix_time::to_iso_string(ptime);
}

std::string format(std::chrono::sys_seconds value) {
   require_legacy_boost_range(value.time_since_epoch().count());
   const auto ptime = epoch() + boost::posix_time::seconds{value.time_since_epoch().count()};
   return boost::posix_time::to_iso_extended_string(ptime);
}

std::chrono::sys_seconds parse_seconds(std::string_view value) {
   try {
      const auto point = parse_iso_time(value);
      const auto seconds = (point - epoch()).total_seconds();
      return std::chrono::sys_seconds{std::chrono::seconds{seconds}};
   } catch (const std::exception&) {
      throw std::invalid_argument{"unable to convert ISO-formatted string to std::chrono::sys_seconds: " +
                                  std::string{value}};
   }
}

std::string format(std::chrono::sys_time<std::chrono::microseconds> value) {
   const auto timestamp = decompose_microseconds(value.time_since_epoch().count());
   require_legacy_boost_range(timestamp.first);
   const auto ptime = epoch() + boost::posix_time::seconds{timestamp.first};
   const auto base = boost::posix_time::to_iso_extended_string(ptime);
   if ((timestamp.second % 1000) == 0) {
      return base + "." + std::to_string((timestamp.second / 1000) + 1000).substr(1);
   }
   return base + "." + std::to_string(timestamp.second + microseconds_per_second).substr(1);
}

std::chrono::sys_time<std::chrono::microseconds> parse_microseconds(std::string_view value) {
   try {
      const auto text = std::string{value};
      const auto decimal = text.find('.');
      if (decimal == std::string::npos) {
         return std::chrono::sys_time<std::chrono::microseconds>{
             std::chrono::duration_cast<std::chrono::microseconds>(parse_seconds(text).time_since_epoch())};
      }

      const auto fraction_end = text.find_first_not_of("0123456789", decimal + 1);
      const auto end = fraction_end == std::string::npos ? text.size() : fraction_end;
      const auto digits = end - decimal - 1;
      if (digits == 0 || digits > 6 || end != text.size()) {
         throw std::invalid_argument{"fractional seconds must contain one to six digits"};
      }

      auto microseconds = std::int64_t{};
      const auto parsed = std::from_chars(text.data() + static_cast<std::ptrdiff_t>(decimal + 1),
                                          text.data() + static_cast<std::ptrdiff_t>(end), microseconds);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + static_cast<std::ptrdiff_t>(end)) {
         throw std::invalid_argument{"invalid fractional microseconds"};
      }
      for (auto index = digits; index < 6; ++index) {
         microseconds *= 10;
      }

      const auto base =
          std::chrono::sys_time<std::chrono::microseconds>{std::chrono::duration_cast<std::chrono::microseconds>(
              parse_seconds(text.substr(0, decimal)).time_since_epoch())};
      return base + std::chrono::microseconds{microseconds};
   } catch (const std::exception&) {
      throw std::invalid_argument{"unable to convert ISO-formatted string to std::chrono::sys_time<microseconds>: " +
                                  std::string{value}};
   }
}

std::string format_rfc3339(std::chrono::sys_time<std::chrono::nanoseconds> value) {
   return format_rfc3339(forge::chrono::timestamp{value});
}

std::string format_rfc3339(forge::chrono::timestamp value) {
   const auto whole_seconds = value.whole_seconds();
   constexpr auto first = std::chrono::sys_seconds{std::chrono::sys_days{
       std::chrono::year{0} / std::chrono::January / 1}};
   // Check before calendar conversion, whose year representation is narrower.
   if (whole_seconds < first || whole_seconds > legacy_boost_last_second) {
      throw std::out_of_range{"RFC3339 timestamp is outside the supported range"};
   }
   const auto day = std::chrono::floor<std::chrono::days>(whole_seconds);
   const auto date = std::chrono::year_month_day{day};
   const auto year = static_cast<int>(date.year());
   if (!date.ok() || year < 0 || year > 9999) {
      throw std::out_of_range{"RFC3339 timestamp is outside the supported range"};
   }

   const auto time = std::chrono::hh_mm_ss<std::chrono::seconds>{whole_seconds - day};
   auto output = std::string{};
   output.reserve(30);
   append_fixed_decimal(output, static_cast<unsigned>(year), 4);
   output.push_back('-');
   append_fixed_decimal(output, static_cast<unsigned>(date.month()), 2);
   output.push_back('-');
   append_fixed_decimal(output, static_cast<unsigned>(date.day()), 2);
   output.push_back('T');
   append_fixed_decimal(output, static_cast<unsigned>(time.hours().count()), 2);
   output.push_back(':');
   append_fixed_decimal(output, static_cast<unsigned>(time.minutes().count()), 2);
   output.push_back(':');
   append_fixed_decimal(output, static_cast<unsigned>(time.seconds().count()), 2);

   if (value.subsecond().count() != 0) {
      auto digits = std::string{};
      digits.reserve(9);
      append_fixed_decimal(digits, static_cast<unsigned>(value.subsecond().count()), 9);
      while (digits.back() == '0') {
         digits.pop_back();
      }
      output.push_back('.');
      output += digits;
   }

   output.push_back('Z');
   return output;
}

namespace {

[[nodiscard]] forge::chrono::timestamp parse_rfc3339_value(std::string_view value, bool truncate_fraction) {
   if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
       value[16] != ':') {
      throw_invalid_rfc3339("invalid shape");
   }

   const auto year = parse_decimal(value, 0, 4);
   const auto month = parse_decimal(value, 5, 2);
   const auto day = parse_decimal(value, 8, 2);
   const auto hour = parse_decimal(value, 11, 2);
   const auto minute = parse_decimal(value, 14, 2);
   const auto second = parse_decimal(value, 17, 2);
   const auto date = std::chrono::year_month_day{std::chrono::year{static_cast<int>(year)}, std::chrono::month{month},
                                                 std::chrono::day{day}};
   if (!date.ok() || hour > 23 || minute > 59 || second > 59) {
      throw_invalid_rfc3339("invalid date or time");
   }

   auto offset = std::size_t{19};
   auto fraction = std::int64_t{};
   if (offset < value.size() && value[offset] == '.') {
      const auto first_digit = ++offset;
      while (offset < value.size() && value[offset] >= '0' && value[offset] <= '9') {
         ++offset;
      }
      const auto digits = offset - first_digit;
      if (digits == 0 || (!truncate_fraction && digits > 9)) {
         throw_invalid_rfc3339("fractional seconds must contain one to nine digits");
      }
      const auto retained = std::min<std::size_t>(digits, 9);
      fraction = parse_decimal(value, first_digit, retained);
      for (auto index = retained; index < 9; ++index) {
         fraction *= 10;
      }
   }

   auto timezone_seconds = std::int64_t{};
   if (offset < value.size() && value[offset] == 'Z') {
      ++offset;
   } else if (offset < value.size() && (value[offset] == '+' || value[offset] == '-')) {
      const auto negative = value[offset++] == '-';
      if (offset + 5 != value.size() || value[offset + 2] != ':') {
         throw_invalid_rfc3339("invalid timezone offset");
      }
      const auto timezone_hour = parse_decimal(value, offset, 2);
      const auto timezone_minute = parse_decimal(value, offset + 3, 2);
      if (timezone_hour > 23 || timezone_minute > 59) {
         throw_invalid_rfc3339("timezone offset is out of range");
      }
      timezone_seconds = static_cast<std::int64_t>(timezone_hour * 60U + timezone_minute) * 60;
      if (negative) {
         timezone_seconds = -timezone_seconds;
      }
      offset += 5;
   } else {
      throw_invalid_rfc3339("missing timezone");
   }

   if (offset != value.size()) {
      throw_invalid_rfc3339("trailing data");
   }

   const auto local = std::chrono::sys_seconds{std::chrono::sys_days{date}} + std::chrono::hours{hour} +
                      std::chrono::minutes{minute} + std::chrono::seconds{second};
   return forge::chrono::timestamp{local - std::chrono::seconds{timezone_seconds},
                                   std::chrono::nanoseconds{fraction}};
}

} // namespace

forge::chrono::timestamp parse_rfc3339_timestamp(std::string_view value) {
   return parse_rfc3339_value(value, true);
}

std::chrono::sys_time<std::chrono::nanoseconds> parse_rfc3339(std::string_view value) {
   const auto parsed = parse_rfc3339_value(value, false);
   return std::chrono::sys_time<std::chrono::nanoseconds>{
       std::chrono::nanoseconds{checked_nanoseconds(parsed.whole_seconds().time_since_epoch().count(),
                                                    parsed.subsecond().count())}};
}

} // namespace forge::chrono::iso8601
