#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>
#include <string_view>

import forge.codec.json;

namespace json_strict_tests {
struct value { std::int64_t count = 0; };
BOOST_DESCRIBE_STRUCT(value, (), (count))
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
