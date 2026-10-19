#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import forge.schema.object;
import forge.schema.exceptions;
import forge.variant.value;
import forge.variant.schema;
import forge.codec.json;
import forge.api.http.openapi;

namespace schema_constraint_tests {

struct record {
   std::string label;
   std::int64_t count = 0;
   std::string amount;
   forge::variant_object data;
   std::optional<std::string> note;
};

struct item { std::string key; };
struct unsupported { std::vector<item> values; };
struct nested_record { record payload; };
struct envelope { nested_record section; };
struct bounded_byte { std::uint8_t value = 0; };
struct object_shape { bounded_byte value; };
struct dynamic_shape { forge::variant_object value; };

BOOST_DESCRIBE_STRUCT(record, (), (label, count, amount, data, note))
BOOST_DESCRIBE_STRUCT(item, (), (key))
BOOST_DESCRIBE_STRUCT(unsupported, (), (values))
BOOST_DESCRIBE_STRUCT(nested_record, (), (payload))
BOOST_DESCRIBE_STRUCT(envelope, (), (section))
BOOST_DESCRIBE_STRUCT(bounded_byte, (), (value))
BOOST_DESCRIBE_STRUCT(object_shape, (), (value))
BOOST_DESCRIBE_STRUCT(dynamic_shape, (), (value))

} // namespace schema_constraint_tests

namespace forge::schema {

template <> struct rules<schema_constraint_tests::record> {
   static object_schema<schema_constraint_tests::record> define() {
      using record = schema_constraint_tests::record;
      auto schema = object<record>();
      schema.field<&record::label>("label_name").length(1, 16).ascii().pattern("^[a-z][a-z0-9._-]*$");
      schema.field<&record::count>("count").range(std::int64_t{0}, std::int64_t{253402300799});
      schema.field<&record::amount>("amount").decimal_range(0, UINT64_MAX);
      schema.field<&record::data>("data");
      schema.field<&record::note>("note").length(0, 32);
      return schema;
   }
};

template <> struct rules<schema_constraint_tests::unsupported> {
   static object_schema<schema_constraint_tests::unsupported> define() {
      using record = schema_constraint_tests::unsupported;
      return object<record>().field<&record::values>("values").unique_by<&schema_constraint_tests::item::key>();
   }
};

template <> struct rules<schema_constraint_tests::nested_record> {
   static object_schema<schema_constraint_tests::nested_record> define() {
      using type = schema_constraint_tests::nested_record;
      return object<type>().field<&type::payload>("payload");
   }
};

template <> struct rules<schema_constraint_tests::envelope> {
   static object_schema<schema_constraint_tests::envelope> define() {
      using type = schema_constraint_tests::envelope;
      return object<type>().field<&type::section>("section");
   }
};

template <> struct rules<schema_constraint_tests::bounded_byte> {
   static object_schema<schema_constraint_tests::bounded_byte> define() {
      using type = schema_constraint_tests::bounded_byte;
      return object<type>().field<&type::value>("value").shape(constraint{.kind = constraint_kind::integer});
   }
};

template <> struct rules<schema_constraint_tests::object_shape> {
   static object_schema<schema_constraint_tests::object_shape> define() {
      using type = schema_constraint_tests::object_shape;
      return object<type>().field<&type::value>("value").shape(constraint{.kind = constraint_kind::object});
   }
};

template <> struct rules<schema_constraint_tests::dynamic_shape> {
   static object_schema<schema_constraint_tests::dynamic_shape> define() {
      using type = schema_constraint_tests::dynamic_shape;
      auto shape = constraint{.kind = constraint_kind::object};
      shape.properties.emplace("counter", constraint{.kind = constraint_kind::integer,
         .minimum = input_value{std::int64_t{0}}, .maximum = input_value{std::int64_t{10}}});
      shape.required = {"counter"};
      return object<type>().field<&type::value>("value").shape(std::move(shape));
   }
};

} // namespace forge::schema

