#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

import forge.chrono.iso8601;
import forge.chrono.relative;
import forge.chrono.timestamp;

BOOST_AUTO_TEST_SUITE(chrono_test_suite)

BOOST_AUTO_TEST_CASE(legacy_iso_roundtrip) {
   const auto one_second = std::chrono::sys_seconds{std::chrono::seconds{1}};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(one_second), "1970-01-01T00:00:01");
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_compact(one_second), "19700101T000001");
   BOOST_CHECK(forge::chrono::iso8601::parse_seconds("1970-01-01T00:00:01") == one_second);
   BOOST_CHECK(forge::chrono::iso8601::parse_seconds("19700101T000001") == one_second);

   const auto one_second_us = std::chrono::sys_time<std::chrono::microseconds>{std::chrono::seconds{1}};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(one_second_us), "1970-01-01T00:00:01.000");
   BOOST_CHECK(forge::chrono::iso8601::parse_microseconds("1970-01-01T00:00:01.000") == one_second_us);

   const auto precise = one_second_us + std::chrono::microseconds{123'456};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(precise), "1970-01-01T00:00:01.123456");
   BOOST_CHECK(forge::chrono::iso8601::parse_microseconds("1970-01-01T00:00:01.123456") == precise);
}

BOOST_AUTO_TEST_CASE(legacy_iso_rejects_invalid_input) {
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_seconds("not-a-time")), std::invalid_argument);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_microseconds("not-a-time")),
                     std::invalid_argument);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_microseconds("1970-01-01T00:00:01.1234567")),
                     std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(iso_seconds_do_not_inherit_fc_wire_bounds) {
   const auto before_epoch = std::chrono::sys_seconds{std::chrono::seconds{-1}};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(before_epoch), "1969-12-31T23:59:59");
   BOOST_CHECK(forge::chrono::iso8601::parse_seconds("1969-12-31T23:59:59") == before_epoch);

   const auto before_epoch_us = std::chrono::sys_time<std::chrono::microseconds>{std::chrono::microseconds{-500'000}};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(before_epoch_us), "1969-12-31T23:59:59.500");
   BOOST_CHECK(forge::chrono::iso8601::parse_microseconds("1969-12-31T23:59:59.500") == before_epoch_us);

   const auto one_microsecond_before_epoch =
       std::chrono::sys_time<std::chrono::microseconds>{std::chrono::microseconds{-1}};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(one_microsecond_before_epoch), "1969-12-31T23:59:59.999999");
   BOOST_CHECK(forge::chrono::iso8601::parse_microseconds("1969-12-31T23:59:59.999999") ==
               one_microsecond_before_epoch);

   const auto after_fc_range = std::chrono::sys_seconds{std::chrono::seconds{4'294'967'296LL}};
   BOOST_CHECK(forge::chrono::iso8601::parse_seconds("2106-02-07T06:28:16") == after_fc_range);
}

BOOST_AUTO_TEST_CASE(legacy_iso_format_enforces_boost_gregorian_range) {
   const auto first = std::chrono::sys_seconds{std::chrono::sys_days{
       std::chrono::year_month_day{std::chrono::year{1400}, std::chrono::month{1}, std::chrono::day{1}}}};
   const auto last = std::chrono::sys_seconds{std::chrono::sys_days{std::chrono::year_month_day{
                         std::chrono::year{9999}, std::chrono::month{12}, std::chrono::day{31}}}} +
                     std::chrono::hours{23} + std::chrono::minutes{59} + std::chrono::seconds{59};

   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(first), "1400-01-01T00:00:00");
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(last), "9999-12-31T23:59:59");
   BOOST_CHECK(forge::chrono::iso8601::parse_seconds("1400-01-01T00:00:00") == first);
   BOOST_CHECK(forge::chrono::iso8601::parse_seconds("9999-12-31T23:59:59") == last);

   const auto first_microseconds = std::chrono::sys_time<std::chrono::microseconds>{first.time_since_epoch()};
   const auto last_microseconds = std::chrono::sys_time<std::chrono::microseconds>{last.time_since_epoch()};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(first_microseconds), "1400-01-01T00:00:00.000");
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format(last_microseconds), "9999-12-31T23:59:59.000");
   BOOST_CHECK(forge::chrono::iso8601::parse_microseconds("1400-01-01T00:00:00.000") == first_microseconds);
   BOOST_CHECK(forge::chrono::iso8601::parse_microseconds("9999-12-31T23:59:59.000") == last_microseconds);

   const auto minimum_seconds =
       std::chrono::sys_seconds{std::chrono::seconds{std::numeric_limits<std::int64_t>::min()}};
   const auto maximum_seconds =
       std::chrono::sys_seconds{std::chrono::seconds{std::numeric_limits<std::int64_t>::max()}};
   const auto minimum_microseconds = std::chrono::sys_time<std::chrono::microseconds>{
       std::chrono::microseconds{std::numeric_limits<std::int64_t>::min()}};
   const auto maximum_microseconds = std::chrono::sys_time<std::chrono::microseconds>{
       std::chrono::microseconds{std::numeric_limits<std::int64_t>::max()}};
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::format(minimum_seconds)), std::out_of_range);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::format(maximum_seconds)), std::out_of_range);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::format(minimum_microseconds)), std::out_of_range);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::format(maximum_microseconds)), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(rfc3339_preserves_nanoseconds_and_normalizes_offsets) {
   const auto instant =
       std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::seconds{1} + std::chrono::nanoseconds{120'340'500}};
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_rfc3339(instant), "1970-01-01T00:00:01.1203405Z");
   BOOST_CHECK(forge::chrono::iso8601::parse_rfc3339("1970-01-01T01:30:01.120340500+01:30") == instant);
}

