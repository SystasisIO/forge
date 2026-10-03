#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

namespace forge_schema_tests {

struct http_config {
   std::uint16_t bind_port = 0;
   std::string bind_host;
   bool tls_enabled = false;
   std::vector<std::string> tags;
   std::string token;
};

struct optional_config {
   std::optional<std::string> token;
   std::optional<std::uint16_t> port;
};

struct optional_default_config {
   std::optional<std::uint16_t> wrapped_port;
   std::optional<std::uint16_t> raw_port;
};

struct optional_list_item {
   std::string id;
};

struct optional_list_config {
   std::optional<std::vector<std::string>> tags;
   std::optional<std::vector<optional_list_item>> items;
};

enum class path_policy {
   direct_only,
   direct_preferred,
};

BOOST_DESCRIBE_ENUM(path_policy, direct_only, direct_preferred)

struct policy_list_config {
   std::vector<path_policy> policies;
};

struct byte_list_config {
   std::vector<std::uint8_t> payload;
};

struct wide_range_config {
   __int128 signed_value = 0;
   unsigned __int128 unsigned_value = 0;
};

struct diagnostic_wire_record {
   std::string name;
   std::string token;
};
BOOST_DESCRIBE_STRUCT(diagnostic_wire_record, (), (name, token))
static_assert(boost::describe::has_describe_members<diagnostic_wire_record>::value);

struct throwing_scalar {
   throwing_scalar() = default;
   explicit throwing_scalar(std::string) {
      throw std::runtime_error{"invalid scalar"};
   }

   [[nodiscard]] std::string to_string() const {
      throw std::runtime_error{"unformattable scalar"};
   }
};

} // namespace forge_schema_tests

BOOST_DESCRIBE_STRUCT(forge_schema_tests::http_config, (), (bind_port, bind_host, tls_enabled, tags, token))
BOOST_DESCRIBE_STRUCT(forge_schema_tests::optional_config, (), (token, port))
BOOST_DESCRIBE_STRUCT(forge_schema_tests::optional_default_config, (), (wrapped_port, raw_port))
BOOST_DESCRIBE_STRUCT(forge_schema_tests::optional_list_item, (), (id))
BOOST_DESCRIBE_STRUCT(forge_schema_tests::optional_list_config, (), (tags, items))
BOOST_DESCRIBE_STRUCT(forge_schema_tests::policy_list_config, (), (policies))
BOOST_DESCRIBE_STRUCT(forge_schema_tests::byte_list_config, (), (payload))
BOOST_DESCRIBE_STRUCT(forge_schema_tests::wide_range_config, (), (signed_value, unsigned_value))

import forge.schema.diagnostic;
import forge.schema.exceptions;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.schema.scalar;
import forge.crypto.digest.sha256;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.secret_string;
import forge.crypto.asymmetric;
import forge.crypto.asymmetric.secp256k1;
import forge.crypto.asymmetric.p256;
import forge.crypto.asymmetric.ed25519;
import forge.crypto.asymmetric.rsa;
import forge.crypto.asymmetric.x25519;
import forge.crypto.bls;
import forge.variant.schema;
import forge.variant.value;
import forge.variant.described;
import forge.codec.json;
import forge.raw.codec;

namespace forge_schema_tests {

struct digest_list_config {
   std::vector<forge::crypto::digest::sha256> values;
};

struct diagnostic_secret_parent {
   forge::crypto::core::secret_string token;
   std::vector<forge::crypto::core::secret_string> children;
};
BOOST_DESCRIBE_STRUCT(diagnostic_secret_parent, (), (token, children))
static_assert(boost::describe::has_describe_members<diagnostic_secret_parent>::value);

} // namespace forge_schema_tests

BOOST_DESCRIBE_STRUCT(forge_schema_tests::digest_list_config, (), (values))

