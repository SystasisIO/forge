#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import forge.codec.json.document;
import forge.codec.json;
import forge.raw.raw;
import forge.schema.exceptions;
import forge.schema.object;
import forge.schema.value_kind;

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

struct optional_documents {
   std::vector<std::optional<forge::codec::json::document>> values;
};

struct document_wrappers {
   std::vector<std::optional<std::optional<forge::codec::json::document>>> twice;
   std::vector<std::optional<std::vector<forge::codec::json::document>>> nested;
};

enum class adapted_mode { native_name };
BOOST_DESCRIBE_ENUM(adapted_mode, native_name)

struct adapted_enums {
   adapted_mode scalar = adapted_mode::native_name;
   std::vector<adapted_mode> values;
};

struct constructor_value {
   std::string text;
   constructor_value() = default;
   explicit constructor_value(std::string value) : text(std::move(value)) {}
};

struct constructor_values {
   std::vector<std::optional<constructor_value>> values;
};

BOOST_DESCRIBE_STRUCT(tool, (), (parameters, choice))
BOOST_DESCRIBE_STRUCT(catalog, (), (tools))
BOOST_DESCRIBE_STRUCT(holder, (), (value))
BOOST_DESCRIBE_STRUCT(documents, (), (values))
BOOST_DESCRIBE_STRUCT(optional_documents, (), (values))
BOOST_DESCRIBE_STRUCT(document_wrappers, (), (twice, nested))
BOOST_DESCRIBE_STRUCT(adapted_enums, (), (scalar, values))
BOOST_DESCRIBE_STRUCT(constructor_values, (), (values))

}

namespace forge::schema {

template <> struct value_adapter<json_document_tests::adapted_mode> {
   static constexpr auto kind = value_kind::string;
   static input_value encode(json_document_tests::adapted_mode)
   {
      return input_value{std::string{"custom"}};
   }
   static json_document_tests::adapted_mode decode(const input_value& input)
   {
      const auto* text = std::get_if<std::string>(&input.storage);
      if (!text || *text != "custom") throw std::invalid_argument{"expected custom mode"};
      return json_document_tests::adapted_mode::native_name;
   }
};

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

template <> struct rules<json_document_tests::optional_documents> {
   static object_schema<json_document_tests::optional_documents> define()
   {
      return object<json_document_tests::optional_documents>()
         .field<&json_document_tests::optional_documents::values>("values");
   }
};

template <> struct rules<json_document_tests::document_wrappers> {
   static object_schema<json_document_tests::document_wrappers> define()
   {
      return object<json_document_tests::document_wrappers>()
         .field<&json_document_tests::document_wrappers::twice>("twice")
         .field<&json_document_tests::document_wrappers::nested>("nested");
   }
};

template <> struct rules<json_document_tests::adapted_enums> {
   static object_schema<json_document_tests::adapted_enums> define()
   {
      return object<json_document_tests::adapted_enums>()
         .field<&json_document_tests::adapted_enums::scalar>("scalar")
         .field<&json_document_tests::adapted_enums::values>("values");
   }
};

template <> struct rules<json_document_tests::constructor_values> {
   static object_schema<json_document_tests::constructor_values> define()
   {
      return object<json_document_tests::constructor_values>()
         .field<&json_document_tests::constructor_values::values>("values");
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
   for (const auto& input :
        {std::string{"null garbage"}, std::string{"{}{}"}, std::string{"true false"}, nul_suffix,
         std::string{R"({"outer":{"value":1})"}, std::string{R"({"outer":[1])"}, std::string{R"([{"inner":[]})"}}) {
      BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::parse_document(input)),
                        forge::schema::exceptions::invalid_value);
      BOOST_CHECK_THROW(static_cast<void>(forge::codec::json::document_value({input})),
                        forge::schema::exceptions::invalid_value);
   }

