#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.codec.json;
import forge.schema.object;
import forge.variant.value;
import forge.variant.schema;

namespace json_strict_tests {
struct value { std::int64_t count = 0; };
struct large_value { std::vector<std::int64_t> values; std::string text; };
struct floating_value { double number = 0; };
struct nested_value { value nested; std::vector<value> list; };
struct dynamic_value { forge::variant_object claims; };
BOOST_DESCRIBE_STRUCT(value, (), (count))
BOOST_DESCRIBE_STRUCT(large_value, (), (values, text))
BOOST_DESCRIBE_STRUCT(floating_value, (), (number))
BOOST_DESCRIBE_STRUCT(nested_value, (), (nested, list))
BOOST_DESCRIBE_STRUCT(dynamic_value, (), (claims))
}

namespace forge::schema {
template <> struct rules<json_strict_tests::dynamic_value> {
   static object_schema<json_strict_tests::dynamic_value> define() {
      return object<json_strict_tests::dynamic_value>().field<&json_strict_tests::dynamic_value::claims>("claims");
   }
};

template <> struct rules<json_strict_tests::large_value> {
   static object_schema<json_strict_tests::large_value> define() {
      using type = json_strict_tests::large_value;
      auto result = object<type>();
      result.field<&type::values>("values").required().min_items(1).max_items(5000);
      result.field<&type::text>("text").length(0, 20000);
      return result;
   }
};
}

BOOST_AUTO_TEST_CASE(json_strict_rejects_escaped_duplicates_invalid_text_and_trailing_input) {
   const auto options = forge::codec::json::read_options{
      .max_depth = 8, .described_records = forge::codec::json::described_record_policy::exact,
      .max_bytes = 8192, .max_nodes = 512, .max_container_size = 128, .max_string_bytes = 2048};
   for (const auto* input : {R"({"x":1,"\u0078":2})", R"({"a":[{"x":1,"\u0078":2}]})",
      R"({"x":"\ud800"})", R"({"x":"\udc00"})", R"({"x":"\u0000"})", R"({"\u0000":1})",
      "{}{}", "{} false", "NaN", "Infinity", "{\"x\":NaN}"}) {
      BOOST_TEST(!forge::codec::json::read_value(input, options).ok());
   }
   auto invalid_utf8 = std::string{"{\"x\":\""};
   invalid_utf8.append("\xc0\xaf", 2);
   invalid_utf8 += "\"}";
   BOOST_TEST(!forge::codec::json::read_value(invalid_utf8, options).ok());
   const auto nul = std::string{"{}\0suffix", 9};
   BOOST_TEST(!forge::codec::json::read_value(nul, options).ok());
   BOOST_TEST(forge::codec::json::read_value("{\"x\":\"\\ud83d\\ude00\"} \r\n", options).ok());
   const auto bytes = std::string{"{}unused"};
   BOOST_TEST(forge::codec::json::read_value(std::string_view{bytes.data(), 2}, options).ok());
}

BOOST_AUTO_TEST_CASE(json_strict_limits_count_root_nodes_containers_and_decoded_strings) {
   auto options = forge::codec::json::read_options{
      .max_depth = 1, .described_records = forge::codec::json::described_record_policy::exact,
      .max_bytes = 8192, .max_nodes = 3, .max_container_size = 2, .max_string_bytes = 2};
   BOOST_TEST(forge::codec::json::read_value(R"({"x":1,"y":2})", options).ok());
   BOOST_TEST(!forge::codec::json::read_value(R"({"x":1,"y":2,"z":3})", options).ok());
   BOOST_TEST(!forge::codec::json::read_value(R"({"x":{}})", options).ok());
   BOOST_TEST(!forge::codec::json::read_value("[[]]", options).ok());
   BOOST_TEST(forge::codec::json::read_value(R"("\u0061\u0062")", options).ok());
   BOOST_TEST(!forge::codec::json::read_value(R"("abc")", options).ok());
   BOOST_TEST(!forge::codec::json::read_value(R"({"abc":1})", options).ok());
   options.max_depth = 8;
   options.max_nodes = 2;
   BOOST_TEST(!forge::codec::json::read_value("[1,2]", options).ok());
   options.max_bytes = 1;
   BOOST_TEST(!forge::codec::json::read_value("{}", options).ok());
   options.max_bytes = 8192;
   auto deep = std::string(1000, '[') + "0" + std::string(1000, ']');
   BOOST_TEST(!forge::codec::json::read_value(deep, options).ok());
}

