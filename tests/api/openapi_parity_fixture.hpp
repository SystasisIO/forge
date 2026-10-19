#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.server_supplied;
import forge.api.http.binding;
import forge.api.http.mapping;
import forge.api.http.openapi;
import forge.api.http.proxy;
import forge.codec.json;
import forge.net.http.types;
import forge.schema.object;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.described;
import forge.variant.static_variant;
import forge.variant.value;

namespace forge::tests::openapi_parity {

using indexed_choice = std::variant<std::int8_t, std::string>;
using overlapping_choice = std::variant<std::int8_t, std::int16_t>;

struct record {
   std::int16_t count = 0;
   std::optional<std::uint64_t> maybe;
   bool operator==(const record&) const = default;
};

using object_choice = std::variant<record, std::string>;
using overlapping_objects = std::variant<record, record>;
using fixed_array = std::array<std::int16_t, 2>;
using choice_list = std::vector<indexed_choice>;
using uint64_list = std::vector<std::uint64_t>;

struct nested {
   record child;
   choice_list choices;
   bool operator==(const nested&) const = default;
};

struct schema_record {
   std::int16_t count = 0;
};

struct values {
   std::int8_t signed8 = 0;
   std::uint8_t unsigned8 = 0;
   std::int16_t signed16 = 0;
   std::uint16_t unsigned16 = 0;
   std::int32_t signed32 = 0;
   std::uint32_t unsigned32 = 0;
   std::int64_t signed64 = 0;
   std::uint64_t unsigned64 = 0;
   indexed_choice indexed;
   overlapping_choice overlapping;
   object_choice object;
   overlapping_objects object_overlap;
   schema_record checked;
   record item;
   nested tree;
   fixed_array pair{};
   choice_list choices;
   uint64_list large;
};

struct server_context {
   std::string id;
};

class api : public forge::api::core::contract<api, forge::api::core::surface::local |
                                                forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<values> inspect(values input) = 0;
   virtual boost::asio::awaitable<record> fixed(server_context trusted, record input) = 0;
};

BOOST_DESCRIBE_STRUCT(record, (), (count, maybe))
BOOST_DESCRIBE_STRUCT(nested, (), (child, choices))
BOOST_DESCRIBE_STRUCT(schema_record, (), (count))
BOOST_DESCRIBE_STRUCT(values, (),
                      (signed8, unsigned8, signed16, unsigned16, signed32, unsigned32,
                       signed64, unsigned64, indexed, overlapping, object, object_overlap, checked,
                       item, tree, pair, choices, large))
BOOST_DESCRIBE_STRUCT(server_context, (), (id))

} // namespace forge::tests::openapi_parity

namespace forge::schema {

template <> struct rules<forge::tests::openapi_parity::schema_record> {
   static object_schema<forge::tests::openapi_parity::schema_record> define() {
      auto schema = object<forge::tests::openapi_parity::schema_record>();
      schema.field<&forge::tests::openapi_parity::schema_record::count>("count").required();
      return schema;
   }
};

} // namespace forge::schema

namespace forge::api::core {

template <> struct server_supplied<forge::tests::openapi_parity::server_context> {
   static constexpr bool required = true;

   static void reset(forge::tests::openapi_parity::server_context& value) {
      value = {};
   }

   static bool apply(forge::tests::openapi_parity::server_context& value, const trusted_invocation& trusted) {
      const auto* source = trusted.find<forge::tests::openapi_parity::server_context>();
      if (source == nullptr) {
         return false;
      }
      value = *source;
      return true;
   }
};

} // namespace forge::api::core

FORGE_API(::forge::tests::openapi_parity::api, FORGE_API_CONTRACT("openapi-parity", 1, 0),
          FORGE_API_METHOD_TYPED(inspect, ::forge::tests::openapi_parity::values,
                                 ::forge::tests::openapi_parity::values),
          FORGE_API_METHOD(fixed, trusted, input))
FORGE_HTTP_API(::forge::tests::openapi_parity::api,
               FORGE_HTTP_POST(inspect, "/parity/values", ok),
               FORGE_HTTP_POST(fixed, "/parity/fixed", ok))

