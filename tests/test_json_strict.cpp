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

namespace json_strict_tests {
struct value { std::int64_t count = 0; };
struct large_value { std::vector<std::int64_t> values; std::string text; };
BOOST_DESCRIBE_STRUCT(value, (), (count))
BOOST_DESCRIBE_STRUCT(large_value, (), (values, text))
}

namespace forge::schema {
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

BOOST_AUTO_TEST_CASE(json_strict_does_not_coerce_integer_kinds_or_bounds) {
   const auto options = forge::codec::json::read_options{
      .described_records = forge::codec::json::described_record_policy::exact};
   for (const auto* input : {R"({"count":"1"})", R"({"count":true})", R"({"count":null})",
      R"({"count":1.0})", R"({"count":1e0})", R"({"count":9223372036854775808})"}) {
      BOOST_TEST(!forge::codec::json::read<json_strict_tests::value>(input, options).ok());
   }
   BOOST_TEST(forge::codec::json::read<json_strict_tests::value>(R"({"count":9223372036854775807})", options).ok());
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