template <> struct forge::schema::rules<forge_schema_tests::http_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::http_config> define() {
      auto schema = forge::schema::object<forge_schema_tests::http_config>();
      schema.field<&forge_schema_tests::http_config::bind_port>("bind-port")
          .required()
          .default_value(8080)
          .range(1, 65535)
          .description("HTTP bind port");
      schema.field<&forge_schema_tests::http_config::bind_host>("bind-host").default_value("127.0.0.1");
      schema.field<&forge_schema_tests::http_config::tls_enabled>("tls-enabled").default_value(false);
      static_cast<void>(schema.field<&forge_schema_tests::http_config::tags>("tags"));
      schema.field<&forge_schema_tests::http_config::token>("token").secret().deprecated("use vault-ref");
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::optional_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::optional_config> define() {
      auto schema = forge::schema::object<forge_schema_tests::optional_config>();
      schema.field<&forge_schema_tests::optional_config::token>("token").non_empty();
      schema.field<&forge_schema_tests::optional_config::port>("port").range(1, 65535);
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::optional_default_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::optional_default_config> define() {
      auto schema = forge::schema::object<forge_schema_tests::optional_default_config>();
      schema.field<&forge_schema_tests::optional_default_config::wrapped_port>("wrapped-port")
          .default_value(std::optional<std::uint16_t>{443})
          .range(1, 65535);
      schema.field<&forge_schema_tests::optional_default_config::raw_port>("raw-port")
          .default_value(8443)
          .range(1, 65535);
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::optional_list_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::optional_list_config> define() {
      auto schema = forge::schema::object<forge_schema_tests::optional_list_config>();
      schema.field<&forge_schema_tests::optional_list_config::tags>("tags").min_items(1).each_non_empty();
      schema.field<&forge_schema_tests::optional_list_config::items>("items")
          .items<forge_schema_tests::optional_list_item>()
          .unique_by<&forge_schema_tests::optional_list_item::id>();
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::digest_list_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::digest_list_config> define() {
      auto schema = forge::schema::object<forge_schema_tests::digest_list_config>();
      static_cast<void>(schema.field<&forge_schema_tests::digest_list_config::values>("values"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::policy_list_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::policy_list_config> define() {
      auto schema = forge::schema::object<forge_schema_tests::policy_list_config>();
      static_cast<void>(schema.field<&forge_schema_tests::policy_list_config::policies>("policies"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::byte_list_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::byte_list_config> define() {
      auto schema = forge::schema::object<forge_schema_tests::byte_list_config>();
      static_cast<void>(schema.field<&forge_schema_tests::byte_list_config::payload>("payload"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::wide_range_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_schema_tests::wide_range_config> define() {
      constexpr auto boundary = static_cast<unsigned __int128>(1) << 100;
      auto schema = forge::schema::object<forge_schema_tests::wide_range_config>();
      schema.field<&forge_schema_tests::wide_range_config::signed_value>("signed-value")
          .range(-static_cast<__int128>(boundary), static_cast<__int128>(boundary));
      schema.field<&forge_schema_tests::wide_range_config::unsigned_value>("unsigned-value")
          .range(boundary, boundary + 10);
      return schema;
   }
};

template <> struct forge::schema::rules<forge_schema_tests::diagnostic_wire_record> {
   [[nodiscard]] static auto define() {
      auto schema = forge::schema::object<forge_schema_tests::diagnostic_wire_record>();
      static_cast<void>(schema.field<&forge_schema_tests::diagnostic_wire_record::name>("public-name"));
      schema.field<&forge_schema_tests::diagnostic_wire_record::token>("private-value").secret();
      return schema;
   }
};

namespace {

[[nodiscard]] bool has_error_code(const std::vector<forge::schema::diagnostic>& diagnostics, std::string_view code) {
   return std::ranges::any_of(diagnostics, [&](const forge::schema::diagnostic& entry) {
      return entry.level == forge::schema::severity::error && entry.code == code;
   });
}

} // namespace

BOOST_AUTO_TEST_CASE(schema_describes_fields_defaults_and_validation) {
   const auto schema = forge::schema::rules<forge_schema_tests::http_config>::define();
   BOOST_REQUIRE_EQUAL(schema.fields().size(), 5U);
   BOOST_TEST(schema.fields()[0].name == "bind-port");
   BOOST_TEST(schema.fields()[0].required);
   BOOST_TEST(schema.fields()[4].secret);
   BOOST_TEST(schema.fields()[4].deprecated);

   auto config = forge_schema_tests::http_config{};
   schema.apply_defaults(config);
   BOOST_TEST(config.bind_port == 8080U);
   BOOST_TEST(config.bind_host == "127.0.0.1");
   BOOST_TEST(!config.tls_enabled);

   config.bind_port = 0;
   const auto diagnostics = schema.validate(config, "http");
   BOOST_REQUIRE_EQUAL(diagnostics.size(), 1U);
   BOOST_TEST(diagnostics.front().path == "http.bind-port");
   BOOST_TEST(diagnostics.front().code == "schema.range");
}

BOOST_AUTO_TEST_CASE(schema_optional_scalar_validators_unwrap_present_values_and_skip_absent) {
   const auto schema = forge::schema::rules<forge_schema_tests::optional_config>::define();

   auto absent = forge_schema_tests::optional_config{};
   BOOST_TEST(schema.validate(absent, "config").empty());

   auto valid = forge_schema_tests::optional_config{.token = "secret", .port = 443};
   BOOST_TEST(schema.validate(valid, "config").empty());

   auto invalid = forge_schema_tests::optional_config{.token = "", .port = 0};
   const auto diagnostics = schema.validate(invalid, "config");
   BOOST_REQUIRE_EQUAL(diagnostics.size(), 2U);
   BOOST_TEST(diagnostics[0].path == "config.token");
   BOOST_TEST(diagnostics[0].code == "schema.non_empty");
   BOOST_TEST(diagnostics[1].path == "config.port");
   BOOST_TEST(diagnostics[1].code == "schema.range");
}

BOOST_AUTO_TEST_CASE(schema_optional_defaults_apply_wrapped_and_raw_values) {
   const auto schema = forge::schema::rules<forge_schema_tests::optional_default_config>::define();

   auto config = forge_schema_tests::optional_default_config{};
   schema.apply_defaults(config);

   BOOST_REQUIRE(config.wrapped_port.has_value());
   BOOST_TEST(*config.wrapped_port == 443U);
   BOOST_REQUIRE(config.raw_port.has_value());
   BOOST_TEST(*config.raw_port == 8443U);
   BOOST_TEST(schema.validate(config, "config").empty());
}

BOOST_AUTO_TEST_CASE(schema_range_validation_preserves_wide_integer_precision) {
   constexpr auto boundary = static_cast<unsigned __int128>(1) << 100;
   const auto schema = forge::schema::rules<forge_schema_tests::wide_range_config>::define();

   const auto valid = forge_schema_tests::wide_range_config{
       .signed_value = static_cast<__int128>(boundary),
       .unsigned_value = boundary,
   };
   BOOST_TEST(schema.validate(valid, "wide").empty());

   const auto signed_overflow = forge_schema_tests::wide_range_config{
       .signed_value = static_cast<__int128>(boundary) + 1,
       .unsigned_value = boundary,
   };
   const auto signed_diagnostics = schema.validate(signed_overflow, "wide");
   BOOST_REQUIRE_EQUAL(signed_diagnostics.size(), 1U);
   BOOST_TEST(signed_diagnostics.front().path == "wide.signed-value");
   BOOST_TEST(signed_diagnostics.front().code == "schema.range");

   const auto unsigned_underflow = forge_schema_tests::wide_range_config{
       .signed_value = 0,
       .unsigned_value = boundary - 1,
   };
   const auto unsigned_diagnostics = schema.validate(unsigned_underflow, "wide");
   BOOST_REQUIRE_EQUAL(unsigned_diagnostics.size(), 1U);
   BOOST_TEST(unsigned_diagnostics.front().path == "wide.unsigned-value");
   BOOST_TEST(unsigned_diagnostics.front().code == "schema.range");
}

BOOST_AUTO_TEST_CASE(schema_optional_list_validators_unwrap_present_values_and_skip_absent) {
   const auto schema = forge::schema::rules<forge_schema_tests::optional_list_config>::define();

   auto absent = forge_schema_tests::optional_list_config{};
   BOOST_TEST(schema.validate(absent, "config").empty());

   auto valid = forge_schema_tests::optional_list_config{
       .tags = std::vector<std::string>{"alpha"},
       .items = std::vector<forge_schema_tests::optional_list_item>{{.id = "a"}, {.id = "b"}},
   };
   BOOST_TEST(schema.validate(valid, "config").empty());

   auto empty_tags = forge_schema_tests::optional_list_config{.tags = std::vector<std::string>{}};
   const auto empty_tag_diagnostics = schema.validate(empty_tags, "config");
   BOOST_TEST(has_error_code(empty_tag_diagnostics, "schema.min_items"));

   auto invalid_tags = forge_schema_tests::optional_list_config{.tags = std::vector<std::string>{""}};
   const auto invalid_tag_diagnostics = schema.validate(invalid_tags, "config");
   BOOST_TEST(has_error_code(invalid_tag_diagnostics, "schema.non_empty"));

   auto duplicate_items = forge_schema_tests::optional_list_config{
       .items = std::vector<forge_schema_tests::optional_list_item>{{.id = "same"}, {.id = "same"}},
   };
   const auto duplicate_item_diagnostics = schema.validate(duplicate_items, "config");
   BOOST_TEST(has_error_code(duplicate_item_diagnostics, "schema.unique"));
}

BOOST_AUTO_TEST_CASE(schema_decode_explicit_null_optionals_as_absent_values) {
   const auto scalar_schema = forge::schema::rules<forge_schema_tests::optional_config>::define();
   auto scalar_config = forge_schema_tests::optional_config{.token = "secret", .port = 443};

   const auto scalar_diagnostics = scalar_schema.decode_object(
       forge::schema::input_value::object_type{
           {"token", forge::schema::input_value{}},
           {"port", forge::schema::input_value{}},
       },
       "config", scalar_config);

   BOOST_TEST(scalar_diagnostics.empty());
   BOOST_TEST(!scalar_config.token.has_value());
   BOOST_TEST(!scalar_config.port.has_value());

   const auto list_schema = forge::schema::rules<forge_schema_tests::optional_list_config>::define();
   auto list_config = forge_schema_tests::optional_list_config{
       .tags = std::vector<std::string>{"alpha"},
       .items = std::vector<forge_schema_tests::optional_list_item>{{.id = "a"}, {.id = "b"}},
   };

   const auto list_diagnostics = list_schema.decode_object(
       forge::schema::input_value::object_type{
           {"tags", forge::schema::input_value{}},
           {"items", forge::schema::input_value{}},
       },
       "config", list_config);

   BOOST_TEST(list_diagnostics.empty());
   BOOST_TEST(!list_config.tags.has_value());
   BOOST_TEST(!list_config.items.has_value());
}

BOOST_AUTO_TEST_CASE(schema_converts_described_enums) {
   auto level = forge::schema::severity::info;
   BOOST_TEST(forge::schema::enum_from_string("warning", level));
   BOOST_TEST(static_cast<int>(level) == static_cast<int>(forge::schema::severity::warning));
   BOOST_TEST(forge::schema::enum_to_string(forge::schema::severity::error).value() == "error");
   BOOST_TEST(forge::schema::enum_from_int(0, level));
   BOOST_TEST(static_cast<int>(level) == static_cast<int>(forge::schema::severity::info));
}

BOOST_AUTO_TEST_CASE(schema_checked_integral_cast_handles_widening_and_narrowing) {
   static_assert(forge::schema::signed_integral_value<__int128>);
   static_assert(forge::schema::unsigned_integral_value<unsigned __int128>);

   BOOST_TEST(forge::schema::checked_integral_cast<long long>(int{-1}) == -1LL);
   BOOST_TEST(forge::schema::checked_integral_cast<std::int64_t>(std::int32_t{-123}) == -123);
   BOOST_TEST(forge::schema::checked_integral_cast<long long>(std::uint32_t{123}) == 123LL);

   BOOST_CHECK_THROW(static_cast<void>(forge::schema::checked_integral_cast<std::uint8_t>(std::uint16_t{256})),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::schema::checked_integral_cast<std::int8_t>(std::int16_t{128})),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::schema::checked_integral_cast<std::uint8_t>(std::int16_t{-1})),
                     forge::schema::exceptions::invalid_value);
}

BOOST_AUTO_TEST_CASE(schema_public_scalar_and_default_failures_are_typed) {
   auto schema = forge::schema::object<forge_schema_tests::http_config>();
   BOOST_CHECK_THROW(
       schema.field<&forge_schema_tests::http_config::bind_port>("bind-port").default_value(std::string{"not-a-port"}),
       forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::schema::parse_scalar_text<forge_schema_tests::throwing_scalar>("x")),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::schema::parse_scalar_text<forge::crypto::digest::sha256>("invalid")),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::schema::format_scalar_text(forge_schema_tests::throwing_scalar{})),
                     forge::schema::exceptions::invalid_value);

   auto diagnostics = std::vector<forge::schema::diagnostic>{};
   BOOST_CHECK_THROW(static_cast<void>(forge::schema::cast_input_to<double>(
                         forge::schema::input_value{std::string{"not-a-number"}}, "value", diagnostics)),
                     forge::schema::exceptions::invalid_value);
}

BOOST_AUTO_TEST_CASE(schema_exact_scalar_validation_checks_float_range_before_narrowing) {
   auto diagnostics = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<float>(forge::schema::input_value{1e100}, "ratio", diagnostics);

   BOOST_REQUIRE_EQUAL(diagnostics.size(), 1U);
   BOOST_TEST(diagnostics.front().code == "config.range");
   BOOST_TEST(diagnostics.front().path == "ratio");
}

BOOST_AUTO_TEST_CASE(schema_exact_scalar_validation_rejects_lossy_floating_point_narrowing) {
   auto exact = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<float>(forge::schema::input_value{1.5}, "ratio", exact);
   BOOST_TEST(exact.empty());

   auto lossy = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<float>(forge::schema::input_value{1.00000001}, "ratio", lossy);
   BOOST_REQUIRE_EQUAL(lossy.size(), 1U);
   BOOST_TEST(lossy.front().code == "config.range");
   BOOST_TEST(lossy.front().path == "ratio");
}

BOOST_AUTO_TEST_CASE(schema_exact_scalar_validation_checks_integer_precision_before_floating_conversion) {
   constexpr auto largest_consecutive_double_integer = std::uint64_t{9007199254740992};
   constexpr auto first_inexact_double_integer = largest_consecutive_double_integer + 1;

   auto exact = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<double>(forge::schema::input_value{largest_consecutive_double_integer},
                                                     "value", exact);
   BOOST_TEST(exact.empty());

   auto unsigned_lossy = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<double>(forge::schema::input_value{first_inexact_double_integer}, "value",
                                                     unsigned_lossy);
   BOOST_REQUIRE_EQUAL(unsigned_lossy.size(), 1U);
   BOOST_TEST(unsigned_lossy.front().code == "config.range");
   BOOST_TEST(unsigned_lossy.front().path == "value");

   auto signed_lossy = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<double>(
       forge::schema::input_value{static_cast<std::int64_t>(first_inexact_double_integer)}, "value", signed_lossy);
   BOOST_REQUIRE_EQUAL(signed_lossy.size(), 1U);
   BOOST_TEST(signed_lossy.front().code == "config.range");
   BOOST_TEST(signed_lossy.front().path == "value");
}

BOOST_AUTO_TEST_CASE(schema_exact_wide_integers_require_canonical_decimal_spelling) {
   auto canonical = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<__int128>(forge::schema::input_value{std::string{"1"}}, "signed",
                                                       canonical);
   forge::schema::validate_exact_input_value<unsigned __int128>(forge::schema::input_value{std::string{"1"}},
                                                                "unsigned", canonical);
   BOOST_TEST(canonical.empty());

   auto leading_zero = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<__int128>(forge::schema::input_value{std::string{"0001"}}, "signed",
                                                       leading_zero);
   BOOST_REQUIRE_EQUAL(leading_zero.size(), 1U);
   BOOST_TEST(leading_zero.front().code == "config.type");
   BOOST_TEST(leading_zero.front().path == "signed");

   auto negative_zero = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<__int128>(forge::schema::input_value{std::string{"-0"}}, "signed",
                                                       negative_zero);
   BOOST_REQUIRE_EQUAL(negative_zero.size(), 1U);
   BOOST_TEST(negative_zero.front().code == "config.type");
   BOOST_TEST(negative_zero.front().path == "signed");

   auto unsigned_leading_zero = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<unsigned __int128>(forge::schema::input_value{std::string{"0001"}},
                                                                "unsigned", unsigned_leading_zero);
   BOOST_REQUIRE_EQUAL(unsigned_leading_zero.size(), 1U);
   BOOST_TEST(unsigned_leading_zero.front().code == "config.type");
   BOOST_TEST(unsigned_leading_zero.front().path == "unsigned");

   auto numeric_signed = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<__int128>(forge::schema::input_value{std::int64_t{1}}, "signed",
                                                       numeric_signed);
   BOOST_REQUIRE_EQUAL(numeric_signed.size(), 1U);
   BOOST_TEST(numeric_signed.front().code == "config.type");
   BOOST_TEST(numeric_signed.front().path == "signed");

   auto numeric_unsigned = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<unsigned __int128>(forge::schema::input_value{std::uint64_t{1}},
                                                                "unsigned", numeric_unsigned);
   BOOST_REQUIRE_EQUAL(numeric_unsigned.size(), 1U);
   BOOST_TEST(numeric_unsigned.front().code == "config.type");
   BOOST_TEST(numeric_unsigned.front().path == "unsigned");
}

BOOST_AUTO_TEST_CASE(schema_exact_enum_validation_accepts_canonical_config_names) {
   auto canonical = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<forge_schema_tests::path_policy>(
       forge::schema::input_value{std::string{"direct-only"}}, "path-policy", canonical);
   BOOST_TEST(canonical.empty());

   auto malformed = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<forge_schema_tests::path_policy>(
       forge::schema::input_value{std::string{"unknown-policy"}}, "path-policy", malformed);
   BOOST_REQUIRE_EQUAL(malformed.size(), 1U);
   BOOST_TEST(malformed.front().code == "config.type");
   BOOST_TEST(malformed.front().path == "path-policy");

   auto numeric = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<forge_schema_tests::path_policy>(
       forge::schema::input_value{std::uint64_t{0}}, "path-policy", numeric);
   BOOST_REQUIRE_EQUAL(numeric.size(), 1U);
   BOOST_TEST(numeric.front().code == "config.type");
   BOOST_TEST(numeric.front().path == "path-policy");

   auto noncanonical = std::vector<forge::schema::diagnostic>{};
   forge::schema::validate_exact_input_value<forge_schema_tests::path_policy>(
       forge::schema::input_value{std::string{"direct_only"}}, "path-policy", noncanonical);
   BOOST_REQUIRE_EQUAL(noncanonical.size(), 1U);
   BOOST_TEST(noncanonical.front().code == "config.type");
   BOOST_TEST(noncanonical.front().path == "path-policy");
}

BOOST_AUTO_TEST_CASE(schema_enum_lists_decode_canonical_config_names) {
   const auto schema = forge::schema::rules<forge_schema_tests::policy_list_config>::define();
   const auto input = forge::schema::input_value::object_type{
       {"policies",
        forge::schema::input_value::array_type{
            forge::schema::input_value{std::string{"direct-only"}},
            forge::schema::input_value{std::string{"direct-preferred"}},
        }},
   };
   const auto exact = schema.validate_exact_input(input, "config");
   BOOST_TEST(exact.empty());

   auto decoded = forge_schema_tests::policy_list_config{};
   const auto diagnostics = schema.decode_object(input, "config", decoded);
   BOOST_TEST(diagnostics.empty());
   BOOST_REQUIRE_EQUAL(decoded.policies.size(), 2U);
   BOOST_TEST(static_cast<int>(decoded.policies[0]) == static_cast<int>(forge_schema_tests::path_policy::direct_only));
   BOOST_TEST(static_cast<int>(decoded.policies[1]) ==
              static_cast<int>(forge_schema_tests::path_policy::direct_preferred));
}

BOOST_AUTO_TEST_CASE(schema_scalar_lists_decode_integral_values_with_bounds) {
   const auto schema = forge::schema::rules<forge_schema_tests::byte_list_config>::define();
   const auto input = forge::schema::input_value::object_type{
       {"payload",
        forge::schema::input_value::array_type{
            forge::schema::input_value{std::uint64_t{0}},
            forge::schema::input_value{std::uint64_t{127}},
            forge::schema::input_value{std::uint64_t{255}},
        }},
   };

   BOOST_TEST(schema.validate_exact_input(input, "config").empty());

   auto decoded = forge_schema_tests::byte_list_config{};
   const auto diagnostics = schema.decode_object(input, "config", decoded);
   BOOST_TEST(diagnostics.empty());
   BOOST_TEST(decoded.payload == (std::vector<std::uint8_t>{0U, 127U, 255U}), boost::test_tools::per_element());

   const auto overflow = schema.validate_exact_input(
       forge::schema::input_value::object_type{
           {"payload", forge::schema::input_value::array_type{forge::schema::input_value{std::uint64_t{256}}}},
       },
       "config");
   BOOST_REQUIRE_EQUAL(overflow.size(), 1U);
   BOOST_TEST(overflow.front().path == "config.payload[0]");
   BOOST_TEST(overflow.front().code == "config.range");
}

BOOST_AUTO_TEST_CASE(schema_exact_lists_require_canonical_string_scalar_spelling) {
   const auto schema = forge::schema::rules<forge_schema_tests::digest_list_config>::define();
   const auto canonical_value = std::string(64U, '0');
   const auto canonical = schema.validate_exact_input(
       forge::schema::input_value::object_type{
           {"values", forge::schema::input_value::array_type{forge::schema::input_value{canonical_value}}},
       },
       "config");
   BOOST_TEST(canonical.empty());

   const auto shortened = schema.validate_exact_input(
       forge::schema::input_value::object_type{
           {"values", forge::schema::input_value::array_type{forge::schema::input_value{std::string{"00"}}}},
       },
       "config");
   BOOST_REQUIRE_EQUAL(shortened.size(), 1U);
   BOOST_TEST(shortened.front().code == "config.type");
   BOOST_TEST(shortened.front().path == "config.values[0]");

   const auto uppercase = schema.validate_exact_input(
       forge::schema::input_value::object_type{
           {"values", forge::schema::input_value::array_type{forge::schema::input_value{std::string(64U, 'A')}}},
       },
       "config");
   BOOST_REQUIRE_EQUAL(uppercase.size(), 1U);
   BOOST_TEST(uppercase.front().code == "config.type");
   BOOST_TEST(uppercase.front().path == "config.values[0]");
}

BOOST_AUTO_TEST_CASE(diagnostic_redaction_does_not_change_ordinary_variant_json_or_raw) {
   const auto input = forge_schema_tests::diagnostic_wire_record{"p", "s"};
   const auto diagnostic = forge::variant_schema::encode_diagnostic(input);
   BOOST_TEST(diagnostic["public-name"].as_string() == "p");
   BOOST_TEST(diagnostic["private-value"].as_string() == "<redacted>");

   const auto ordinary = forge::variant_schema::encode(input);
   BOOST_TEST(ordinary["private-value"].as_string() == "s");
   BOOST_TEST(forge::variant{input}["token"].as_string() == "s");
   const auto written = forge::codec::json::write(input);
   BOOST_REQUIRE(written.ok());
   const auto parsed = forge::codec::json::read_value(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_TEST(parsed.value["private-value"].as_string() == "s");
   const auto packed = forge::raw::pack(input);
   BOOST_TEST(packed == (forge::raw::bytes{1U, 'p', 1U, 's'}), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(real_crypto_secret_types_redact_without_variant_or_crypto_dependencies_in_schema) {
   const auto bytes = forge::crypto::core::secret_bytes{std::vector<std::uint8_t>{1, 2, 3}};
   const auto text = forge::crypto::core::secret_string{"unexposed"};
   const auto key = forge::crypto::asymmetric::private_key{};
   BOOST_TEST(forge::variant_schema::encode_diagnostic(bytes).as_string() == "<redacted>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(text).as_string() == "<redacted>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(key).as_string() == "<redacted>");
   BOOST_TEST(
       forge::variant_schema::encode_diagnostic(forge::crypto::asymmetric::secp256k1::private_key{}).as_string() ==
       "<redacted>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(forge::crypto::asymmetric::p256::private_key{}).as_string() ==
              "<redacted>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(forge::crypto::asymmetric::ed25519::private_key{}).as_string() ==
              "<redacted>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(forge::crypto::asymmetric::rsa::private_key{}).as_string() ==
              "<redacted>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(forge::crypto::asymmetric::x25519::private_key{}).as_string() ==
              "<redacted>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(forge::crypto::bls::private_key{}).as_string() == "<redacted>");
   const auto parent = forge_schema_tests::diagnostic_secret_parent{forge::crypto::core::secret_string{"unexposed"},
                                                                    {forge::crypto::core::secret_string{"unexposed"}}};
   const auto nested = forge::variant_schema::encode_diagnostic(parent);
   BOOST_TEST(nested["token"].as_string() == "<redacted>");
   BOOST_TEST(nested["children"][std::size_t{0}].as_string() == "<redacted>");
}