BOOST_AUTO_TEST_CASE(json_strict_requires_complete_nested_roots_within_the_supplied_view) {
   const auto options =
       forge::codec::json::read_options{.max_depth = 8,
                                        .described_records = forge::codec::json::described_record_policy::exact,
                                        .max_bytes = 4096,
                                        .max_nodes = 128,
                                        .max_container_size = 32,
                                        .max_string_bytes = 256};
   for (const auto text :
        {std::string_view{R"({"outer":{"value":1}})"}, std::string_view{R"({"outer":[1,{"value":2}]})"},
         std::string_view{R"([{"inner":[1,2]}])"}, std::string_view{R"([[1,2],{"a":{"b":true}}])"}}) {
      const auto storage = std::string{text} + "unused";
      BOOST_REQUIRE(forge::codec::json::read_value(std::string_view{storage.data(), text.size()}, options).ok());
      // The physical buffer still contains every omitted closing byte. Exact
      // parsing must not borrow it from beyond the caller's bounded view.
      for (std::size_t size = 0; size < text.size(); ++size) {
         const auto parsed = forge::codec::json::read_value(std::string_view{storage.data(), size}, options);
         BOOST_REQUIRE(!parsed.ok());
         BOOST_REQUIRE(!parsed.diagnostics.empty());
         BOOST_TEST(parsed.diagnostics.front().code == "json.parse");
      }
      BOOST_TEST(forge::codec::json::read_value(std::string{text} + " \t\r\n", options).ok());
      for (const auto suffix : {"{}", " false", "]", " trailing"})
         BOOST_TEST(!forge::codec::json::read_value(std::string{text} + suffix, options).ok());
   }
}

BOOST_AUTO_TEST_CASE(json_strict_typed_and_config_reads_reject_an_unclosed_parent_container) {
   const auto options =
       forge::codec::json::read_options{.described_records = forge::codec::json::described_record_policy::exact};
   const auto typed = std::string{R"({"nested":{"count":1},"list":[{"count":2}]})"};
   BOOST_REQUIRE(forge::codec::json::read<json_strict_tests::nested_value>(typed, options).ok());
   BOOST_REQUIRE(forge::codec::json::read_document(typed, options).ok());
   for (const auto omitted : {1U, 2U, 3U}) {
      const auto truncated = std::string_view{typed.data(), typed.size() - omitted};
      BOOST_TEST(!forge::codec::json::read<json_strict_tests::nested_value>(truncated, options).ok());
      BOOST_TEST(!forge::codec::json::read_document(truncated, options).ok());
   }
}

BOOST_AUTO_TEST_CASE(json_strict_does_not_coerce_integer_kinds_or_bounds) {
   const auto options = forge::codec::json::read_options{
      .described_records = forge::codec::json::described_record_policy::exact};
   for (const auto* input : {R"({"count":"1"})", R"({"count":true})", R"({"count":null})",
      R"({"count":1.0})", R"({"count":1e0})", R"({"count":1e2})", R"({"count":1E+2})",
      R"({"count":-1e0})", R"({"count":9223372036854775808})"}) {
      BOOST_TEST(!forge::codec::json::read<json_strict_tests::value>(input, options).ok());
   }
   BOOST_TEST(forge::codec::json::read<json_strict_tests::value>(R"({"count":9223372036854775807})", options).ok());
}

