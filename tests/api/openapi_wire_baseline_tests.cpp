#include "openapi_parity_fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace forge::tests::openapi_wire_baseline {

using namespace forge::tests::openapi_parity;

struct document_fixture {
   forge::variant document = make_document();

   const forge::variant& request_schema() const {
      return forge::tests::openapi_parity::request_schema(document);
   }

   const forge::variant& property(const char* name) const {
      return request_schema()["properties"][name];
   }
};

void check_schema_fixture(const forge::variant& generated, std::string_view expected_json) {
   const auto expected = forge::codec::json::read_value(expected_json);
   BOOST_REQUIRE(expected.ok());
   BOOST_CHECK(generated == expected.value);
}

template <typename T> void check_exact_decode(std::string_view json, const T& expected) {
   const auto decoded = forge::codec::json::read<T>(json, exact_options);
   BOOST_REQUIRE(decoded.ok());
   BOOST_CHECK(decoded.diagnostics.empty());
   BOOST_CHECK(decoded.value == expected);
}

template <typename T> void check_exact_value(std::string_view json, const T& expected) {
   check_exact_decode(json, expected);
   const auto encoded = forge::codec::json::write(expected);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK_EQUAL(encoded.text, json);
}

template <typename T>
void check_exact_rejection(std::string_view json, std::string_view code, std::string_view path = {}) {
   const auto decoded = forge::codec::json::read<T>(json, exact_options);
   BOOST_REQUIRE(!decoded.ok());
   BOOST_REQUIRE_EQUAL(decoded.diagnostics.size(), 1U);
   BOOST_CHECK_EQUAL(decoded.diagnostics.front().code, code);
   BOOST_CHECK_EQUAL(decoded.diagnostics.front().path, path);
}

BOOST_FIXTURE_TEST_SUITE(openapi_wire_baseline, document_fixture)

BOOST_AUTO_TEST_CASE(document_declares_canonical_json_profile) {
   BOOST_REQUIRE(document.get_object().contains("x-forge-json-profile"));
   BOOST_CHECK(document["x-forge-json-profile"]["canonical"].as_bool());
}

BOOST_AUTO_TEST_CASE(integer_schemas_expose_exact_bounds_without_changing_formats) {
   BOOST_CHECK_EQUAL(document["openapi"].as_string(), "3.1.0");
   const auto& response_schema = document["paths"]["/parity/values"]["post"]
                                         ["responses"]["200"]["content"]["application/json"]["schema"];
   BOOST_CHECK(request_schema() == response_schema);

   struct fixture {
      const char* name;
      std::string_view schema;
   };
   constexpr auto fixtures = std::array{
      fixture{"signed8", R"({"type":"integer","format":"int32","minimum":-128,"maximum":127})"},
      fixture{"unsigned8", R"({"type":"integer","format":"uint32","minimum":0,"maximum":255})"},
      fixture{"signed16", R"({"type":"integer","format":"int32","minimum":-32768,"maximum":32767})"},
      fixture{"unsigned16", R"({"type":"integer","format":"uint32","minimum":0,"maximum":65535})"},
      fixture{"signed32", R"({"type":"integer","format":"int32","minimum":-2147483648,"maximum":2147483647})"},
      fixture{"unsigned32", R"({"type":"integer","format":"uint32","minimum":0,"maximum":4294967295})"},
      fixture{"signed64", R"({"type":"integer","format":"int64","minimum":-9223372036854775808,"maximum":9223372036854775807})"},
      fixture{"unsigned64", R"({"type":"integer","format":"uint64","minimum":0,"maximum":18446744073709551615})"},
   };
   for (const auto& item : fixtures) {
      BOOST_TEST_CONTEXT("property=" << item.name) {
         const auto& schema = property(item.name);
         check_schema_fixture(schema, item.schema);
      }
   }
}

BOOST_AUTO_TEST_CASE(int8_uint8_codec_limits_match_generated_schema) {
   check_schema_fixture(property("signed8"), R"({"type":"integer","format":"int32","minimum":-128,"maximum":127})");
   check_schema_fixture(property("unsigned8"), R"({"type":"integer","format":"uint32","minimum":0,"maximum":255})");

   check_exact_value<std::int8_t>("-128", -128);
   check_exact_value<std::int8_t>("127", 127);
   check_exact_rejection<std::int8_t>("-129", "json.range");
   check_exact_rejection<std::int8_t>("128", "json.range");

   check_exact_value<std::uint8_t>("0", 0U);
   check_exact_value<std::uint8_t>("255", 255U);
   check_exact_rejection<std::uint8_t>("-1", "json.range");
   check_exact_rejection<std::uint8_t>("256", "json.range");
}

