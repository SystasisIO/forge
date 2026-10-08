#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

import forge.exceptions;
import forge.variant.static_variant;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

namespace forge_variant_tests {

struct scalar_text {
   std::string value;

   bool operator==(const scalar_text&) const = default;
};

void to_variant(const scalar_text& value, forge::variant& output) {
   output = std::string{"custom:"} + value.value;
}

void from_variant(const forge::variant& input, scalar_text& output) {
   output.value = input.get_string().substr(7);
}

} // namespace forge_variant_tests

BOOST_AUTO_TEST_SUITE(static_variant_test_suite)
BOOST_AUTO_TEST_CASE(static_variant_preserves_scalar_types_and_full_integer_ranges) {
   using scalar = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;
   const auto values = std::vector<scalar>{false,
                                           true,
                                           std::numeric_limits<std::int64_t>::min(),
                                           std::numeric_limits<std::uint64_t>::max(),
                                           1.25,
                                           std::string{"scalar"}};
   for (const auto& value : values) {
      auto encoded = forge::variant{};
      forge::to_variant(value, encoded);
      BOOST_REQUIRE(encoded.is_array());
      const auto& pair = encoded.get_array();
      BOOST_REQUIRE_EQUAL(pair.size(), 2U);
      BOOST_TEST(pair[0].as_uint64() == value.index());
      std::visit([&](const auto& selected) { BOOST_CHECK(pair[1] == forge::variant{selected}); }, value);
      auto restored = scalar{};
      forge::from_variant(encoded, restored);
      BOOST_CHECK(restored == value);
   }
}

BOOST_AUTO_TEST_CASE(static_variant_retains_owning_namespace_custom_conversion) {
   const auto value =
       std::variant<std::int32_t, forge_variant_tests::scalar_text>{forge_variant_tests::scalar_text{"record"}};
   auto encoded = forge::variant{};
   forge::to_variant(value, encoded);
   BOOST_TEST(encoded.get_array()[1].get_string() == "custom:record");
   auto restored = decltype(value){};
   forge::from_variant(encoded, restored);
   BOOST_CHECK(restored == value);
}

BOOST_AUTO_TEST_CASE(static_variant_still_reads_legacy_numeric_boolean_payloads) {
   auto restored = std::variant<bool, std::int32_t>{};
   forge::from_variant(forge::variant{forge::variants{forge::variant{0U}, forge::variant{1U}}}, restored);
   BOOST_REQUIRE(std::holds_alternative<bool>(restored));
   BOOST_TEST(std::get<bool>(restored));
}

BOOST_AUTO_TEST_CASE(to_from_fc_variant) {
   using variant_type = std::variant<int32_t, bool>;
   auto std_variant_1 = variant_type{false};
   auto forge_variant = forge::variant{};

   forge::to_variant(std_variant_1, forge_variant);

   auto std_variant_2 = variant_type{};
   forge::from_variant(forge_variant, std_variant_2);

   BOOST_REQUIRE(std_variant_1 == std_variant_2);
}

BOOST_AUTO_TEST_CASE(get) {
   using variant_type = std::variant<int32_t, bool, std::string>;

   auto v1 = variant_type{std::string{"hello world"}};
   BOOST_CHECK_EXCEPTION(std::get<int32_t>(v1), std::bad_variant_access, [](const auto& e) { return true; });
   auto result1 = std::get<std::string>(v1);
   BOOST_REQUIRE(result1 == std::string{"hello world"});

   const auto v2 = variant_type{std::string{"hello world"}};
   BOOST_CHECK_EXCEPTION(std::get<int32_t>(v2), std::bad_variant_access, [](const auto& e) { return true; });
   const auto result2 = std::get<std::string>(v2);
   BOOST_REQUIRE(result2 == std::string{"hello world"});
}

BOOST_AUTO_TEST_CASE(static_variant_from_index) {
   using variant_type = std::variant<int32_t, bool, std::string>;
   auto v = variant_type{};

   BOOST_CHECK_THROW(forge::from_index(v, 3), std::out_of_range);

   forge::from_index(v, 2);
   BOOST_REQUIRE(std::string{} == std::get<std::string>(v));
}

BOOST_AUTO_TEST_CASE(static_variant_rejects_indices_before_narrowing) {
   using variant_type = std::variant<int32_t, bool>;
   auto value = variant_type{};
   const auto encoded = forge::variant{
       forge::variants{
           forge::variant{std::uint64_t{1} << 32U},
           forge::variant{7},
       },
   };

   BOOST_CHECK_THROW(forge::from_variant(encoded, value), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(static_variant_get_index) {
   using variant_type = std::variant<int32_t, bool, std::string>;
   BOOST_REQUIRE((forge::get_index<variant_type, int32_t>() == 0));
   BOOST_REQUIRE((forge::get_index<variant_type, bool>() == 1));
   BOOST_REQUIRE((forge::get_index<variant_type, std::string>() == 2));
   BOOST_REQUIRE((forge::get_index<variant_type, double>() ==
                  std::variant_size_v<variant_type>)); // Isn't a type contained in variant.
}
BOOST_AUTO_TEST_SUITE_END()