BOOST_AUTO_TEST_CASE(json_strict_preserves_floating_tokens_and_permissive_numeric_storage) {
   const auto options = forge::codec::json::read_options{
      .described_records = forge::codec::json::described_record_policy::exact};
   const std::pair<std::string_view, double> examples[] = {
      {"1.0", 1.0}, {"1e0", 1.0}, {"1e2", 100.0}, {"1E+2", 100.0}, {"-1e2", -100.0}, {"1e-1", 0.1}};
   for (const auto& [text, expected] : examples) {
      const auto parsed = forge::codec::json::read_value(text, options);
      BOOST_REQUIRE(parsed.ok());
      BOOST_REQUIRE(parsed.value.is_double());
      BOOST_TEST(parsed.value.as_double() == expected);
      const auto typed = forge::codec::json::read<json_strict_tests::floating_value>(
         std::string{"{\"number\":"} + std::string{text} + "}", options);
      BOOST_REQUIRE(typed.ok());
      BOOST_TEST(typed.value.number == expected);
   }
   const auto legacy = forge::codec::json::read_value("1e2");
   BOOST_REQUIRE(legacy.ok());
   BOOST_REQUIRE(legacy.value.is_uint64());
   BOOST_TEST(legacy.value.as_uint64() == 100U);
   BOOST_TEST(forge::codec::json::read<json_strict_tests::value>(R"({"count":1e2})").ok());
   const auto bytes = std::string{"1e2unused"};
   const auto bounded = forge::codec::json::read_value(std::string_view{bytes.data(), 3}, options);
   BOOST_REQUIRE(bounded.ok());
   BOOST_REQUIRE(bounded.value.is_double());
   BOOST_TEST(bounded.value.as_double() == 100.0);
}

BOOST_AUTO_TEST_CASE(json_strict_rejects_nested_integer_exponents_but_keeps_dynamic_floats) {
   const auto options = forge::codec::json::read_options{
      .described_records = forge::codec::json::described_record_policy::exact};
   for (const auto* input : {R"({"nested":{"count":1e0},"list":[{"count":2}]})",
      R"({"nested":{"count":1},"list":[{"count":1e2}]})"}) {
      BOOST_TEST(!forge::codec::json::read<json_strict_tests::nested_value>(input, options).ok());
   }
   const auto dynamic = forge::codec::json::read<json_strict_tests::dynamic_value>(
      R"({"claims":{"numbers":[1e0,1.0,1e-1,18446744073709551615],"nested":{"\u0078":1e2}}})", options);
   BOOST_REQUIRE(dynamic.ok());
   const auto& numbers = dynamic.value.claims["numbers"].get_array();
   BOOST_REQUIRE_EQUAL(numbers.size(), 4U);
   for (std::size_t index = 0; index < 3; ++index) BOOST_TEST(numbers[index].is_double());
   BOOST_TEST(numbers[0].as_double() == 1.0);
   BOOST_TEST(numbers[1].as_double() == 1.0);
   BOOST_TEST(numbers[2].as_double() == 0.1);
   BOOST_REQUIRE(numbers[3].is_uint64());
   BOOST_TEST(numbers[3].as_uint64() == UINT64_MAX);
   const auto& nested = dynamic.value.claims["nested"].get_object()["x"];
   BOOST_REQUIRE(nested.is_double());
   BOOST_TEST(nested.as_double() == 100.0);
}

BOOST_AUTO_TEST_CASE(json_exact_decode_has_no_hidden_dynamic_adapter_limits) {
   auto options = forge::codec::json::read_options{
      .max_depth = 64, .described_records = forge::codec::json::described_record_policy::exact,
      .max_nodes = 5000, .max_container_size = 5000, .max_string_bytes = 20000};
   for (const auto count : {257U, 4097U}) {
      auto values = forge::variants{};
      for (unsigned i = 0; i < count; ++i) values.emplace_back(std::int64_t{i});
      const auto input = forge::variant{forge::mutable_variant_object{}("values", std::move(values))
         ("text", std::string(16385, 'x'))};
      const auto written = forge::codec::json::write_value(input);
      BOOST_REQUIRE(written.ok());
      auto parsed = forge::codec::json::read_value(written.text, options);
      BOOST_REQUIRE(parsed.ok());
      const auto decoded = forge::codec::json::decode<json_strict_tests::large_value>(std::move(parsed.value), options);
      BOOST_REQUIRE(decoded.ok());
      BOOST_TEST(decoded.value.values.size() == count);
      BOOST_TEST(decoded.value.text.size() == 16385U);
      auto field_rules = forge::schema::object<json_strict_tests::large_value>();
      field_rules.field<&json_strict_tests::large_value::values>("values").length(1, 5000);
      BOOST_TEST(field_rules.validate(decoded.value).empty());
      BOOST_TEST(forge::codec::json::write(decoded.value).ok());
      auto smaller = options;
      smaller.max_container_size = 256;
      BOOST_TEST(!forge::codec::json::read_value(written.text, smaller).ok());
   }
}
