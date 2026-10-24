#define BOOST_TEST_MODULE forge_raw_bounded_consumer
#include <boost/test/included/unit_test.hpp>
#include <boost/describe.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

import forge.raw.raw;
import forge.raw.exceptions;

bool portable_limits_preserve_legacy_wire();

enum class byte_mode : std::uint8_t { one = 1, two = 2 };
enum class narrow_mode : std::uint8_t { one = 1 };
enum class unvalidated_mode : std::uint8_t { one = 1 };
BOOST_DESCRIBE_ENUM(narrow_mode, one)

namespace forge::raw {
template <> struct enum_wire_type<byte_mode> {
   using type = std::uint8_t;
   static constexpr bool shared = true;
   static constexpr bool is_valid(type value) { return value == 1U || value == 2U; }
};
} // namespace forge::raw

namespace {

constexpr auto limits = forge::raw::bounded_unpack_limits{
    .allocations = {.max_container_elements = 4U,
                    .max_total_container_elements = 6U,
                    .max_bytes = 8U,
                    .first_container_elements = 4U},
    .max_total_bytes = 12U,
    .reject_noncanonical_varints = true,
    .reject_unknown_enum_values = true};

struct custom_value {
   std::vector<std::vector<std::uint32_t>> values;

   template <typename Stream> friend Stream& operator<<(Stream& stream, const custom_value& value) {
      return stream << value.values;
   }
   template <typename Stream> friend Stream& operator>>(Stream& stream, custom_value& value) {
      return stream >> value.values;
   }
};

template <typename T> T decode(const forge::raw::bytes& bytes, forge::raw::bounded_unpack_limits selected = limits) {
   return forge::raw::unpack_bounded_exact<T>(std::span<const std::uint8_t>{bytes}, selected);
}

} // namespace

