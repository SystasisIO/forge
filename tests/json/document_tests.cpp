#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>
#include <vector>

import forge.codec.json.document;
import forge.codec.json;
import forge.raw.raw;
import forge.schema.exceptions;
import forge.schema.object;

namespace json_document_tests {

struct tool {
   forge::codec::json::document parameters;
   std::optional<forge::codec::json::document> choice;
};

struct catalog {
   std::vector<tool> tools;
};

struct holder {
   forge::codec::json::document value;
};

struct documents {
   std::vector<forge::codec::json::document> values;
};

BOOST_DESCRIBE_STRUCT(tool, (), (parameters, choice))
BOOST_DESCRIBE_STRUCT(catalog, (), (tools))
BOOST_DESCRIBE_STRUCT(holder, (), (value))
BOOST_DESCRIBE_STRUCT(documents, (), (values))

}

namespace forge::schema {

template <> struct rules<json_document_tests::catalog> {
   static object_schema<json_document_tests::catalog> define()
   {
      return object<json_document_tests::catalog>().field<&json_document_tests::catalog::tools>("tools");
   }
};

template <> struct rules<json_document_tests::holder> {
   static object_schema<json_document_tests::holder> define()
   {
      return object<json_document_tests::holder>().field<&json_document_tests::holder::value>("value");
   }
};

template <> struct rules<json_document_tests::documents> {
   static object_schema<json_document_tests::documents> define()
   {
      return object<json_document_tests::documents>().field<&json_document_tests::documents::values>("values");
   }
};

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
   BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::parse_document("{")), forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::parse_document(R"({"x":1,"x":2})")),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::parse_document("[[[0]]]", {.max_depth = 1})),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::parse_document("[123]", {.max_bytes = 4})),
                     forge::schema::exceptions::invalid_value);
   BOOST_TEST(forge::codec::json::parse_document("null", {.max_bytes = 4}).text == "null");
}

BOOST_AUTO_TEST_CASE(json_document_schema_bound_root_preserves_dynamic_nested_values)
{
   const auto input = std::string{R"({"tools":[{"parameters":{"type":"object","properties":{"x":{"type":"string"}}},"choice":"auto"}]})"};
   const auto decoded = forge::codec::json::read<json_document_tests::catalog>(input,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   const auto json = forge::codec::json::read_value(encoded.text);
   BOOST_REQUIRE(json.ok());
   BOOST_TEST(json.value["tools"].get_array().front()["parameters"].is_object());
   BOOST_TEST(json.value["tools"].get_array().front()["choice"].get_string() == "auto");
}

BOOST_AUTO_TEST_CASE(json_document_direct_schema_field_preserves_json_null)
{
   const auto decoded = forge::codec::json::read<json_document_tests::holder>(R"({"value":null})",
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   BOOST_TEST(decoded.value.value.text == "null");
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   BOOST_TEST(encoded.text == R"({"value":null})");
}

BOOST_AUTO_TEST_CASE(json_document_requires_one_complete_json_value)
{
   const auto nul_suffix = std::string{"null\0garbage", 12};
   for (const auto& input : {std::string{"null garbage"}, std::string{"{}{}"}, std::string{"true false"}, nul_suffix})
   {
      BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::parse_document(input)),
                        forge::schema::exceptions::invalid_value);
      BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::document_value({input})),
                        forge::schema::exceptions::invalid_value);
   }

   BOOST_TEST(forge::codec::json::parse_document("null \t\r\n").text == "null");
   BOOST_TEST(forge::codec::json::parse_document("{} \t\r\n").text == "{}");
}

BOOST_AUTO_TEST_CASE(json_document_schema_list_preserves_string_null_and_mixed_values)
{
   const auto input = std::string{R"({"values":["null","auto",null,{"x":1},[true,2]]})"};
   const auto decoded = forge::codec::json::read<json_document_tests::documents>(input,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   BOOST_REQUIRE(decoded.value.values.size() == 5);
   BOOST_TEST(decoded.value.values[0].text == R"("null")");
   BOOST_TEST(decoded.value.values[1].text == R"("auto")");
   BOOST_TEST(decoded.value.values[2].text == "null");
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK(forge::codec::json::read_value(encoded.text).value == forge::codec::json::read_value(input).value);
}