BOOST_AUTO_TEST_CASE(rfc3339_roundtrips_exact_int64_nanosecond_boundaries) {
   const auto minimum = std::chrono::sys_time<std::chrono::nanoseconds>{
       std::chrono::nanoseconds{std::numeric_limits<std::int64_t>::min()}};
   const auto maximum = std::chrono::sys_time<std::chrono::nanoseconds>{
       std::chrono::nanoseconds{std::numeric_limits<std::int64_t>::max()}};

   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_rfc3339(minimum), "1677-09-21T00:12:43.145224192Z");
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_rfc3339(maximum), "2262-04-11T23:47:16.854775807Z");
   BOOST_CHECK(forge::chrono::iso8601::parse_rfc3339("1677-09-21T00:12:43.145224192Z") == minimum);
   BOOST_CHECK(forge::chrono::iso8601::parse_rfc3339("2262-04-11T23:47:16.854775807Z") == maximum);

   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339("1677-09-21T00:12:43.145224191Z")),
                     std::invalid_argument);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339("2262-04-11T23:47:16.854775808Z")),
                     std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(rfc3339_rejects_invalid_or_lossy_input) {
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339("1970-02-30T00:00:00Z")),
                     std::invalid_argument);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339("1970-01-01T00:00:00.1234567890Z")),
                     std::invalid_argument);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339("1970-01-01T00:00:00Ztrailing")),
                     std::invalid_argument);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339("1970-01-01T00:00:00+24:00")),
                     std::invalid_argument);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339("9999-01-01T00:00:00Z")),
                     std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(wide_timestamp_roundtrips_negative_epochs_and_int64_boundaries) {
   for (const auto count : {std::numeric_limits<std::int64_t>::min(),
                            std::numeric_limits<std::int64_t>::min() + 1,
                            std::int64_t{-1'000'000'001}, std::int64_t{-1'000'000'000},
                            std::int64_t{-1}, std::int64_t{0}, std::int64_t{1},
                            std::numeric_limits<std::int64_t>::max()}) {
      const auto narrow = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{count}};
      const auto wide = forge::chrono::timestamp{narrow};
      BOOST_CHECK(wide.subsecond() >= std::chrono::nanoseconds::zero());
      BOOST_CHECK(wide.subsecond() < std::chrono::seconds{1});
      const auto text = forge::chrono::iso8601::format_rfc3339(wide);
      BOOST_CHECK(forge::chrono::iso8601::parse_rfc3339_timestamp(text) == wide);
      BOOST_CHECK(forge::chrono::iso8601::parse_rfc3339(text) == narrow);
   }
   const auto negative = forge::chrono::timestamp{
       std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{-1}}};
   BOOST_CHECK_EQUAL(negative.whole_seconds().time_since_epoch().count(), -1);
   BOOST_CHECK_EQUAL(negative.subsecond().count(), 999'999'999);
   BOOST_CHECK(negative < forge::chrono::timestamp{});
}

BOOST_AUTO_TEST_CASE(rfc3339_existing_standard_time_conversions_remain_unambiguous) {
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_rfc3339(std::chrono::sys_seconds{}),
                     "1970-01-01T00:00:00Z");
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_rfc3339(
                         std::chrono::sys_time<std::chrono::microseconds>{std::chrono::microseconds{1}}),
                     "1970-01-01T00:00:00.000001Z");
}