BOOST_AUTO_TEST_CASE(schema_dynamic_object_adapter_preserves_nested_kinds) {
   auto data = forge::variant{forge::mutable_variant_object{}("null", forge::variant{})
      ("signed", std::int64_t{-7})("unsigned", UINT64_MAX)("double", 0.25)
      ("array", forge::variants{forge::variant{true}, forge::variant{std::string{"text"}},
                                forge::variant{forge::mutable_variant_object{}("inner", false)}})};
   const auto input = forge::schema::value_adapter<forge::variant_object>::encode(data.get_object());
   const auto output = forge::schema::value_adapter<forge::variant_object>::decode(input);
   BOOST_TEST(output["null"].is_null());
   BOOST_TEST(output["signed"].is_int64());
   BOOST_TEST(output["signed"].as_int64() == -7);
   BOOST_TEST(output["unsigned"].is_uint64());
   BOOST_TEST(output["unsigned"].as_uint64() == UINT64_MAX);
   BOOST_TEST(output["double"].is_double());
   BOOST_TEST(output["double"].as_double() == 0.25);
   BOOST_TEST(output["array"].get_array()[2].get_object()["inner"].is_bool());
   for (const auto& wrong : {forge::schema::input_value{}, forge::schema::input_value{true},
                             forge::schema::input_value{std::string{"{}"}},
                             forge::schema::input_value{forge::schema::input_value::array_type{}}}) {
      BOOST_CHECK_THROW(forge::schema::value_adapter<forge::variant_object>::decode(wrong),
                        forge::schema::exceptions::invalid_value);
   }
   BOOST_CHECK_THROW(forge::variant_schema::to_input(data, {.max_depth = 1}),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(forge::variant_schema::to_input(data, {.max_nodes = 2}),
                     forge::schema::exceptions::invalid_value);
}

BOOST_AUTO_TEST_CASE(schema_typed_and_artifact_rules_have_one_source) {
   using record = schema_constraint_tests::record;
   const auto rules = forge::schema::export_constraints<record>();
   const auto encoded = forge::variant_schema::from_input(forge::schema::export_artifact(rules));
   const auto text = forge::codec::json::write_value(encoded);
   BOOST_REQUIRE(text.ok());
   const auto local = forge::codec::json::read_value(text.text,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(local.ok());
   const auto loaded = forge::schema::load_artifact(forge::variant_schema::to_input(local.value));
   const auto json = std::string{R"({"label_name":"read.v1","count":253402300799,"amount":"18446744073709551615","data":{"nested":[null,true,7]}})"};
   const auto typed = forge::codec::json::read<record>(json,
      {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(typed.ok());
   BOOST_REQUIRE(forge::codec::json::read(json, loaded).ok());
   const auto roundtrip = forge::codec::json::write(typed.value);
   BOOST_REQUIRE(roundtrip.ok());
   BOOST_REQUIRE(forge::codec::json::read(roundtrip.text, loaded).ok());
   auto invalid_output = typed.value;
   invalid_output.count = -1;
   BOOST_TEST(!forge::codec::json::write(invalid_output).ok());
   const auto with_null = std::string{R"({"label_name":"read","count":1,"amount":"1","data":{},"note":null})"};
   BOOST_TEST(forge::codec::json::read(with_null, loaded).ok());
   for (const auto& invalid : {
      R"({"label_name":"read","count":1,"amount":"18446744073709551616","data":{}})",
      R"({"label_name":"read","count":1,"amount":"01","data":{}})",
      R"({"label_name":"read","count":1e0,"amount":"1","data":{}})",
      R"({"label_name":"read","count":253402300800,"amount":"1","data":{}})",
      R"({"label_name":"read","count":true,"amount":"1","data":{}})",
      R"({"label_name":"read","count":1,"amount":"1","data":[]})",
      R"({"label_name":"read\n","count":1,"amount":"1","data":{}})",
      R"({"label_name":"read","count":1,"amount":"1","data":{},"extra":0})",
      R"({"label":"read","count":1,"amount":"1","data":{}})"}) {
      BOOST_TEST(!forge::codec::json::read(invalid, loaded).ok());
      BOOST_TEST(!forge::codec::json::read<record>(invalid,
         {.described_records = forge::codec::json::described_record_policy::exact}).ok());
   }
}

BOOST_AUTO_TEST_CASE(schema_writer_validates_nested_records_before_emitting_json) {
   auto value = schema_constraint_tests::envelope{};
   value.section.payload.label = "read";
   value.section.payload.amount = "1";
   BOOST_REQUIRE(forge::codec::json::write(value).ok());
   value.section.payload.count = 253402300800;
   const auto written = forge::codec::json::write(value);
   BOOST_TEST(!written.ok());
   BOOST_TEST(written.text.empty());
   BOOST_REQUIRE(!written.diagnostics.empty());
   BOOST_TEST(written.diagnostics.front().path == "section.payload.count");
   BOOST_CHECK_THROW(forge::variant_schema::encode(value), forge::schema::encoding_error);
}

BOOST_AUTO_TEST_CASE(schema_shapes_preserve_integer_type_bounds_and_reject_unsupported_object_intersections) {
   const auto artifact = forge::schema::export_constraints<schema_constraint_tests::bounded_byte>();
   const auto& field = artifact.root.properties.at("value");
   BOOST_REQUIRE(field.maximum.has_value());
   BOOST_TEST(std::get<std::uint64_t>(field.maximum->storage) == 255U);
   BOOST_TEST(forge::codec::json::read(R"({"value":255})", artifact).ok());
   BOOST_TEST(!forge::codec::json::read(R"({"value":300})", artifact).ok());
   BOOST_CHECK_THROW(forge::schema::export_constraints<schema_constraint_tests::object_shape>(),
                     forge::schema::exceptions::unsupported_export);
   const auto dynamic = forge::schema::export_constraints<schema_constraint_tests::dynamic_shape>();
   BOOST_TEST(forge::codec::json::read(R"({"value":{"counter":10}})", dynamic).ok());
   for (const auto* invalid : {R"({"value":{"counter":11}})", R"({"value":{"counter":1,"extra":0}})"}) {
      BOOST_TEST(!forge::codec::json::read(invalid, dynamic).ok());
      BOOST_TEST(!forge::codec::json::read<schema_constraint_tests::dynamic_shape>(invalid,
         {.described_records = forge::codec::json::described_record_policy::exact}).ok());
   }
}

BOOST_AUTO_TEST_CASE(schema_dynamic_consumer_needs_only_local_data_artifact) {
   using namespace forge::schema;
   auto object = constraint{.kind = constraint_kind::object};
   object.properties.emplace("version", constraint{.kind = constraint_kind::integer,
      .constant = input_value{std::uint64_t{1}}});
   object.properties.emplace("names", constraint{.kind = constraint_kind::array,
      .items = {constraint{.kind = constraint_kind::string, .min_size = 1, .max_size = 8,
         .enum_values = {input_value{std::string{"read"}}, input_value{std::string{"write"}}}}},
      .min_size = 1, .max_size = 2});
   object.required = {"version", "names"};
   const auto local = load_artifact(export_artifact(artifact{.root = object}));
   BOOST_TEST(forge::codec::json::read(R"({"version":1,"names":["read","write"]})", local).ok());
   for (const auto* bad : {R"({"version":2,"names":["read"]})", R"({"version":1,"names":[]})",
      R"({"version":1,"names":["other"]})", R"({"version":1,"names":["read"],"unknown":true})"})
      BOOST_TEST(!forge::codec::json::read(bad, local).ok());
}

BOOST_AUTO_TEST_CASE(schema_decimal_regex_preserves_both_endpoints) {
   using namespace forge::schema;
   const auto rules = artifact{.root = constraint{.kind = constraint_kind::string,
      .decimal_minimum = 37, .decimal_maximum = 142}};
   const auto exported = forge::variant_schema::from_input(json_schema(rules));
   const auto regex = std::regex{exported.get_object()["pattern"].as_string()};
   for (std::uint64_t value = 0; value < 200; ++value) {
      const auto expected = value >= 37 && value <= 142;
      const auto text = std::to_string(value);
      BOOST_TEST(std::regex_search(text, regex) == expected);
      BOOST_TEST(validate(rules, input_value{text}).empty() == expected);
   }
}

BOOST_AUTO_TEST_CASE(schema_json_schema_and_openapi_preserve_names_ranges_and_decimal_bound) {
   using record = schema_constraint_tests::record;
   const auto expected = forge::variant_schema::from_input(
      forge::schema::json_schema(forge::schema::export_constraints<record>()));
   const auto openapi = forge::api::http::detail::make_json_schema<record>();
   BOOST_TEST(forge::codec::json::write_value(expected).text == forge::codec::json::write_value(openapi).text);
   const auto& properties = openapi.get_object()["properties"].get_object();
   BOOST_TEST(properties.contains("label_name"));
   BOOST_TEST(!properties.contains("label"));
   BOOST_TEST(properties["count"].get_object()["maximum"].as_int64() == 253402300799);
   const auto pattern = properties["amount"].get_object()["pattern"].as_string();
   const auto regex = std::regex{pattern};
   BOOST_TEST(std::regex_search(std::string{"18446744073709551615"}, regex));
   BOOST_TEST(!std::regex_search(std::string{"18446744073709551616"}, regex));
   BOOST_TEST(!std::regex_search(std::string{"18446744073709551615\n"}, regex));
   const auto fields = forge::api::http::detail::request_fields<record>();
   BOOST_REQUIRE(!fields.empty());
   BOOST_TEST(fields.front().name == "label_name");
   BOOST_CHECK_THROW(forge::schema::export_constraints<schema_constraint_tests::unsupported>(),
                     forge::schema::exceptions::unsupported_export);
   BOOST_CHECK_THROW(forge::api::http::detail::make_json_schema<schema_constraint_tests::unsupported>(),
                     forge::schema::exceptions::unsupported_export);
}

BOOST_AUTO_TEST_CASE(schema_local_artifact_loader_rejects_versions_refs_kinds_and_limits) {
   using namespace forge::schema;
   auto valid = export_artifact(export_constraints<schema_constraint_tests::record>());
   auto version = valid;
   std::get<input_value::object_type>(version.storage)["version"] = input_value{std::uint64_t{2}};
   BOOST_CHECK_THROW(load_artifact(version), exceptions::unsupported_export);
   auto reference = valid;
   auto& envelope = std::get<input_value::object_type>(reference.storage);
   std::get<input_value::object_type>(envelope.at("schema").storage)["$ref"] = input_value{std::string{"https://invalid/schema"}};
   BOOST_CHECK_THROW(load_artifact(reference), exceptions::unsupported_export);
   BOOST_CHECK_THROW(load_artifact(valid, {.max_nodes = 2}), exceptions::unsupported_export);
   auto invalid = artifact{.root = constraint{.kind = constraint_kind::integer, .pattern = "^x$"}};
   BOOST_CHECK_THROW(json_schema(invalid), exceptions::unsupported_export);
}

BOOST_AUTO_TEST_CASE(schema_export_errors_preserve_structured_context) {
   const auto expect_context = [](auto action, std::string_view message, std::string_view key,
                                   std::string_view value) {
      try {
         action();
         BOOST_FAIL("expected unsupported_export");
      } catch (const forge::schema::exceptions::unsupported_export& error) {
         BOOST_TEST(error.message() == message);
         BOOST_REQUIRE_EQUAL(error.context().size(), 1U);
         BOOST_TEST(error.context().front().key == key);
         BOOST_TEST(error.context().front().value == value);
      }
   };
   expect_context([] { static_cast<void>(forge::schema::json_schema(forge::schema::artifact{.version = 2})); },
      "unsupported constraint artifact", "reason", "unsupported version");
   expect_context([] { static_cast<void>(forge::schema::export_constraints<schema_constraint_tests::unsupported>()); },
      "unrepresentable schema field", "field", "values");
}

BOOST_AUTO_TEST_CASE(schema_unicode_lengths_and_json_byte_budgets_are_distinct) {
   using record = schema_constraint_tests::record;
   const auto artifact = forge::schema::export_constraints<record>();
   const auto exported = forge::variant_schema::from_input(forge::schema::json_schema(artifact));
   const auto& note = exported.get_object()["properties"].get_object()["note"].get_object()["anyOf"].get_array()[0];
   BOOST_TEST(note.get_object()["maxLength"].as_uint64() == 32U);
   for (const auto& scalar : {std::string{"\xc3\xa9"}, std::string{"\xf0\x9f\x98\x80"}}) {
      auto text = std::string{};
      for (unsigned count = 0; count < 32; ++count) text += scalar;
      const auto document = [&](const std::string& value) {
         return std::string{R"({"label_name":"read","count":1,"amount":"1","data":{},"note":")"} + value + R"("})";
      };
      BOOST_TEST(forge::codec::json::read<record>(document(text),
         {.described_records = forge::codec::json::described_record_policy::exact}).ok());
      BOOST_TEST(forge::codec::json::read(document(text), artifact).ok());
      BOOST_TEST(!forge::codec::json::read_value(document(text),
         {.described_records = forge::codec::json::described_record_policy::exact, .max_string_bytes = 32}).ok());
      text += scalar;
      BOOST_TEST(!forge::codec::json::read<record>(document(text),
         {.described_records = forge::codec::json::described_record_policy::exact}).ok());
      BOOST_TEST(!forge::codec::json::read(document(text), artifact).ok());
   }
}

BOOST_AUTO_TEST_CASE(schema_artifact_patterns_use_bounded_backend_not_safety_analysis) {
   using namespace forge::schema;
   for (const auto* pattern : {"^([a-z]$", "^(?=a)a$", "^(a)\\1$"}) {
      const auto rules = artifact{.root = constraint{.kind = constraint_kind::string, .pattern = pattern, .ascii = true}};
      BOOST_CHECK_THROW(export_artifact(rules), exceptions::unsupported_export);
   }
   const auto adversarial = artifact{.root = constraint{.kind = constraint_kind::string,
      .pattern = "^(a+)+$", .ascii = true}};
   BOOST_CHECK_NO_THROW(load_artifact(export_artifact(adversarial)));
   const auto input = input_value{std::string(4096, 'a') + "!"};
   // A syntactically valid adversarial expression reaches Boost's state/stack
   // budget and returns a typed failure, not a timer or a homegrown heuristic.
   BOOST_CHECK_THROW(validate(adversarial, input), exceptions::constraint_limit);
   const auto uri = artifact{.root = constraint{.kind = constraint_kind::string,
      .pattern = "^[A-Za-z][A-Za-z0-9+.-]*:[!-~]*$", .ascii = true}};
   BOOST_CHECK_NO_THROW(load_artifact(export_artifact(uri)));
   BOOST_TEST(validate(uri, input_value{std::string{"example:read/v1"}}).empty());
   const auto alternatives = artifact{.root = constraint{.kind = constraint_kind::string, .pattern = "^a|b$", .ascii = true}};
   const auto exported = forge::variant_schema::from_input(json_schema(alternatives));
   const auto expression = std::regex{exported.get_object()["pattern"].as_string()};
   BOOST_TEST(!std::regex_search(std::string{"xxb"}, expression));
   BOOST_TEST(!std::regex_search(std::string{"axy"}, expression));
}
