#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>

import forge.codec.json.document;
import forge.codec.json;
import forge.raw.raw;
import forge.schema.exceptions;

namespace json_document_tests {

struct tool {
   forge::codec::json::document parameters;
   std::optional<forge::codec::json::document> choice;
};

BOOST_DESCRIBE_STRUCT(tool, (), (parameters, choice))

}

BOOST_AUTO_TEST_CASE(json_document_nested_dto_preserves_objects_arrays_and_null)
{
   const auto input = std::string{R"({"parameters":{"type":"object","required":["city"],"additionalProperties":false},"choice":{"type":"function","function":{"name":"weather"}}})"};
   const auto decoded = forge::codec::json::read<json_document_tests::tool>(input,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   const auto json = forge::codec::json::read_value(encoded.text);
   BOOST_REQUIRE(json.ok());
   BOOST_TEST(json.value["parameters"].is_object());
   BOOST_TEST(json.value["parameters"]["required"].is_array());
   BOOST_TEST(json.value["choice"].is_object());
   const auto raw = forge::raw::pack(decoded.value.parameters);
   BOOST_CHECK(raw == forge::raw::pack(decoded.value.parameters.text));
   BOOST_CHECK(forge::raw::unpack_exact<forge::codec::json::document>(raw) == decoded.value.parameters);
}

BOOST_AUTO_TEST_CASE(json_document_parser_rejects_malformed_duplicate_depth_and_bytes)
{
   BOOST_CHECK_THROW(forge::codec::json::parse_document("{"), forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(forge::codec::json::parse_document(R"({"x":1,"x":2})"),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(forge::codec::json::parse_document("[[[0]]]", {.max_depth = 1}),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(forge::codec::json::parse_document("[123]", {.max_bytes = 4}),
                     forge::schema::exceptions::invalid_value);
   BOOST_TEST(forge::codec::json::parse_document("null", {.max_bytes = 4}).text == "null");
}