BOOST_AUTO_TEST_CASE(variant_schema_correlates_each_const_index_with_its_payload) {
   const auto& schema = property("indexed");
   check_schema_fixture(schema, R"({"oneOf":[
      {"type":"array","prefixItems":[{"type":"integer","const":0},
       {"type":"integer","format":"int32","minimum":-128,"maximum":127}],"minItems":2,"maxItems":2},
      {"type":"array","prefixItems":[{"type":"integer","const":1},{"type":"string"}],
       "minItems":2,"maxItems":2}]})");

   check_exact_value<indexed_choice>("[0,7]", indexed_choice{std::in_place_index<0>, std::int8_t{7}});
   check_exact_value<indexed_choice>(R"([1,"text"])", indexed_choice{std::in_place_index<1>, "text"});
   check_exact_rejection<indexed_choice>(R"([0,"text"])", "json.type", "[1]");
   check_exact_rejection<indexed_choice>("[1,7]", "json.type", "[1]");
   check_exact_rejection<indexed_choice>("[0,128]", "json.range", "[1]");
}

BOOST_AUTO_TEST_CASE(overlapping_variant_alternatives_are_disjoint_by_index) {
   const auto& schema = property("overlapping");
   check_schema_fixture(schema, R"({"oneOf":[
      {"type":"array","prefixItems":[{"type":"integer","const":0},
       {"type":"integer","format":"int32","minimum":-128,"maximum":127}],"minItems":2,"maxItems":2},
      {"type":"array","prefixItems":[{"type":"integer","const":1},
       {"type":"integer","format":"int32","minimum":-32768,"maximum":32767}],"minItems":2,"maxItems":2}]})");

   check_exact_value<overlapping_choice>("[0,7]", overlapping_choice{std::in_place_index<0>, std::int8_t{7}});
   check_exact_value<overlapping_choice>("[1,7]", overlapping_choice{std::in_place_index<1>, std::int16_t{7}});
   check_exact_rejection<overlapping_choice>("[0,128]", "json.range", "[1]");
   check_exact_value<overlapping_choice>("[1,128]", overlapping_choice{std::in_place_index<1>, std::int16_t{128}});
}

BOOST_AUTO_TEST_CASE(overlapping_object_payloads_remain_distinct_by_index) {
   BOOST_REQUIRE(property("object_overlap").get_object().contains("oneOf"));
   const auto& alternatives = property("object_overlap")["oneOf"].get_array();
   BOOST_REQUIRE_EQUAL(alternatives.size(), 2U);
   for (std::size_t index = 0; index < alternatives.size(); ++index) {
      const auto& branch = alternatives[index];
      BOOST_CHECK_EQUAL(branch["prefixItems"][std::size_t{0}]["const"].as_uint64(), index);
      BOOST_CHECK(branch["prefixItems"][std::size_t{1}] == property("item"));
      BOOST_CHECK_EQUAL(branch["minItems"].as_uint64(), 2U);
      BOOST_CHECK_EQUAL(branch["maxItems"].as_uint64(), 2U);
   }
   check_exact_value<overlapping_objects>(R"([0,{"count":7}])",
                                          overlapping_objects{std::in_place_index<0>, record{7, std::nullopt}});
   check_exact_value<overlapping_objects>(R"([1,{"count":7}])",
                                          overlapping_objects{std::in_place_index<1>, record{7, std::nullopt}});
}

BOOST_AUTO_TEST_CASE(integer_64_bit_codec_and_schema_preserve_exact_extrema) {
   check_schema_fixture(property("signed64"), R"({"type":"integer","format":"int64","minimum":-9223372036854775808,"maximum":9223372036854775807})");
   check_schema_fixture(property("unsigned64"), R"({"type":"integer","format":"uint64","minimum":0,"maximum":18446744073709551615})");

   check_exact_decode<std::int64_t>("-9223372036854775808", -9223372036854775807LL - 1LL);
   check_exact_decode<std::int64_t>("9223372036854775807", 9223372036854775807LL);
   check_exact_decode<std::int64_t>("9007199254740993", 9007199254740993LL);
   check_exact_decode<std::uint64_t>("18446744073709551615", 18446744073709551615ULL);
   check_exact_rejection<std::int64_t>("9223372036854775808", "json.range");
   check_exact_rejection<std::uint64_t>("-1", "json.range");
   check_exact_decode<std::uint64_t>("9007199254740993", 9007199254740993ULL);
   check_exact_value<uint64_list>("[9007199254740993,18446744073709551615]",
                                  uint64_list{9007199254740993ULL, std::numeric_limits<std::uint64_t>::max()});
}