   BOOST_TEST(forge::codec::json::parse_document("null \t\r\n").text == "null");
   BOOST_TEST(forge::codec::json::parse_document("{} \t\r\n").text == "{}");
   constexpr auto expected = std::string_view{R"({"outer":[{}]})"};
   const auto bounded = std::string{expected} + "unused";
   BOOST_TEST(forge::codec::json::parse_document(std::string_view{bounded.data(), expected.size()}).text == expected);
   BOOST_CHECK_THROW(
       static_cast<void>(forge::codec::json::parse_document(std::string_view{bounded.data(), expected.size() - 1})),
       forge::schema::exceptions::invalid_value);
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

BOOST_AUTO_TEST_CASE(json_document_schema_optional_list_preserves_string_null_and_mixed_values)
{
   const auto input = std::string{R"({"values":["null","auto",null,{"x":1},[true,2]]})"};
   const auto decoded = forge::codec::json::read<json_document_tests::optional_documents>(input,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   BOOST_REQUIRE(decoded.value.values.size() == 5);
   BOOST_REQUIRE(decoded.value.values[0]);
   BOOST_REQUIRE(decoded.value.values[1]);
   BOOST_TEST(decoded.value.values[0]->text == R"("null")");
   BOOST_TEST(decoded.value.values[1]->text == R"("auto")");
   BOOST_CHECK(!decoded.value.values[2]);
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK(forge::codec::json::read_value(encoded.text).value == forge::codec::json::read_value(input).value);
}

BOOST_AUTO_TEST_CASE(json_document_schema_recursive_wrappers_preserve_json_types)
{
   const auto input = std::string{R"({"twice":["null","auto",null,{"x":1}],"nested":[["null","auto",null],null]})"};
   const auto decoded = forge::codec::json::read<json_document_tests::document_wrappers>(input,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   BOOST_REQUIRE(decoded.value.twice.size() == 4);
   BOOST_REQUIRE(decoded.value.twice[0] && *decoded.value.twice[0]);
   BOOST_REQUIRE(decoded.value.twice[1] && *decoded.value.twice[1]);
   BOOST_TEST((**decoded.value.twice[0]).text == R"("null")");
   BOOST_TEST((**decoded.value.twice[1]).text == R"("auto")");
   BOOST_CHECK(!decoded.value.twice[2]);
   BOOST_REQUIRE(decoded.value.nested.size() == 2);
   BOOST_REQUIRE(decoded.value.nested[0]);
   BOOST_TEST((*decoded.value.nested[0])[0].text == R"("null")");
   BOOST_CHECK(!decoded.value.nested[1]);
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK(forge::codec::json::read_value(encoded.text).value == forge::codec::json::read_value(input).value);
}

BOOST_AUTO_TEST_CASE(schema_value_adapter_precedence_matches_scalar_and_enum_list)
{
   const auto input = std::string{R"({"scalar":"custom","values":["custom"]})"};
   const auto decoded = forge::codec::json::read<json_document_tests::adapted_enums>(input,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   BOOST_REQUIRE(decoded.value.values.size() == 1);
   BOOST_CHECK(decoded.value.scalar == json_document_tests::adapted_mode::native_name);
   BOOST_CHECK(decoded.value.values[0] == decoded.value.scalar);
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK(forge::codec::json::read_value(encoded.text).value == forge::codec::json::read_value(input).value);
}

BOOST_AUTO_TEST_CASE(schema_list_preserves_constructor_only_optional_values)
{
   using forge::schema::input_value;
   const auto input = input_value::object_type{{"values", input_value{input_value::array_type{
      input_value{std::string{"kept"}}, input_value{}}}}};
   const auto schema = forge::schema::rules<json_document_tests::constructor_values>::define();
   BOOST_CHECK(schema.validate_exact_input(input, "legacy").empty());
   auto decoded = json_document_tests::constructor_values{};
   BOOST_REQUIRE(schema.decode_object(input, "legacy", decoded).empty());
   BOOST_REQUIRE(decoded.values.size() == 2);
   BOOST_REQUIRE(decoded.values[0]);
   BOOST_TEST(decoded.values[0]->text == "kept");
   BOOST_CHECK(!decoded.values[1]);
}