BOOST_AUTO_TEST_CASE(bounded_raw_golden_and_legacy_options_wire_are_unchanged) {
   BOOST_CHECK(portable_limits_preserve_legacy_wire());
   const auto value = custom_value{.values = {{7U, 11U}, {42U}}};
   const auto bytes = forge::raw::pack(value);
   BOOST_TEST(bytes == forge::raw::pack(value.values), boost::test_tools::per_element());
   BOOST_CHECK(decode<custom_value>(bytes).values == value.values);
   BOOST_TEST(forge::raw::pack(limits.allocations) == forge::raw::pack(std::uint32_t{4}, std::uint32_t{6},
                                                          std::uint32_t{8}, std::uint32_t{4}),
              boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(bounded_raw_rejects_counts_before_resize_in_pointer_and_span_streams) {
   const auto bytes = forge::raw::pack(forge::unsigned_int{std::numeric_limits<std::uint32_t>::max()});
   auto values = std::vector<std::uint32_t>{};
   BOOST_CHECK_THROW(forge::raw::unpack_bounded_exact(std::span<const std::uint8_t>{bytes}, values, limits),
                     forge::raw::exceptions::codec_error);
   BOOST_CHECK(values.empty());

   const auto small_count = forge::raw::pack(forge::unsigned_int{5U});
   auto stream = forge::raw::make_input_stream(small_count.data(), small_count.size(), limits);
   BOOST_CHECK_THROW(forge::raw::unpack(stream, values), forge::raw::exceptions::allocation_limit);
   BOOST_CHECK(values.empty());

   auto bytes_value = forge::raw::bytes{};
   const auto length = forge::raw::pack(forge::unsigned_int{9U});
   BOOST_CHECK_THROW(forge::raw::unpack_bounded_exact(std::span<const std::uint8_t>{length}, bytes_value, limits),
                     forge::raw::exceptions::allocation_limit);
   BOOST_CHECK(bytes_value.empty());
}

BOOST_AUTO_TEST_CASE(bounded_raw_custom_streams_share_cumulative_container_and_byte_budgets) {
   auto selected = limits;
   selected.allocations.max_total_container_elements = 5U;
   const auto bytes = forge::raw::pack(custom_value{.values = {{1U, 2U}, {3U, 4U}}});
   auto value = custom_value{};
   auto stream = forge::raw::make_input_stream(bytes.data(), bytes.size(), selected);
   BOOST_CHECK_THROW(forge::raw::unpack(stream, value), forge::raw::exceptions::allocation_limit);
   BOOST_REQUIRE_EQUAL(value.values.size(), 2U);
   BOOST_CHECK(value.values[1].empty());

   const auto strings = forge::raw::pack(std::vector<std::string>{"abc", "def"});
   selected.max_total_bytes = 5U;
   auto output = std::vector<std::string>{};
   BOOST_CHECK_THROW(forge::raw::unpack_bounded_exact(std::span<const std::uint8_t>{strings}, output, selected),
                     forge::raw::exceptions::allocation_limit);
   BOOST_REQUIRE_EQUAL(output.size(), 2U);
   BOOST_CHECK(output[1].empty());
}

BOOST_AUTO_TEST_CASE(bounded_raw_enum_domain_checks_precede_narrowing_and_cover_bulk_arrays) {
   BOOST_CHECK(decode<byte_mode>(forge::raw::pack(byte_mode::one)) == byte_mode::one);
   BOOST_CHECK_THROW(decode<byte_mode>(forge::raw::pack(std::uint8_t{3})), forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(decode<unvalidated_mode>(forge::raw::pack(std::uint8_t{1})),
                     forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(decode<narrow_mode>(forge::raw::pack(std::int64_t{257})),
                     forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(decode<narrow_mode>(forge::raw::pack(std::int64_t{-255})),
                     forge::raw::exceptions::codec_error);
   const auto legacy = forge::raw::pack(std::int64_t{257});
   BOOST_CHECK(forge::raw::unpack_exact<narrow_mode>(legacy) == narrow_mode::one);
   const auto array = std::array{byte_mode::one, static_cast<byte_mode>(3)};
   BOOST_CHECK_THROW((decode<std::array<byte_mode, 2>>(forge::raw::pack(array))),
                     forge::raw::exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(bounded_raw_rejects_trailing_nonminimal_varints_and_noncanonical_bool) {
   auto bytes = forge::raw::pack(forge::unsigned_int{0});
   bytes[0] |= 0x80U;
   bytes.push_back(0U);
   BOOST_CHECK_THROW(decode<forge::unsigned_int>(bytes), forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(decode<forge::signed_int>(bytes), forge::raw::exceptions::codec_error);
   BOOST_TEST(forge::raw::unpack_exact<forge::unsigned_int>(bytes).value == 0U);
   BOOST_CHECK_THROW(decode<std::vector<std::uint32_t>>(bytes), forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(decode<bool>(forge::raw::pack(std::uint8_t{2})), forge::raw::exceptions::codec_error);
   auto trailing = forge::raw::pack(std::uint32_t{7});
   trailing.push_back(0U);
   BOOST_CHECK_THROW(decode<std::uint32_t>(trailing), forge::raw::exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(bounded_raw_nested_frames_inherit_strict_flags_first_container_and_totals) {
   const auto frame = forge::raw::pack(std::vector<std::uint32_t>{1U, 2U});
   auto selected = limits;
   selected.allocations.first_container_elements = 1U;
   auto parent = forge::raw::make_input_stream(frame.data(), 0U, selected);
   BOOST_CHECK_THROW((forge::raw::unpack_nested_exact<std::vector<std::uint32_t>>(parent, frame)),
                     forge::raw::exceptions::allocation_limit);

   auto allowed = forge::raw::make_input_stream(frame.data(), 0U, limits);
   BOOST_TEST((forge::raw::unpack_nested_exact<std::vector<std::uint32_t>>(allowed, frame).size()) == 2U);
   BOOST_TEST(allowed.remaining_container_elements() == 4U);
   BOOST_TEST(allowed.has_consumed_container());

   const auto text = forge::raw::pack(std::string{"12345678"});
   auto byte_parent = forge::raw::make_input_stream(text.data(), 0U, limits);
   BOOST_TEST(forge::raw::unpack_nested_exact<std::string>(byte_parent, text) == "12345678");
   BOOST_CHECK_THROW((forge::raw::unpack_nested_exact<std::string>(byte_parent, text)),
                     forge::raw::exceptions::allocation_limit);

   auto overlong = forge::raw::pack(forge::unsigned_int{0U});
   overlong[0] |= 0x80U;
   overlong.push_back(0U);
   BOOST_CHECK_THROW((forge::raw::unpack_nested_exact<forge::unsigned_int>(allowed, overlong)),
                     forge::raw::exceptions::codec_error);
   const auto unknown = forge::raw::pack(std::uint8_t{3});
   BOOST_CHECK_THROW((forge::raw::unpack_nested_exact<byte_mode>(allowed, unknown)),
                     forge::raw::exceptions::codec_error);
}