BOOST_AUTO_TEST_CASE(integer_64_bit_schema_bounds_are_serialized_without_float_conversion) {
   BOOST_REQUIRE(property("signed64").get_object().contains("minimum"));
   BOOST_REQUIRE(property("signed64").get_object().contains("maximum"));
   BOOST_REQUIRE(property("unsigned64").get_object().contains("maximum"));
   BOOST_CHECK(property("signed64")["minimum"].is_int64());
   BOOST_CHECK(!property("signed64")["maximum"].is_double());
   BOOST_CHECK(property("unsigned64")["maximum"].is_uint64());
   BOOST_CHECK_EQUAL(property("signed64")["minimum"].as_int64(), std::numeric_limits<std::int64_t>::min());
   BOOST_CHECK_EQUAL(property("signed64")["maximum"].as_int64(), std::numeric_limits<std::int64_t>::max());
   BOOST_CHECK_EQUAL(property("unsigned64")["maximum"].as_uint64(), std::numeric_limits<std::uint64_t>::max());
   const auto encoded = forge::codec::json::write_value(document);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK(encoded.text.find("-9223372036854775808") != std::string::npos);
   BOOST_CHECK(encoded.text.find("9223372036854775807") != std::string::npos);
   BOOST_CHECK(encoded.text.find("18446744073709551615") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(nested_schema_preserves_correlated_variants_and_integer_bounds) {
   const auto& nested_schema = property("tree")["properties"];
   BOOST_CHECK(nested_schema["child"] == property("item"));
   BOOST_CHECK(nested_schema["choices"]["items"] == property("indexed"));
   BOOST_CHECK(property("choices")["items"] == property("indexed"));
   BOOST_CHECK(property("pair")["items"] == property("signed16"));
   BOOST_CHECK_EQUAL(property("pair")["minItems"].as_uint64(), 2U);
   BOOST_CHECK_EQUAL(property("pair")["maxItems"].as_uint64(), 2U);
   BOOST_REQUIRE(property("object").get_object().contains("oneOf"));
   const auto& alternatives = property("object")["oneOf"].get_array();
   BOOST_REQUIRE_EQUAL(alternatives.size(), 2U);
   BOOST_CHECK(alternatives[0]["prefixItems"][std::size_t{1}] == property("item"));
}

BOOST_AUTO_TEST_CASE(optional_schema_allows_missing_null_or_value_but_record_is_closed) {
   const auto& schema = property("item");
   check_schema_fixture(schema["required"], R"(["count"])");
   BOOST_CHECK(!schema["additionalProperties"].as_bool());
   const auto& alternatives = schema["properties"]["maybe"]["anyOf"].get_array();
   BOOST_REQUIRE_EQUAL(alternatives.size(), 2U);
   BOOST_CHECK(alternatives[0] == property("unsigned64"));
   check_schema_fixture(alternatives[1], R"({"type":"null"})");
   check_exact_decode<record>(R"({"count":7})", record{7, std::nullopt});
   check_exact_decode<record>(R"({"count":7,"maybe":null})", record{7, std::nullopt});
   check_exact_value<record>(R"({"count":7,"maybe":9007199254740993})", record{7, 9007199254740993ULL});
}

BOOST_AUTO_TEST_CASE(server_supplied_fixed_argument_is_absent_from_client_openapi) {
   const auto& operation = document["paths"]["/parity/fixed"]["post"];
   BOOST_CHECK(forge::tests::openapi_parity::request_schema(document, "/parity/fixed") == property("item"));
   const auto encoded = forge::codec::json::write_value(operation);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK(encoded.text.find("trusted") == std::string::npos);
   BOOST_CHECK(encoded.text.find("server_context") == std::string::npos);
   const auto found = operation.get_object().find("parameters");
   if (found != operation.get_object().end()) {
      BOOST_CHECK(found->value().get_array().empty());
   }
}

BOOST_AUTO_TEST_CASE(server_supplied_fixed_argument_cannot_be_bound_to_client_path) {
   auto operations = forge::api::http::traits<api>::openapi_operations();
   auto changed = false;
   for (auto& operation : operations) {
      if (operation.mapping.method_name == "fixed") {
         operation.mapping.target = "/parity/{trusted}";
         changed = true;
      }
   }
   BOOST_REQUIRE(changed);
   BOOST_CHECK_THROW((void)forge::api::http::detail::build_openapi_document(
                         forge::api::core::api_traits<api>::describe(), std::move(operations), {}),
                      forge::api::core::exceptions::protocol_error);
}

BOOST_AUTO_TEST_CASE(exported_components_are_copies_of_real_operation_schemas) {
   const auto& schemas = document["components"]["schemas"];
   BOOST_CHECK_EQUAL(schemas.get_object().size(), schema_names.size());
   for (const auto& entry : schema_names) {
      BOOST_TEST_CONTEXT("component=" << entry.component) {
         BOOST_CHECK(schemas[entry.component] == property(entry.property));
      }
   }
}

BOOST_AUTO_TEST_CASE(corpus_verdicts_preserve_exact_codec_and_named_http_compatibility) {
   auto names = std::set<std::string>{};
   for (const auto& entry : make_cases()) {
      BOOST_TEST_CONTEXT("case=" << entry.name << " json=" << entry.json) {
         BOOST_CHECK(names.insert(entry.name).second);
         BOOST_REQUIRE(document["components"]["schemas"].get_object().contains(entry.schema));
         const auto exact = entry.decode(entry.json, false);
         BOOST_CHECK_EQUAL(exact.ok, entry.expected_codec);
         if (exact.ok && (std::string_view{entry.schema}.starts_with("Int") ||
                          std::string_view{entry.schema}.starts_with("UInt")) &&
             std::string_view{entry.category} == "canonical") {
            BOOST_CHECK_EQUAL(exact.canonical_json, entry.json);
         }
         if (entry.expected_http.has_value()) {
            BOOST_CHECK_EQUAL(entry.decode(entry.json, true).ok, *entry.expected_http);
            BOOST_CHECK(!std::string_view{entry.note}.empty());
         } else {
            BOOST_CHECK_EQUAL(entry.expected, entry.expected_codec);
         }
         BOOST_CHECK_NO_THROW(evaluate_case(entry));
      }
   }
}

BOOST_AUTO_TEST_CASE(http_decoding_rejects_lossy_coercions_and_incomplete_values) {
   BOOST_CHECK(!decode_case<std::int8_t>("128", true).ok);
   BOOST_CHECK(!decode_case<std::int8_t>("7.5", true).ok);
   BOOST_CHECK(!decode_case<std::int8_t>("null", true).ok);
   BOOST_CHECK(!decode_case<std::int8_t>("7e0", false).ok);
   BOOST_CHECK(!decode_case<std::int8_t>("7e0", true).ok);
   BOOST_CHECK(!decode_case<indexed_choice>("[false,7]", true).ok);
   BOOST_CHECK(!decode_case<indexed_choice>("[-0.5,7]", true).ok);
   BOOST_CHECK(!decode_case<indexed_choice>("[0e0,7]", false).ok);
   BOOST_CHECK(!decode_case<indexed_choice>("[0e0,7]", true).ok);
   BOOST_CHECK(!decode_case<object_choice>("[0,{}]", true).ok);
   BOOST_CHECK(!decode_case<record>(R"({"count":7,"extra":0})", true).ok);
   BOOST_CHECK(!decode_case<schema_record>(R"({"count":"7"})", true).ok);
}

BOOST_AUTO_TEST_CASE(permissive_codec_keeps_exponent_normalization_outside_exact_http_profile) {
   const auto integer = forge::codec::json::read<std::int8_t>("7e0");
   BOOST_REQUIRE(integer.ok());
   BOOST_CHECK_EQUAL(integer.value, 7);
   const auto integer_json = forge::codec::json::write_value(forge::variant{integer.value});
   BOOST_REQUIRE(integer_json.ok());
   BOOST_CHECK_EQUAL(integer_json.text, "7");

   const auto indexed = forge::codec::json::read<indexed_choice>("[0e0,7]");
   BOOST_REQUIRE(indexed.ok());
   BOOST_REQUIRE_EQUAL(indexed.value.index(), 0U);
   BOOST_CHECK_EQUAL(std::get<0>(indexed.value), 7);
   const auto indexed_json = forge::codec::json::write(indexed.value);
   BOOST_REQUIRE(indexed_json.ok());
   BOOST_CHECK_EQUAL(indexed_json.text, "[0,7]");
}

#ifdef __APPLE__
BOOST_AUTO_TEST_CASE(baseline_apple_root_int64_writer_lacks_an_unambiguous_conversion) {
   const auto encoded = forge::codec::json::write(std::int64_t{-9223372036854775807LL - 1LL});
   BOOST_REQUIRE(!encoded.ok());
   BOOST_REQUIRE_EQUAL(encoded.diagnostics.size(), 1U);
   BOOST_CHECK_EQUAL(encoded.diagnostics.front().code, "json.type");
   // The composite codec takes a distinct path and preserves the same integer exactly.
   check_exact_value<std::variant<std::int64_t>>(
      "[0,-9223372036854775808]", std::variant<std::int64_t>{-9223372036854775807LL - 1LL});
}
#endif

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::tests::openapi_wire_baseline