namespace forge::tests::openapi_parity {

inline const auto exact_options = forge::codec::json::read_options{
   .described_records = forge::codec::json::described_record_policy::exact,
};

// Mirrors the typed HTTP JSON policy; real loopback tests guard this mapping.
inline const auto http_options = forge::codec::json::read_options{
   .unknown_fields = forge::codec::json::unknown_field_policy::error,
   .described_records = forge::codec::json::described_record_policy::exact,
};

struct schema_name {
   const char* property;
   const char* component;
};

inline constexpr auto schema_names = std::array{
   schema_name{"signed8", "Int8"}, schema_name{"unsigned8", "UInt8"},
   schema_name{"signed16", "Int16"}, schema_name{"unsigned16", "UInt16"},
   schema_name{"signed32", "Int32"}, schema_name{"unsigned32", "UInt32"},
   schema_name{"signed64", "Int64"}, schema_name{"unsigned64", "UInt64"},
   schema_name{"indexed", "IndexedChoice"}, schema_name{"overlapping", "OverlappingChoice"},
   schema_name{"object", "ObjectChoice"}, schema_name{"item", "Record"},
   schema_name{"object_overlap", "OverlappingObjects"}, schema_name{"checked", "SchemaRecord"},
   schema_name{"tree", "Nested"}, schema_name{"pair", "FixedArray"},
   schema_name{"choices", "ChoiceList"}, schema_name{"large", "UInt64List"},
};

inline const forge::variant& request_schema(const forge::variant& document, const char* path = "/parity/values") {
   return document["paths"][path]["post"]["requestBody"]["content"]["application/json"]["schema"];
}

inline forge::variant make_document() {
   auto generated = forge::api::http::openapi<api>({.title = "OpenAPI parity", .version = "1.0.0"});
   const auto& properties = request_schema(generated)["properties"];
   auto schemas = forge::mutable_variant_object{};
   for (const auto& entry : schema_names) {
      schemas.set(entry.component, properties[entry.property]);
   }
   // Component promotion copies real typed-operation schemas without rewriting them.
   auto document = forge::mutable_variant_object{generated.get_object()};
   document.set("components", forge::variant{forge::mutable_variant_object{}("schemas", std::move(schemas))});
   return forge::variant{std::move(document)};
}

struct codec_result {
   bool ok = false;
   std::string canonical_json;
};

template <typename T> codec_result decode_case(std::string_view json, bool http) {
   const auto decoded = forge::codec::json::read<T>(json, http ? http_options : exact_options);
   if (!decoded.ok()) {
      return {};
   }
   const auto encoded = [&] {
      if constexpr (std::is_integral_v<T>) {
         // The existing variant constructor preserves integer kind and width.
         // This does not exercise or fix the known Apple root-int64 write<T> issue.
         return forge::codec::json::write_value(forge::variant{decoded.value});
      } else {
         return forge::codec::json::write(decoded.value);
      }
   }();
   if (!encoded.ok()) {
      throw std::runtime_error{"failed to serialize an accepted parity fixture value"};
   }
   return {true, encoded.text};
}

struct case_definition {
   std::string name;
   const char* schema;
   std::string json;
   bool expected;
   bool expected_codec;
   std::optional<bool> expected_http;
   const char* category;
   const char* note;
   codec_result (*decode)(std::string_view, bool);
};

template <typename T>
void add_case(std::vector<case_definition>& cases, std::string name, const char* schema,
              std::string json, bool expected) {
   cases.push_back({std::move(name), schema, std::move(json), expected, expected, expected,
                    "canonical", "Typed HTTP JSON uses the exact codec policy.", &decode_case<T>});
}

template <typename T>
void add_compatibility(std::vector<case_definition>& cases, const char* name, const char* schema,
                       const char* json, bool expected, bool expected_codec, bool expected_http,
                       const char* category, const char* note) {
   cases.push_back({name, schema, json, expected, expected_codec, expected_http, category, note,
                    &decode_case<T>});
}

template <typename T>
void add_integer_cases(std::vector<case_definition>& cases, const char* schema,
                       const char* minimum, const char* maximum, const char* underflow, const char* overflow) {
   const auto prefix = std::string{schema};
   add_case<T>(cases, prefix + " minimum", schema, minimum, true);
   add_case<T>(cases, prefix + " maximum", schema, maximum, true);
   add_case<T>(cases, prefix + " underflow", schema, underflow, false);
   add_case<T>(cases, prefix + " overflow", schema, overflow, false);
}

inline std::vector<case_definition> make_cases() {
   auto cases = std::vector<case_definition>{};
   add_integer_cases<std::int8_t>(cases, "Int8", "-128", "127", "-129", "128");
   add_integer_cases<std::uint8_t>(cases, "UInt8", "0", "255", "-1", "256");
   add_integer_cases<std::int16_t>(cases, "Int16", "-32768", "32767", "-32769", "32768");
   add_integer_cases<std::uint16_t>(cases, "UInt16", "0", "65535", "-1", "65536");
   add_integer_cases<std::int32_t>(cases, "Int32", "-2147483648", "2147483647", "-2147483649", "2147483648");
   add_integer_cases<std::uint32_t>(cases, "UInt32", "0", "4294967295", "-1", "4294967296");
   add_integer_cases<std::int64_t>(cases, "Int64", "-9223372036854775808", "9223372036854775807",
                                  "-9223372036854775809", "9223372036854775808");
   add_integer_cases<std::uint64_t>(cases, "UInt64", "0", "18446744073709551615", "-1", "18446744073709551616");
   add_case<std::uint64_t>(cases, "UInt64 above binary64 exact range", "UInt64", "9007199254740993", true);
   add_case<uint64_list>(cases, "UInt64 nested exact values", "UInt64List",
                         "[9007199254740993,18446744073709551615]", true);
   add_case<uint64_list>(cases, "UInt64 nested overflow", "UInt64List", "[18446744073709551616]", false);

   add_case<indexed_choice>(cases, "indexed integer", "IndexedChoice", "[0,7]", true);
   add_case<indexed_choice>(cases, "indexed string", "IndexedChoice", R"([1,"text"])", true);
   add_case<indexed_choice>(cases, "integer index with text", "IndexedChoice", R"([0,"text"])", false);
   add_case<indexed_choice>(cases, "string index with integer", "IndexedChoice", "[1,7]", false);
   add_case<indexed_choice>(cases, "indexed payload overflow", "IndexedChoice", "[0,128]", false);
   add_case<indexed_choice>(cases, "negative index", "IndexedChoice", "[-1,7]", false);
   add_case<indexed_choice>(cases, "out of range index", "IndexedChoice", "[2,7]", false);
   add_case<indexed_choice>(cases, "uint64 maximum index", "IndexedChoice", "[18446744073709551615,7]", false);
   add_case<indexed_choice>(cases, "missing payload", "IndexedChoice", "[0]", false);
   add_case<indexed_choice>(cases, "empty variant", "IndexedChoice", "[]", false);
   add_case<indexed_choice>(cases, "variant trailing item", "IndexedChoice", "[0,7,8]", false);

   add_case<overlapping_choice>(cases, "overlap first alternative", "OverlappingChoice", "[0,7]", true);
   add_case<overlapping_choice>(cases, "overlap second alternative", "OverlappingChoice", "[1,7]", true);
   add_case<overlapping_choice>(cases, "overlap first overflow", "OverlappingChoice", "[0,128]", false);
   add_case<overlapping_choice>(cases, "overlap second wider range", "OverlappingChoice", "[1,128]", true);
   add_case<overlapping_choice>(cases, "overlap second minimum", "OverlappingChoice", "[1,-32768]", true);
   add_case<overlapping_choice>(cases, "overlap second maximum", "OverlappingChoice", "[1,32767]", true);
   add_case<overlapping_choice>(cases, "overlap second overflow", "OverlappingChoice", "[1,32768]", false);

   add_case<record>(cases, "optional missing", "Record", R"({"count":7})", true);
   add_case<record>(cases, "optional null", "Record", R"({"count":7,"maybe":null})", true);
   add_case<record>(cases, "optional value", "Record", R"({"count":7,"maybe":18446744073709551615})", true);
   add_case<record>(cases, "optional wrong type", "Record", R"({"count":7,"maybe":"7"})", false);
   add_case<record>(cases, "missing required field", "Record", "{}", false);
   add_case<record>(cases, "unknown field", "Record", R"({"count":7,"extra":0})", false);
   add_case<record>(cases, "null required field", "Record", R"({"count":null})", false);

   add_case<object_choice>(cases, "object alternative", "ObjectChoice", R"([0,{"count":7}])", true);
   add_case<object_choice>(cases, "object alternative string branch", "ObjectChoice", R"([1,"text"])", true);
   add_case<object_choice>(cases, "object alternative wrong pairing", "ObjectChoice", R"([1,{"count":7}])", false);
   add_case<object_choice>(cases, "object alternative missing field", "ObjectChoice", "[0,{}]", false);
   add_case<object_choice>(cases, "object alternative unknown field", "ObjectChoice", R"([0,{"count":7,"extra":0}])", false);
   add_case<overlapping_objects>(cases, "overlapping object first index", "OverlappingObjects", R"([0,{"count":7}])", true);
   add_case<overlapping_objects>(cases, "overlapping object second index", "OverlappingObjects", R"([1,{"count":7}])", true);
   add_case<overlapping_objects>(cases, "overlapping object invalid index", "OverlappingObjects", R"([2,{"count":7}])", false);
   add_case<overlapping_objects>(cases, "overlapping object invalid payload", "OverlappingObjects", "[1,7]", false);

   add_case<nested>(cases, "nested records and arrays", "Nested",
                    R"({"child":{"count":7,"maybe":9007199254740993},"choices":[[0,7],[1,"text"]]})", true);
   add_case<nested>(cases, "nested missing field", "Nested", R"({"child":{},"choices":[]})", false);
   add_case<nested>(cases, "nested unknown field", "Nested", R"({"child":{"count":7,"extra":0},"choices":[]})", false);
   add_case<nested>(cases, "nested wrong pairing", "Nested", R"({"child":{"count":7},"choices":[[1,7]]})", false);
   add_case<choice_list>(cases, "array of variants", "ChoiceList", R"([[0,-128],[1,"text"]])", true);
   add_case<choice_list>(cases, "array nested trailing item", "ChoiceList", "[[0,7,8]]", false);
   add_case<choice_list>(cases, "array boolean index", "ChoiceList", "[[false,7]]", false);
   add_case<nested>(cases, "nested boolean index", "Nested",
                    R"({"child":{"count":7},"choices":[[true,"text"]]})", false);
   add_case<fixed_array>(cases, "fixed array", "FixedArray", "[-32768,32767]", true);
   add_case<fixed_array>(cases, "fixed array missing item", "FixedArray", "[7]", false);
   add_case<fixed_array>(cases, "fixed array trailing item", "FixedArray", "[7,8,9]", false);
   add_case<fixed_array>(cases, "fixed array overflow", "FixedArray", "[7,32768]", false);

   add_compatibility<std::int8_t>(cases, "HTTP quoted integer", "Int8", R"("7")", false, false, false,
       "http_strict", "HTTP rejects quoted integer payloads.");
   add_compatibility<std::int8_t>(cases, "HTTP narrowing overflow", "Int8", "128", false, false, false,
       "http_strict", "HTTP rejects narrowing overflow instead of wrapping to -128.");
   add_compatibility<record>(cases, "HTTP missing record field", "Record", "{}", false, false, false,
       "http_strict", "HTTP rejects missing non-optional fields on plain described records.");
   add_compatibility<record>(cases, "HTTP unknown record field", "Record", R"({"count":7,"extra":0})", false, false, false,
       "http_strict", "HTTP rejects unknown plain-record fields.");
   add_compatibility<record>(cases, "HTTP quoted record integer", "Record", R"({"count":"7"})", false, false, false,
       "http_strict", "Nested integer fields do not accept string coercion.");
   add_compatibility<object_choice>(cases, "HTTP object variant missing field", "ObjectChoice", "[0,{}]", false, false, false,
       "http_strict", "HTTP validates the selected object alternative before conversion.");
   add_compatibility<indexed_choice>(cases, "HTTP variant object encoding", "IndexedChoice",
       R"({"index":0,"value":7})", false, false, false, "alternative_encoding",
       "This std::variant decoder rejects object-form alternatives in both modes.");
   add_compatibility<indexed_choice>(cases, "HTTP variant trailing item", "IndexedChoice", "[0,7,8]", false, false, false,
       "http_strict", "HTTP requires exactly two variant elements.");
   add_compatibility<indexed_choice>(cases, "HTTP variant missing payload", "IndexedChoice", "[0]", false, false, false,
       "http_strict", "HTTP rejects missing variant payloads.");
   add_compatibility<indexed_choice>(cases, "HTTP quoted index", "IndexedChoice", R"(["0",7])", false, false, false,
       "http_strict", "Variant indexes must not be strings.");
   add_compatibility<indexed_choice>(cases, "HTTP fractional index", "IndexedChoice", "[0.5,7]", false, false, false,
       "http_strict", "Variant indexes must not be fractional numbers.");
   add_compatibility<std::int8_t>(cases, "integer decimal spelling", "Int8", "7.0", true, false, false,
       "integer_lexical", "JSON Schema integer is mathematical; exact native decoding requires an integer JSON token.");
   add_compatibility<std::int8_t>(cases, "integer exponent spelling", "Int8", "7e0", true, false, false,
       "integer_lexical", "JSON Schema integer is mathematical; exact native decoding rejects exponent tokens.");
   add_compatibility<indexed_choice>(cases, "index decimal spelling", "IndexedChoice", "[0.0,7]", true, false, false,
       "integer_lexical", "JSON Schema const 0 matches 0.0; exact native variant indexes require integer tokens.");
   add_compatibility<indexed_choice>(cases, "index exponent spelling", "IndexedChoice", "[0e0,7]", true, false, false,
       "integer_lexical", "JSON Schema const 0 matches 0e0; exact native variant indexes require integer tokens.");
   add_compatibility<indexed_choice>(cases, "boolean false index", "IndexedChoice", "[false,7]", false, false, false,
       "literal_index", "Schema and HTTP reject boolean indexes; raw Pydantic literals remain separately observable.");
   add_compatibility<indexed_choice>(cases, "boolean true index", "IndexedChoice", R"([true,"text"])", false, false, false,
       "literal_index", "Schema and HTTP reject boolean indexes; Python consumers must validate before normalization.");
   add_compatibility<indexed_choice>(cases, "null index", "IndexedChoice", "[null,7]", false, false, false,
       "http_strict", "Null cannot select a variant alternative.");
   add_compatibility<indexed_choice>(cases, "negative fractional index", "IndexedChoice", "[-0.5,7]", false, false, false,
       "http_strict", "Negative fractional indexes are rejected, not truncated.");
   add_compatibility<indexed_choice>(cases, "nonnumeric string index", "IndexedChoice", R"(["bad",7])", false, false, false,
       "http_strict", "Neither decoder accepts a nonnumeric index string.");
   add_compatibility<indexed_choice>(cases, "second index decimal spelling", "IndexedChoice", R"([1.0,"text"])", true, false, false,
       "integer_lexical", "The second alternative has the same integer-token versus mathematical-integer boundary.");
   add_compatibility<std::int8_t>(cases, "boolean integer payload", "Int8", "true", false, false, false,
       "http_strict", "Boolean is not an integer payload.");
   add_compatibility<std::int8_t>(cases, "null integer payload", "Int8", "null", false, false, false,
       "http_strict", "Null is only accepted by nullable types.");
   add_compatibility<std::int8_t>(cases, "fractional integer payload", "Int8", "7.5", false, false, false,
       "http_strict", "Fractional integer payloads are rejected instead of truncated.");
   add_compatibility<schema_record>(cases, "schema record valid", "SchemaRecord", R"({"count":7})", true, true, true,
       "schema_bound", "Explicit schema rules preserve an ordinary valid record.");
   add_compatibility<schema_record>(cases, "schema record missing required", "SchemaRecord", "{}", false, false, false,
       "schema_bound", "Explicit required schema rules reject missing fields in HTTP mode too.");
   add_compatibility<schema_record>(cases, "schema record unknown field", "SchemaRecord", R"({"count":7,"extra":0})", false, false, false,
       "schema_bound", "Explicit schema rules expose unknown fields to the HTTP unknown_fields=error policy.");
   add_compatibility<schema_record>(cases, "schema record quoted integer", "SchemaRecord", R"({"count":"7"})", false, false, false,
       "schema_bound", "Explicit schema-bound integer fields also reject scalar text.");
   add_compatibility<schema_record>(cases, "schema record overflow", "SchemaRecord", R"({"count":32768})", false, false, false,
       "schema_bound", "Schema-bound integer conversion checks range instead of silently narrowing.");
   return cases;
}

inline forge::variant evaluate_case(const case_definition& entry) {
   const auto codec = entry.decode(entry.json, false);
   if (codec.ok != entry.expected_codec) {
      throw std::runtime_error{"unexpected exact codec verdict: " + entry.name};
   }
   auto result = forge::mutable_variant_object{}("name", entry.name)("schema", entry.schema)("json", entry.json)(
       "expected", entry.expected)("expected_codec", entry.expected_codec)("codec_ok", codec.ok)("category", entry.category);
   if (codec.ok) {
      result("codec_json", codec.canonical_json);
   }
   if (entry.expected_http.has_value()) {
      const auto http = entry.decode(entry.json, true);
      if (http.ok != *entry.expected_http) {
         throw std::runtime_error{"unexpected HTTP-options codec verdict: " + entry.name};
      }
      result("codec_http_ok", http.ok)("expected_http", *entry.expected_http)("note", entry.note);
      if (http.ok) {
         result("codec_http_json", http.canonical_json);
      }
   }
   return forge::variant{std::move(result)};
}

} // namespace forge::tests::openapi_parity