BOOST_AUTO_TEST_CASE(wide_rfc3339_preserves_year_range_and_fraction_policy) {
   for (const auto text : {"0000-01-01T00:00:00Z", "1969-12-31T23:59:59.999999999Z",
                           "2500-01-02T03:04:05.123456789Z", "9999-12-31T23:59:59.999999999Z"}) {
      BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_rfc3339(
                            forge::chrono::iso8601::parse_rfc3339_timestamp(text)), text);
   }
   const auto precise = forge::chrono::iso8601::parse_rfc3339_timestamp(
       "9999-12-31T23:59:59.123456789999Z");
   BOOST_CHECK_EQUAL(forge::chrono::iso8601::format_rfc3339(precise),
                     "9999-12-31T23:59:59.123456789Z");
   BOOST_CHECK(forge::chrono::iso8601::parse_rfc3339_timestamp("1970-01-01T01:30:01.120340500+01:30") ==
               forge::chrono::iso8601::parse_rfc3339_timestamp("1970-01-01T00:00:01.1203405Z"));
   BOOST_CHECK(forge::chrono::iso8601::parse_rfc3339_timestamp("1970-01-01T00:00:00-01:30") ==
               forge::chrono::iso8601::parse_rfc3339_timestamp("1970-01-01T01:30:00Z"));
   const auto last = forge::chrono::iso8601::parse_rfc3339_timestamp("9999-12-31T23:59:59.999999999Z");
   BOOST_CHECK(precise < last);
   BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::format_rfc3339(
                         forge::chrono::iso8601::parse_rfc3339_timestamp("9999-12-31T23:59:59-00:01"))),
                     std::out_of_range);
}

BOOST_AUTO_TEST_CASE(wide_timestamp_rejects_invalid_remainders_and_text) {
   BOOST_CHECK_THROW((forge::chrono::timestamp{std::chrono::sys_seconds{}, std::chrono::nanoseconds{-1}}),
                     std::invalid_argument);
   BOOST_CHECK_THROW((forge::chrono::timestamp{std::chrono::sys_seconds{}, std::chrono::seconds{1}}),
                     std::invalid_argument);
   for (const auto text : {"1970-02-30T00:00:00Z", "1970-01-01T00:00:00.Z",
                           "1970-01-01T00:00:00Ztrailing", "1970-01-01T00:00:00+24:00",
                           "1970-01-01T00:00:00", "1970-01-01T00:00:60Z",
                           "1970-01-01t00:00:00z"}) {
      BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::parse_rfc3339_timestamp(text)),
                        std::invalid_argument);
   }
   for (const auto seconds : {std::numeric_limits<std::int64_t>::min(),
                              std::numeric_limits<std::int64_t>::max()}) {
      const auto value = forge::chrono::timestamp{std::chrono::sys_seconds{std::chrono::seconds{seconds}}};
      BOOST_CHECK_THROW(static_cast<void>(forge::chrono::iso8601::format_rfc3339(value)), std::out_of_range);
   }
}

BOOST_AUTO_TEST_CASE(relative_format_retains_suffix_and_future_behavior) {
   const auto epoch = std::chrono::sys_seconds{};
   BOOST_CHECK_EQUAL(forge::chrono::relative::format(epoch, epoch + std::chrono::seconds{1}), "1 second ago");
   BOOST_CHECK_EQUAL(forge::chrono::relative::format(epoch, epoch + std::chrono::seconds{2}, " since start"),
                     "2 seconds since start");
   BOOST_CHECK_EQUAL(forge::chrono::relative::format(epoch + std::chrono::seconds{2}, epoch, " ignored"),
                     "2 seconds in the future");
   BOOST_CHECK_EQUAL(forge::chrono::relative::format(epoch, epoch + std::chrono::days{365}), "52 weeks ago");
   BOOST_CHECK_EQUAL(forge::chrono::relative::format(epoch, epoch + std::chrono::days{500}), "1 year 5 months ago");
   BOOST_CHECK_EQUAL(forge::chrono::relative::format(epoch, epoch + std::chrono::days{730}), "2 years ago");

   const auto epoch_us = std::chrono::sys_time<std::chrono::microseconds>{};
   BOOST_CHECK_EQUAL(forge::chrono::relative::format(epoch_us, epoch_us + std::chrono::seconds{1}), "1 second ago");
}

BOOST_AUTO_TEST_CASE(relative_format_handles_full_sys_seconds_range) {
   const auto minimum = std::chrono::sys_seconds{std::chrono::seconds{std::numeric_limits<std::int64_t>::min()}};
   const auto maximum = std::chrono::sys_seconds{std::chrono::seconds{std::numeric_limits<std::int64_t>::max()}};

   const auto past = forge::chrono::relative::format(minimum, maximum);
   const auto future = forge::chrono::relative::format(maximum, minimum);
   BOOST_CHECK(past.ends_with(" ago"));
   BOOST_CHECK(future.ends_with(" in the future"));
}

BOOST_AUTO_TEST_SUITE_END()
