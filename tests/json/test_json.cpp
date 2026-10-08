#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace forge_json_tests {

struct http_config {
   std::uint16_t bind_port = 0;
   std::string bind_host;
   bool tls_enabled = false;
   std::vector<std::string> tags;
};

struct named_tag {
   std::string value;

   named_tag() = default;
   explicit named_tag(std::string input) : value(std::move(input)) {}
};

struct tag_config {
   std::vector<named_tag> tags;
};

enum class path_policy {
   direct_only,
   direct_preferred,
};

BOOST_DESCRIBE_ENUM(path_policy, direct_only, direct_preferred)

struct policy_config {
   path_policy policy = path_policy::direct_only;
};

struct policy_list_config {
   std::vector<path_policy> policies;
};

struct dotted_config {
   std::uint32_t deadline_ms = 0;
   path_policy policy = path_policy::direct_only;
};

struct throwing_json_value {};

struct byte_payload {
   std::vector<std::uint8_t> value;
};

struct scalar_variant_config {
   std::optional<std::variant<bool, std::int64_t, std::uint64_t, double, std::string>> value;

   bool operator==(const scalar_variant_config&) const = default;
};

BOOST_DESCRIBE_STRUCT(byte_payload, (), (value))
BOOST_DESCRIBE_STRUCT(scalar_variant_config, (), (value))

} // namespace forge_json_tests

BOOST_DESCRIBE_STRUCT(forge_json_tests::http_config, (), (bind_port, bind_host, tls_enabled, tags))
BOOST_DESCRIBE_STRUCT(forge_json_tests::named_tag, (), (value))
BOOST_DESCRIBE_STRUCT(forge_json_tests::tag_config, (), (tags))
BOOST_DESCRIBE_STRUCT(forge_json_tests::policy_config, (), (policy))
BOOST_DESCRIBE_STRUCT(forge_json_tests::policy_list_config, (), (policies))
BOOST_DESCRIBE_STRUCT(forge_json_tests::dotted_config, (), (deadline_ms, policy))

import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.codec.json;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;
import forge.tests.codec.json.exact_types;

namespace forge_json_tests {

void to_variant(const throwing_json_value&, forge::variant&) {
   throw std::runtime_error{"test JSON conversion failure"};
}

void from_variant(const forge::variant& source, byte_payload& value) {
   forge::from_variant(source, value);
}

void to_variant(const byte_payload& value, forge::variant& output) {
   forge::to_variant(value, output);
}

} // namespace forge_json_tests

template <> struct forge::schema::rules<forge_json_tests::http_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::http_config> define() {
      auto schema = forge::schema::object<forge_json_tests::http_config>();
      schema.field<&forge_json_tests::http_config::bind_port>("bind-port")
          .alias("port")
          .required()
          .default_value(8080)
          .range(1, 65535);
      schema.field<&forge_json_tests::http_config::bind_host>("bind-host").default_value("127.0.0.1");
      schema.field<&forge_json_tests::http_config::tls_enabled>("tls-enabled").default_value(false);
      static_cast<void>(schema.field<&forge_json_tests::http_config::tags>("tags"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::named_tag> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::named_tag> define() {
      auto schema = forge::schema::object<forge_json_tests::named_tag>();
      static_cast<void>(schema.field<&forge_json_tests::named_tag::value>("value"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::tag_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::tag_config> define() {
      auto schema = forge::schema::object<forge_json_tests::tag_config>();
      static_cast<void>(schema.field<&forge_json_tests::tag_config::tags>("tags"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::policy_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::policy_config> define() {
      auto schema = forge::schema::object<forge_json_tests::policy_config>();
      static_cast<void>(schema.field<&forge_json_tests::policy_config::policy>("path-policy"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::policy_list_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::policy_list_config> define() {
      auto schema = forge::schema::object<forge_json_tests::policy_list_config>();
      static_cast<void>(schema.field<&forge_json_tests::policy_list_config::policies>("policies"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::dotted_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::dotted_config> define() {
      auto schema = forge::schema::object<forge_json_tests::dotted_config>();
      schema.field<&forge_json_tests::dotted_config::deadline_ms>("api.deadline-ms").alias("deadline-ms");
      static_cast<void>(schema.field<&forge_json_tests::dotted_config::policy>("path.policy"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::exact_alias_leaf> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::exact_alias_leaf> define() {
      auto schema = forge::schema::object<forge_json_tests::exact_alias_leaf>();
      schema.field<&forge_json_tests::exact_alias_leaf::bind_port>("bind-port").alias("port").default_value(8080);
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::exact_dotted_leaf> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::exact_dotted_leaf> define() {
      auto schema = forge::schema::object<forge_json_tests::exact_dotted_leaf>();
      static_cast<void>(schema.field<&forge_json_tests::exact_dotted_leaf::deadline_ms>("api.deadline-ms"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::exact_long_double_record> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::exact_long_double_record> define() {
      auto schema = forge::schema::object<forge_json_tests::exact_long_double_record>();
      static_cast<void>(schema.field<&forge_json_tests::exact_long_double_record::value>("value"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::exact_schema_plain_parent> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::exact_schema_plain_parent> define() {
      auto schema = forge::schema::object<forge_json_tests::exact_schema_plain_parent>();
      static_cast<void>(schema.field<&forge_json_tests::exact_schema_plain_parent::child>("child"));
      static_cast<void>(schema.field<&forge_json_tests::exact_schema_plain_parent::children>("children"));
      static_cast<void>(schema.field<&forge_json_tests::exact_schema_plain_parent::canonical>("canonical"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_json_tests::exact_dotted_schema_parent> {
   [[nodiscard]] static forge::schema::object_schema<forge_json_tests::exact_dotted_schema_parent> define() {
      auto schema = forge::schema::object<forge_json_tests::exact_dotted_schema_parent>();
      static_cast<void>(schema.field<&forge_json_tests::exact_dotted_schema_parent::config>("config"));
      return schema;
   }
};

BOOST_AUTO_TEST_SUITE(json_codec_tests)

BOOST_AUTO_TEST_CASE(json_schema_writer_rejects_long_double_without_narrowing) {
   const auto input = forge_json_tests::exact_long_double_record{.value = 1.0L};
   const auto written = forge::codec::json::write(input);

   BOOST_REQUIRE(!written.ok());
   BOOST_TEST(written.text.empty());
   BOOST_REQUIRE_EQUAL(written.diagnostics.size(), 1U);
   BOOST_TEST(written.diagnostics.front().path == "value");
   BOOST_TEST(written.diagnostics.front().code == "json.type");
   BOOST_TEST(written.diagnostics.front().message == "long double schema fields are not supported by config codecs");
   BOOST_CHECK_THROW(static_cast<void>(forge::config::core::encode(input)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(json_schema_writer_reports_nested_encoding_path) {
   const auto input = forge_json_tests::exact_long_double_parent{.nested = {.value = 1.0L}};
   const auto written = forge::codec::json::write(input);

   BOOST_REQUIRE(!written.ok());
   BOOST_TEST(written.text.empty());
   BOOST_REQUIRE_EQUAL(written.diagnostics.size(), 1U);
   BOOST_TEST(written.diagnostics.front().path == "nested.value");
   BOOST_TEST(written.diagnostics.front().code == "json.type");
   BOOST_TEST(written.diagnostics.front().message == "long double schema fields are not supported by config codecs");

   const auto saved = forge::codec::json::save({}, input);
   BOOST_REQUIRE(!saved.ok());
   BOOST_REQUIRE_EQUAL(saved.diagnostics.size(), 1U);
   BOOST_TEST(saved.diagnostics.front().path == "nested.value");
   BOOST_TEST(saved.diagnostics.front().code == "json.type");
   BOOST_TEST(saved.diagnostics.front().message == "long double schema fields are not supported by config codecs");
}

BOOST_AUTO_TEST_CASE(json_schema_roundtrip_preserves_described_children_without_rules) {
   const auto input = forge_json_tests::exact_schema_plain_parent{
       .child = {.value = 7},
       .children = {{.value = 11}, {.value = 13}},
       .canonical = {.digest =
                         forge::crypto::digest::sha256{
                             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}},
   };

   const auto encoded = forge::config::core::encode(input);
   const auto decoded = forge::config::core::decode<forge_json_tests::exact_schema_plain_parent>(encoded);
   BOOST_REQUIRE(decoded.ok());
   BOOST_CHECK(decoded.value == input);

   const auto written = forge::codec::json::write(input);
   BOOST_REQUIRE(written.ok());
   const auto parsed = forge::codec::json::read<forge_json_tests::exact_schema_plain_parent>(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_CHECK(parsed.value == input);

   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };
   const auto noncanonical = forge::codec::json::read<forge_json_tests::exact_schema_plain_parent>(
       R"({"child":{"value":7},"children":[],"canonical":{"digest":"00"}})", options);
   BOOST_REQUIRE(!noncanonical.ok());
   BOOST_REQUIRE_EQUAL(noncanonical.diagnostics.size(), 1U);
   BOOST_TEST(noncanonical.diagnostics.front().path == "canonical.digest");
   BOOST_TEST(noncanonical.diagnostics.front().code == "json.type");
}

BOOST_AUTO_TEST_CASE(json_value_roundtrip_preserves_generic_shapes) {
   const auto parsed =
       forge::codec::json::read_value(R"({"null":null,"flag":true,"i":-2,"u":7,"d":3.5,"s":"x","a":[1,"b"]})");
   BOOST_REQUIRE(parsed.ok());

   const auto& object = parsed.value.get_object();
   BOOST_TEST(object["flag"].as_bool());
   BOOST_TEST(object["i"].as_int64() == -2);
   BOOST_TEST(object["u"].as_uint64() == 7U);
   BOOST_TEST(object["d"].as_double() == 3.5);
   BOOST_TEST(object["s"].get_string() == "x");
   BOOST_REQUIRE_EQUAL(object["a"].get_array().size(), 2U);

   const auto written = forge::codec::json::write_value(parsed.value);
   BOOST_REQUIRE(written.ok());
   const auto reparsed = forge::codec::json::read_value(written.text);
   BOOST_REQUIRE(reparsed.ok());
   BOOST_TEST(reparsed.value.get_object()["flag"].as_bool());
   BOOST_TEST(reparsed.value.get_object()["i"].as_int64() == -2);
   BOOST_TEST(reparsed.value.get_object()["u"].as_uint64() == 7U);
   BOOST_REQUIRE_EQUAL(reparsed.value.get_object()["a"].get_array().size(), 2U);
}

BOOST_AUTO_TEST_CASE(json_typed_writers_return_diagnostics_for_conversion_failures) {
   const auto written = forge::codec::json::write(forge_json_tests::throwing_json_value{});
   BOOST_TEST(!written.ok());
   BOOST_REQUIRE_EQUAL(written.diagnostics.size(), 1U);
   BOOST_TEST(written.diagnostics.front().code == "json.type");
   BOOST_TEST(written.diagnostics.front().message == "test JSON conversion failure");

   const auto path = std::filesystem::temp_directory_path() / "forge-json-conversion-failure.json";
   std::error_code ignored;
   std::filesystem::remove(path, ignored);
   const auto saved = forge::codec::json::save(path, forge_json_tests::throwing_json_value{});
   BOOST_TEST(!saved.ok());
   BOOST_REQUIRE_EQUAL(saved.diagnostics.size(), 1U);
   BOOST_TEST(saved.diagnostics.front().code == "json.type");
   BOOST_TEST(!std::filesystem::exists(path));
}

BOOST_AUTO_TEST_CASE(json_large_uint64_is_not_silently_converted_to_double) {
   const auto parsed = forge::codec::json::read_value(R"({"max":18446744073709551615})");
   BOOST_REQUIRE(parsed.ok());
   BOOST_TEST(parsed.value.get_object()["max"].as_uint64() == 18446744073709551615ULL);

   const auto written = forge::codec::json::write_value(parsed.value);
   BOOST_REQUIRE(written.ok());
   BOOST_TEST(written.text.find("18446744073709551615") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(json_document_roundtrip_uses_config_document) {
   auto document = forge::config::core::document{};
   document.set("http.bind-host", "127.0.0.1");
   document.set("http.bind-port", 8080);
   document.set("http.tags", forge::config::core::value::array_type{forge::config::core::value{"alpha"},
                                                                    forge::config::core::value{"beta"}});

   const auto written = forge::codec::json::write_document(document, {.pretty = true});
   BOOST_REQUIRE(written.ok());
   const auto parsed = forge::codec::json::read_document(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_REQUIRE(parsed.value.try_get("http.bind-host") != nullptr);
   BOOST_REQUIRE(parsed.value.try_get("http.bind-port") != nullptr);
   BOOST_REQUIRE(parsed.value.try_get("http.tags") != nullptr);
}

BOOST_AUTO_TEST_CASE(json_typed_read_uses_schema_defaults_validation_and_unknown_policy) {
   const auto parsed = forge::codec::json::read<forge_json_tests::http_config>(
       R"({"bind-port":9090,"tls-enabled":false,"tags":["alpha"],"extra":1})");
   BOOST_REQUIRE(parsed.ok());
   BOOST_TEST(parsed.value.bind_port == 9090U);
   BOOST_TEST(parsed.value.bind_host == "127.0.0.1");
   BOOST_REQUIRE_EQUAL(parsed.value.tags.size(), 1U);
   BOOST_TEST(parsed.diagnostics.size() == 1U);
   BOOST_TEST(parsed.diagnostics.front().code == "json.unknown");

   auto options = forge::codec::json::read_options{};
   options.unknown_fields = forge::codec::json::unknown_field_policy::error;
   const auto rejected =
       forge::codec::json::read<forge_json_tests::http_config>(R"({"bind-port":9090,"extra":1})", options);
   BOOST_TEST(!rejected.ok());
   BOOST_TEST(rejected.diagnostics.front().code == "json.unknown");

   const auto invalid = forge::codec::json::read<forge_json_tests::http_config>(R"({"bind-port":0})");
   BOOST_TEST(!invalid.ok());
}

BOOST_AUTO_TEST_CASE(json_typed_load_uses_same_unknown_policy_as_read) {
   const auto path = std::filesystem::temp_directory_path() /
                     ("forge_json_unknown_policy_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
   {
      auto out = std::ofstream{path};
      out << R"({"bind-port":9090,"extra":1})";
   }
   struct cleanup {
      std::filesystem::path path;
      ~cleanup() {
         std::error_code ignored;
         std::filesystem::remove(path, ignored);
      }
   } remove_file{path};

   const auto warned = forge::codec::json::load<forge_json_tests::http_config>(path);
   BOOST_REQUIRE(warned.ok());
   BOOST_REQUIRE_EQUAL(warned.diagnostics.size(), 1U);
   BOOST_TEST(warned.diagnostics.front().code == "json.unknown");

   auto rejected_options = forge::codec::json::read_options{};
   rejected_options.unknown_fields = forge::codec::json::unknown_field_policy::error;
   const auto rejected = forge::codec::json::load<forge_json_tests::http_config>(path, rejected_options);
   BOOST_TEST(!rejected.ok());
   BOOST_REQUIRE_EQUAL(rejected.diagnostics.size(), 1U);
   BOOST_TEST(rejected.diagnostics.front().code == "json.unknown");

   auto ignored_options = forge::codec::json::read_options{};
   ignored_options.unknown_fields = forge::codec::json::unknown_field_policy::ignore;
   const auto ignored = forge::codec::json::load<forge_json_tests::http_config>(path, ignored_options);
   BOOST_REQUIRE(ignored.ok());
   BOOST_TEST(ignored.diagnostics.empty());
   BOOST_TEST(ignored.value.bind_port == 9090U);
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_validate_nested_fields_and_variants) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };
   const auto canonical = forge::codec::json::read<forge_json_tests::exact_record>(
       R"({"items":[{"value":1}],"choice":[0,{"value":2}]})", options);
   for (const auto& diagnostic : canonical.diagnostics) {
      BOOST_TEST_MESSAGE(diagnostic.code << " at " << diagnostic.path << ": " << diagnostic.message);
   }
   BOOST_REQUIRE(canonical.ok());
   BOOST_REQUIRE_EQUAL(canonical.value.items.size(), 1U);
   BOOST_TEST(canonical.value.items.front().value == 1U);
   BOOST_TEST(std::get<forge_json_tests::exact_leaf>(canonical.value.choice).value == 2U);
   BOOST_TEST(!canonical.value.optional.has_value());

   const auto written = forge::codec::json::write(canonical.value, {.pretty = true});
   BOOST_REQUIRE(written.ok());
   const auto written_roundtrip = forge::codec::json::read<forge_json_tests::exact_record>(written.text, options);
   BOOST_REQUIRE(written_roundtrip.ok());
   BOOST_CHECK(written_roundtrip.value == canonical.value);

   const auto path =
       std::filesystem::temp_directory_path() /
       ("forge_json_exact_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
   struct cleanup {
      std::filesystem::path path;
      ~cleanup() {
         auto ignored = std::error_code{};
         std::filesystem::remove(path, ignored);
      }
   } remove_file{path};
   const auto saved = forge::codec::json::save(path, canonical.value, {.pretty = true});
   BOOST_REQUIRE(saved.ok());
   const auto loaded = forge::codec::json::load<forge_json_tests::exact_record>(path, options);
   BOOST_REQUIRE(loaded.ok());
   BOOST_CHECK(loaded.value == canonical.value);

   const auto unknown = forge::codec::json::read<forge_json_tests::exact_record>(
       R"({"items":[{"value":1,"extra":2}],"choice":[0,{"value":2}]})", options);
   BOOST_REQUIRE(!unknown.ok());
   BOOST_TEST(unknown.diagnostics.front().code == "json.unknown");
   BOOST_TEST(unknown.diagnostics.front().path == "items[0].extra");

   const auto missing =
       forge::codec::json::read<forge_json_tests::exact_record>(R"({"items":[{}],"choice":[0,{"value":2}]})", options);
   BOOST_REQUIRE(!missing.ok());
   BOOST_TEST(missing.diagnostics.front().code == "json.missing");
   BOOST_TEST(missing.diagnostics.front().path == "items[0].value");

   const auto invalid_array =
       forge::codec::json::read<forge_json_tests::exact_record>(R"({"items":{},"choice":[0,{"value":2}]})", options);
   BOOST_REQUIRE(!invalid_array.ok());
   BOOST_TEST(invalid_array.diagnostics.front().code == "json.array");
   BOOST_TEST(invalid_array.diagnostics.front().path == "items");

   const auto object_variant =
       forge::codec::json::read<forge_json_tests::exact_record>(R"({"items":[],"choice":{"value":2}})", options);
   BOOST_REQUIRE(!object_variant.ok());
   BOOST_TEST(object_variant.diagnostics.front().code == "json.variant");
   BOOST_TEST(object_variant.diagnostics.front().path == "choice");

   const auto string_variant =
       forge::codec::json::read<forge_json_tests::exact_record>(R"({"items":[],"choice":"bad"})", options);
   BOOST_REQUIRE(!string_variant.ok());
   BOOST_TEST(string_variant.diagnostics.front().code == "json.variant");
   BOOST_TEST(string_variant.diagnostics.front().path == "choice");

   const auto invalid_index =
       forge::codec::json::read<forge_json_tests::exact_record>(R"({"items":[],"choice":[2,{"value":2}]})", options);
   BOOST_REQUIRE(!invalid_index.ok());
   BOOST_TEST(invalid_index.diagnostics.front().code == "json.variant");
   BOOST_TEST(invalid_index.diagnostics.front().path == "choice[0]");

   const auto negative_index =
       forge::codec::json::read<forge_json_tests::exact_record>(R"({"items":[],"choice":[-1,{"value":2}]})", options);
   BOOST_REQUIRE(!negative_index.ok());
   BOOST_TEST(negative_index.diagnostics.front().code == "json.variant");
   BOOST_TEST(negative_index.diagnostics.front().path == "choice[0]");

   const auto false_index = forge::codec::json::read<forge_json_tests::exact_record>(
       R"({"items":[],"choice":[false,{"value":2}]})", options);
   BOOST_REQUIRE(!false_index.ok());
   BOOST_TEST(false_index.diagnostics.front().code == "json.variant");
   BOOST_TEST(false_index.diagnostics.front().path == "choice[0]");

   const auto true_index =
       forge::codec::json::read<forge_json_tests::exact_record>(R"({"items":[],"choice":[true,{"value":2}]})", options);
   BOOST_REQUIRE(!true_index.ok());
   BOOST_TEST(true_index.diagnostics.front().code == "json.variant");
   BOOST_TEST(true_index.diagnostics.front().path == "choice[0]");

   const auto extra_variant_element = forge::codec::json::read<forge_json_tests::exact_record>(
       R"({"items":[],"choice":[0,{"value":2},false]})", options);
   BOOST_REQUIRE(!extra_variant_element.ok());
   BOOST_TEST(extra_variant_element.diagnostics.front().code == "json.variant");
   BOOST_TEST(extra_variant_element.diagnostics.front().path == "choice");

   const auto pointers = forge::codec::json::read<forge_json_tests::exact_pointer_record>(
       R"({"shared":{"bind-port":3},"unique":{"port":4}})", options);
   BOOST_REQUIRE(pointers.ok());
   BOOST_REQUIRE(pointers.value.shared);
   BOOST_REQUIRE(pointers.value.unique);
   BOOST_TEST(pointers.value.shared->bind_port == 3U);
   BOOST_TEST(pointers.value.unique->bind_port == 4U);

   const auto null_pointers =
       forge::codec::json::read<forge_json_tests::exact_pointer_record>(R"({"shared":null,"unique":null})", options);
   BOOST_REQUIRE(null_pointers.ok());
   BOOST_TEST(!null_pointers.value.shared);
   BOOST_TEST(!null_pointers.value.unique);

   const auto shared_missing = forge::codec::json::read<forge_json_tests::exact_pointer_record>(
       R"({"shared":{},"unique":{"bind-port":4}})", options);
   BOOST_REQUIRE(!shared_missing.ok());
   BOOST_TEST(shared_missing.diagnostics.front().code == "json.missing");
   BOOST_TEST(shared_missing.diagnostics.front().path == "shared.bind-port");

   const auto unique_unknown = forge::codec::json::read<forge_json_tests::exact_pointer_record>(
       R"({"shared":{"bind-port":3},"unique":{"bind-port":4,"extra":5}})", options);
   BOOST_REQUIRE(!unique_unknown.ok());
   BOOST_TEST(unique_unknown.diagnostics.front().code == "json.unknown");
   BOOST_TEST(unique_unknown.diagnostics.front().path == "unique.extra");

   const auto duplicate_alias = forge::codec::json::read<forge_json_tests::exact_pointer_record>(
       R"({"shared":{"bind-port":3,"port":4},"unique":{"bind-port":5}})", options);
   BOOST_REQUIRE(!duplicate_alias.ok());
   BOOST_TEST(duplicate_alias.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(duplicate_alias.diagnostics.front().path == "shared.port");

   const auto schema_set = forge::codec::json::read<forge_json_tests::exact_schema_set_record>(
       R"({"values":[{"bind-port":1},{"port":2}]})", options);
   BOOST_REQUIRE(schema_set.ok());
   BOOST_REQUIRE_EQUAL(schema_set.value.values.size(), 2U);
   auto set_entry = schema_set.value.values.begin();
   BOOST_TEST(set_entry->bind_port == 1U);
   ++set_entry;
   BOOST_TEST(set_entry->bind_port == 2U);

   const auto schema_set_duplicate = forge::codec::json::read<forge_json_tests::exact_schema_set_record>(
       R"({"values":[{"bind-port":1},{"port":1}]})", options);
   BOOST_REQUIRE(!schema_set_duplicate.ok());
   BOOST_TEST(schema_set_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(schema_set_duplicate.diagnostics.front().path == "values[1]");
}

BOOST_AUTO_TEST_CASE(json_exact_scalar_variant_and_optional_described_record_roundtrip) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };
   const std::vector<std::string> values{
       "[0,false]", "[0,true]", "[1,-9223372036854775808]", "[2,18446744073709551615]", "[3,1.25]", R"([4,"symbol"])"};
   for (const auto& json : values) {
      const auto scalar =
          forge::codec::json::read<std::variant<bool, std::int64_t, std::uint64_t, double, std::string>>(json, options);
      BOOST_REQUIRE(scalar.ok());
      const auto encoded_scalar = forge::codec::json::write(scalar.value);
      BOOST_REQUIRE(encoded_scalar.ok());
      BOOST_TEST(encoded_scalar.text == json);

      const auto object = forge::codec::json::read<forge_json_tests::scalar_variant_config>(
          std::string{"{\"value\":"} + json + '}', options);
      BOOST_REQUIRE(object.ok());
      BOOST_REQUIRE(object.value.value.has_value());
      BOOST_CHECK(*object.value.value == scalar.value);
      const auto encoded = forge::codec::json::write(object.value);
      BOOST_REQUIRE(encoded.ok());
      const auto restored = forge::codec::json::read<forge_json_tests::scalar_variant_config>(encoded.text, options);
      BOOST_REQUIRE(restored.ok());
      BOOST_CHECK(restored.value == object.value);
   }
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_validate_schema_names_and_associative_entries) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto schema_record = forge::codec::json::read<forge_json_tests::http_config>(
       R"({"bind-port":9090,"bind-host":"127.0.0.1","tls-enabled":false,"tags":[]})", options);
   BOOST_REQUIRE(schema_record.ok());

   const auto written_schema_record = forge::codec::json::write(schema_record.value);
   BOOST_REQUIRE(written_schema_record.ok());
   BOOST_TEST(written_schema_record.text.find(R"("bind-port")") != std::string::npos);
   BOOST_TEST(written_schema_record.text.find(R"("bind_port")") == std::string::npos);
   const auto written_schema_roundtrip =
       forge::codec::json::read<forge_json_tests::http_config>(written_schema_record.text, options);
   BOOST_REQUIRE(written_schema_roundtrip.ok());
   BOOST_TEST(written_schema_roundtrip.value.bind_port == schema_record.value.bind_port);
   BOOST_TEST(written_schema_roundtrip.value.bind_host == schema_record.value.bind_host);
   BOOST_TEST(written_schema_roundtrip.value.tls_enabled == schema_record.value.tls_enabled);
   BOOST_TEST(written_schema_roundtrip.value.tags == schema_record.value.tags);

   const auto schema_path = std::filesystem::temp_directory_path() /
                            ("forge_json_exact_schema_" +
                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
   struct schema_cleanup {
      std::filesystem::path path;
      ~schema_cleanup() {
         auto ignored = std::error_code{};
         std::filesystem::remove(path, ignored);
      }
   } remove_schema_file{schema_path};
   const auto saved_schema_record = forge::codec::json::save(schema_path, schema_record.value);
   BOOST_REQUIRE(saved_schema_record.ok());
   const auto saved_schema_roundtrip = forge::codec::json::load<forge_json_tests::http_config>(schema_path, options);
   BOOST_REQUIRE(saved_schema_roundtrip.ok());
   BOOST_TEST(saved_schema_roundtrip.value.bind_port == schema_record.value.bind_port);
   BOOST_TEST(saved_schema_roundtrip.value.bind_host == schema_record.value.bind_host);
   BOOST_TEST(saved_schema_roundtrip.value.tls_enabled == schema_record.value.tls_enabled);
   BOOST_TEST(saved_schema_roundtrip.value.tags == schema_record.value.tags);

   const auto schema_missing = forge::codec::json::read<forge_json_tests::http_config>(
       R"({"bind-port":9090,"bind-host":"127.0.0.1","tls-enabled":false})", options);
   BOOST_REQUIRE(!schema_missing.ok());
   BOOST_TEST(schema_missing.diagnostics.front().code == "json.missing");
   BOOST_TEST(schema_missing.diagnostics.front().path == "tags");

   const auto schema_unknown = forge::codec::json::read<forge_json_tests::http_config>(
       R"({"bind-port":9090,"bind-host":"127.0.0.1","tls-enabled":false,"tags":[],"extra":1})", options);
   BOOST_REQUIRE(!schema_unknown.ok());
   BOOST_TEST(schema_unknown.diagnostics.front().code == "json.unknown");
   BOOST_TEST(schema_unknown.diagnostics.front().path == "extra");

   const auto schema_duplicate = forge::codec::json::read<forge_json_tests::http_config>(
       R"({"bind-port":9090,"port":9091,"bind-host":"127.0.0.1","tls-enabled":false,"tags":[]})", options);
   BOOST_REQUIRE(!schema_duplicate.ok());
   BOOST_TEST(schema_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(schema_duplicate.diagnostics.front().path == "port");

   const auto duplicate_member = forge::codec::json::read<forge_json_tests::http_config>(
       R"({"bind-port":9090,"bind-port":9091,"bind-host":"127.0.0.1","tls-enabled":false,"tags":[]})", options);
   BOOST_REQUIRE(!duplicate_member.ok());
   BOOST_TEST(duplicate_member.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(duplicate_member.diagnostics.front().path == "bind-port");

   const auto textual_scalars = forge::codec::json::read<forge_json_tests::http_config>(
       R"({"bind-port":"9090","bind-host":"127.0.0.1","tls-enabled":"false","tags":[]})", options);
   BOOST_REQUIRE(!textual_scalars.ok());
   BOOST_TEST(textual_scalars.diagnostics.front().code == "json.type");
   BOOST_TEST(textual_scalars.diagnostics.front().path == "bind-port");

   const auto escaped_duplicate =
       forge::codec::json::read<forge_json_tests::exact_leaf>(R"({"value":7,"\u0076alue":8})", options);
   BOOST_REQUIRE(!escaped_duplicate.ok());
   BOOST_TEST(escaped_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(escaped_duplicate.diagnostics.front().path == "value");

   const auto permissive_duplicate = forge::codec::json::read_value(R"({"value":7,"value":8})");
   BOOST_REQUIRE(permissive_duplicate.ok());

   const auto map_record =
       forge::codec::json::read<forge_json_tests::exact_map_record>(R"({"values":[["first",{"value":7}]]})", options);
   BOOST_REQUIRE(map_record.ok());
   BOOST_TEST(map_record.value.values.at("first").value == 7U);

   const auto map_missing_value =
       forge::codec::json::read<forge_json_tests::exact_map_record>(R"({"values":[["first"]]})", options);
   BOOST_REQUIRE(!map_missing_value.ok());
   BOOST_TEST(map_missing_value.diagnostics.front().code == "json.pair");
   BOOST_TEST(map_missing_value.diagnostics.front().path == "values[0]");

   const auto map_unknown = forge::codec::json::read<forge_json_tests::exact_map_record>(
       R"({"values":[["first",{"value":7,"extra":1}]]})", options);
   BOOST_REQUIRE(!map_unknown.ok());
   BOOST_TEST(map_unknown.diagnostics.front().code == "json.unknown");
   BOOST_TEST(map_unknown.diagnostics.front().path == "values[0][1].extra");

   const auto nested_duplicate = forge::codec::json::read<forge_json_tests::exact_map_record>(
       R"({"values":[["first",{"value":7,"value":8}]]})", options);
   BOOST_REQUIRE(!nested_duplicate.ok());
   BOOST_TEST(nested_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(nested_duplicate.diagnostics.front().path == "values[0][1].value");

   const auto map_duplicate = forge::codec::json::read<forge_json_tests::exact_map_record>(
       R"({"values":[["first",{"value":7}],["first",{"value":8}]]})", options);
   BOOST_REQUIRE(!map_duplicate.ok());
   BOOST_TEST(map_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(map_duplicate.diagnostics.front().path == "values[1][0]");

   const auto set_duplicate = forge::codec::json::read<forge_json_tests::exact_set_record>(
       R"({"ordered":["first","first"],"unordered":[]})", options);
   BOOST_REQUIRE(!set_duplicate.ok());
   BOOST_TEST(set_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(set_duplicate.diagnostics.front().path == "ordered[1]");

   const auto unordered_set_duplicate = forge::codec::json::read<forge_json_tests::exact_set_record>(
       R"({"ordered":[],"unordered":["first","first"]})", options);
   BOOST_REQUIRE(!unordered_set_duplicate.ok());
   BOOST_TEST(unordered_set_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(unordered_set_duplicate.diagnostics.front().path == "unordered[1]");

   const auto multi_index_record =
       forge::codec::json::read<forge_json_tests::exact_multi_index_record>(R"({"values":[{"value":7}]})", options);
   BOOST_REQUIRE(multi_index_record.ok());
   BOOST_REQUIRE_EQUAL(multi_index_record.value.values.size(), 1U);

   const auto multi_index_unknown = forge::codec::json::read<forge_json_tests::exact_multi_index_record>(
       R"({"values":[{"value":7,"extra":1}]})", options);
   BOOST_REQUIRE(!multi_index_unknown.ok());
   BOOST_TEST(multi_index_unknown.diagnostics.front().code == "json.unknown");
   BOOST_TEST(multi_index_unknown.diagnostics.front().path == "values[0].extra");

   const auto multi_index_duplicate = forge::codec::json::read<forge_json_tests::exact_multi_index_record>(
       R"({"values":[{"value":7},{"value":7}]})", options);
   BOOST_REQUIRE(!multi_index_duplicate.ok());
   BOOST_TEST(multi_index_duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(multi_index_duplicate.diagnostics.front().path == "values[1]");

   const auto shorthand = forge::codec::json::read<forge_json_tests::tag_config>(R"({"tags":["alpha"]})", options);
   BOOST_REQUIRE(shorthand.ok());
   BOOST_REQUIRE_EQUAL(shorthand.value.tags.size(), 1U);
   BOOST_TEST(shorthand.value.tags.front().value == "alpha");
}

BOOST_AUTO_TEST_CASE(json_exact_schema_paths_roundtrip_through_typed_writers) {
   const auto input = forge_json_tests::dotted_config{
       .deadline_ms = 2500,
       .policy = forge_json_tests::path_policy::direct_preferred,
   };
   const auto written = forge::codec::json::write(input);
   BOOST_REQUIRE(written.ok());
   BOOST_TEST(written.text.find(R"("api":{"deadline-ms":2500})") != std::string::npos);
   BOOST_TEST(written.text.find(R"("path":{"policy":"direct-preferred"})") != std::string::npos);

   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };
   const auto roundtrip = forge::codec::json::read<forge_json_tests::dotted_config>(written.text, options);
   BOOST_REQUIRE(roundtrip.ok());
   BOOST_TEST(roundtrip.diagnostics.empty());
   BOOST_TEST(roundtrip.value.deadline_ms == input.deadline_ms);
   BOOST_TEST(static_cast<int>(roundtrip.value.policy) == static_cast<int>(input.policy));

   const auto strict_unknown_options = forge::codec::json::read_options{
       .unknown_fields = forge::codec::json::unknown_field_policy::error,
   };
   const auto permissive_roundtrip =
       forge::codec::json::read<forge_json_tests::dotted_config>(written.text, strict_unknown_options);
   BOOST_REQUIRE(permissive_roundtrip.ok());
   BOOST_TEST(permissive_roundtrip.diagnostics.empty());

   const auto nested = forge::codec::json::read<forge_json_tests::exact_dotted_parent>(
       R"({"config":{"api":{"deadline-ms":2500}}})", options);
   BOOST_REQUIRE(nested.ok());
   BOOST_TEST(nested.diagnostics.empty());
   BOOST_TEST(nested.value.config.deadline_ms == 2500U);

   const auto nested_written =
       forge::codec::json::write(forge_json_tests::exact_dotted_parent{.config = {.deadline_ms = 2500}});
   BOOST_REQUIRE(nested_written.ok());
   BOOST_TEST(nested_written.text.find(R"("config":{"api":{"deadline-ms":2500}})") != std::string::npos);
   BOOST_TEST(nested_written.text.find("deadline_ms") == std::string::npos);
   const auto nested_roundtrip =
       forge::codec::json::read<forge_json_tests::exact_dotted_parent>(nested_written.text, options);
   BOOST_REQUIRE(nested_roundtrip.ok());
   BOOST_TEST(nested_roundtrip.diagnostics.empty());
   BOOST_TEST(nested_roundtrip.value.config.deadline_ms == 2500U);

   const auto nested_permissive_roundtrip =
       forge::codec::json::read<forge_json_tests::exact_dotted_parent>(nested_written.text);
   BOOST_REQUIRE(nested_permissive_roundtrip.ok());
   BOOST_TEST(nested_permissive_roundtrip.diagnostics.empty());
   BOOST_TEST(nested_permissive_roundtrip.value.config.deadline_ms == 2500U);

   const auto nested_path = std::filesystem::temp_directory_path() /
                            ("forge_json_nested_schema_" +
                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
   struct nested_cleanup {
      std::filesystem::path path;
      ~nested_cleanup() {
         auto ignored = std::error_code{};
         std::filesystem::remove(path, ignored);
      }
   } remove_nested_file{nested_path};
   const auto nested_saved =
       forge::codec::json::save(nested_path, forge_json_tests::exact_dotted_parent{.config = {.deadline_ms = 2500}});
   BOOST_REQUIRE(nested_saved.ok());
   const auto nested_loaded = forge::codec::json::load<forge_json_tests::exact_dotted_parent>(nested_path);
   BOOST_REQUIRE(nested_loaded.ok());
   BOOST_TEST(nested_loaded.diagnostics.empty());
   BOOST_TEST(nested_loaded.value.config.deadline_ms == 2500U);

   const auto nested_unknown = forge::codec::json::read<forge_json_tests::exact_dotted_parent>(
       R"({"config":{"api":{"deadline-ms":2500,"extra":1}}})");
   BOOST_REQUIRE(nested_unknown.ok());
   BOOST_REQUIRE_EQUAL(nested_unknown.diagnostics.size(), 1U);
   BOOST_TEST(nested_unknown.diagnostics.front().code == "json.unknown");
   BOOST_TEST(nested_unknown.diagnostics.front().path == "config.api.extra");
   BOOST_TEST(nested_unknown.value.config.deadline_ms == 2500U);

   const auto nested_unknown_ignored = forge::codec::json::read<forge_json_tests::exact_dotted_parent>(
       R"({"config":{"api":{"deadline-ms":2500,"extra":1}}})",
       {.unknown_fields = forge::codec::json::unknown_field_policy::ignore});
   BOOST_REQUIRE(nested_unknown_ignored.ok());
   BOOST_TEST(nested_unknown_ignored.diagnostics.empty());
   BOOST_TEST(nested_unknown_ignored.value.config.deadline_ms == 2500U);

   const auto nested_unknown_rejected = forge::codec::json::read<forge_json_tests::exact_dotted_parent>(
       R"({"config":{"api":{"deadline-ms":2500,"extra":1}}})",
       {.unknown_fields = forge::codec::json::unknown_field_policy::error});
   BOOST_REQUIRE(!nested_unknown_rejected.ok());
   BOOST_REQUIRE_EQUAL(nested_unknown_rejected.diagnostics.size(), 1U);
   BOOST_TEST(nested_unknown_rejected.diagnostics.front().code == "json.unknown");
   BOOST_TEST(nested_unknown_rejected.diagnostics.front().path == "config.api.extra");

   const auto partial_pair = forge::codec::json::read<forge_json_tests::exact_dotted_pair_parent>(
       R"({"value":[{"api":{"deadline-ms":2500}}]})");
   BOOST_REQUIRE(partial_pair.ok());
   BOOST_TEST(partial_pair.diagnostics.empty());
   BOOST_TEST(partial_pair.value.value.first.deadline_ms == 2500U);
   BOOST_TEST(partial_pair.value.value.second == 0U);

   const auto partial_map_entry = forge::codec::json::read<forge_json_tests::exact_dotted_map_key_parent>(
       R"({"values":[[{"api":{"deadline-ms":2500}}]]})");
   BOOST_REQUIRE(partial_map_entry.ok());
   BOOST_TEST(partial_map_entry.diagnostics.empty());
   BOOST_REQUIRE_EQUAL(partial_map_entry.value.values.size(), 1U);
   BOOST_TEST(partial_map_entry.value.values.begin()->first.deadline_ms == 2500U);
   BOOST_TEST(partial_map_entry.value.values.begin()->second == 0U);

   const auto permissive_variant = forge::codec::json::read<forge_json_tests::exact_dotted_variant_parent>(
       R"({"value":[0,{"api":{"deadline-ms":2500}},"ignored"]})");
   BOOST_REQUIRE(permissive_variant.ok());
   BOOST_TEST(permissive_variant.diagnostics.empty());
   BOOST_TEST(std::get<forge_json_tests::exact_dotted_leaf>(permissive_variant.value.value).deadline_ms == 2500U);

   const auto oversized_variant = forge::codec::json::read<forge_json_tests::exact_dotted_variant_parent>(
       R"({"value":[4294967296,{"api":{"deadline-ms":2500}}]})");
   BOOST_REQUIRE(!oversized_variant.ok());
   BOOST_REQUIRE_EQUAL(oversized_variant.diagnostics.size(), 1U);
   BOOST_TEST(oversized_variant.diagnostics.front().code == "json.type");

   const auto schema_parent_input = forge_json_tests::exact_dotted_schema_parent{.config = {.deadline_ms = 2500}};
   const auto encoded_parent = forge::config::core::encode(schema_parent_input);
   const auto decoded_parent =
       forge::config::core::decode<forge_json_tests::exact_dotted_schema_parent>(encoded_parent);
   BOOST_REQUIRE(decoded_parent.ok());
   BOOST_TEST(decoded_parent.diagnostics.entries.empty());
   BOOST_TEST(decoded_parent.value.config.deadline_ms == 2500U);

   const auto alias = forge::codec::json::read<forge_json_tests::dotted_config>(
       R"({"deadline-ms":2500,"path":{"policy":"direct-preferred"}})", options);
   BOOST_REQUIRE(alias.ok());

   const auto duplicate = forge::codec::json::read<forge_json_tests::dotted_config>(
       R"({"api":{"deadline-ms":2500},"deadline-ms":3000,"path":{"policy":"direct-preferred"}})", options);
   BOOST_REQUIRE(!duplicate.ok());
   BOOST_TEST(duplicate.diagnostics.front().code == "json.duplicate");
   BOOST_TEST(duplicate.diagnostics.front().path == "deadline-ms");

   const auto unknown = forge::codec::json::read<forge_json_tests::dotted_config>(
       R"({"api":{"deadline-ms":2500,"extra":1},"path":{"policy":"direct-preferred"}})", options);
   BOOST_REQUIRE(!unknown.ok());
   BOOST_TEST(unknown.diagnostics.front().code == "json.unknown");
   BOOST_TEST(unknown.diagnostics.front().path == "api.extra");

   const auto missing = forge::codec::json::read<forge_json_tests::dotted_config>(
       R"({"api":{},"path":{"policy":"direct-preferred"}})", options);
   BOOST_REQUIRE(!missing.ok());
   BOOST_TEST(missing.diagnostics.front().code == "json.missing");
   BOOST_TEST(missing.diagnostics.front().path == "api.deadline-ms");

   const auto blocked = forge::codec::json::read<forge_json_tests::dotted_config>(
       R"({"api":2500,"path":{"policy":"direct-preferred"}})", options);
   BOOST_REQUIRE(!blocked.ok());
   BOOST_TEST(blocked.diagnostics.front().code == "json.type");
   BOOST_TEST(blocked.diagnostics.front().path == "api");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_validate_scalar_kinds_and_ranges) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };
   const auto canonical = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":true,"signed_value":-8,"unsigned_value":8,"ratio":1.5,"label":"ready"})", options);
   BOOST_REQUIRE(canonical.ok());
   BOOST_TEST(canonical.value.enabled);
   BOOST_TEST(canonical.value.signed_value == -8);
   BOOST_TEST(canonical.value.unsigned_value == 8U);
   BOOST_TEST(canonical.value.ratio == 1.5F);
   BOOST_TEST(canonical.value.label == "ready");

   const auto boolean_integer = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":true,"signed_value":false,"unsigned_value":8,"ratio":1.5,"label":"ready"})", options);
   BOOST_REQUIRE(!boolean_integer.ok());
   BOOST_TEST(boolean_integer.diagnostics.front().code == "json.type");
   BOOST_TEST(boolean_integer.diagnostics.front().path == "signed_value");

   const auto signed_overflow = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":true,"signed_value":-129,"unsigned_value":8,"ratio":1.5,"label":"ready"})", options);
   BOOST_REQUIRE(!signed_overflow.ok());
   BOOST_TEST(signed_overflow.diagnostics.front().code == "json.range");
   BOOST_TEST(signed_overflow.diagnostics.front().path == "signed_value");

   const auto unsigned_overflow = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":true,"signed_value":-8,"unsigned_value":256,"ratio":1.5,"label":"ready"})", options);
   BOOST_REQUIRE(!unsigned_overflow.ok());
   BOOST_TEST(unsigned_overflow.diagnostics.front().code == "json.range");
   BOOST_TEST(unsigned_overflow.diagnostics.front().path == "unsigned_value");

   const auto string_boolean = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":"true","signed_value":-8,"unsigned_value":8,"ratio":1.5,"label":"ready"})", options);
   BOOST_REQUIRE(!string_boolean.ok());
   BOOST_TEST(string_boolean.diagnostics.front().code == "json.type");
   BOOST_TEST(string_boolean.diagnostics.front().path == "enabled");

   const auto lossy_float_integer = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":true,"signed_value":-8,"unsigned_value":8,"ratio":16777217,"label":"ready"})", options);
   BOOST_REQUIRE(!lossy_float_integer.ok());
   BOOST_TEST(lossy_float_integer.diagnostics.front().code == "json.range");
   BOOST_TEST(lossy_float_integer.diagnostics.front().path == "ratio");

   const auto lossy_float_fraction = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":true,"signed_value":-8,"unsigned_value":8,"ratio":1.00000001,"label":"ready"})", options);
   BOOST_REQUIRE(!lossy_float_fraction.ok());
   BOOST_TEST(lossy_float_fraction.diagnostics.front().code == "json.range");
   BOOST_TEST(lossy_float_fraction.diagnostics.front().path == "ratio");

   const auto floating_underflow = forge::codec::json::read<forge_json_tests::exact_scalar_record>(
       R"({"enabled":true,"signed_value":-8,"unsigned_value":8,"ratio":1e-100,"label":"ready"})", options);
   BOOST_REQUIRE(!floating_underflow.ok());
   BOOST_TEST(floating_underflow.diagnostics.front().code == "json.range");
   BOOST_TEST(floating_underflow.diagnostics.front().path == "ratio");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_reject_integer_precision_loss_before_conversion) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto exact =
       forge::codec::json::read<forge_json_tests::exact_double_record>(R"({"value":9007199254740992})", options);
   BOOST_REQUIRE(exact.ok());
   BOOST_TEST(exact.value.value == 9007199254740992.0);

   const auto lossy =
       forge::codec::json::read<forge_json_tests::exact_double_record>(R"({"value":9007199254740993})", options);
   BOOST_REQUIRE(!lossy.ok());
   BOOST_TEST(lossy.diagnostics.front().code == "json.range");
   BOOST_TEST(lossy.diagnostics.front().path == "value");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_preserve_wide_integer_strings) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   constexpr auto unsigned_value = static_cast<unsigned __int128>(1) << 100;
   constexpr auto signed_value = -static_cast<__int128>(unsigned_value);
   const auto decoded = forge::codec::json::read<forge_json_tests::exact_wide_integer_record>(
       R"({"signed_value":"-1267650600228229401496703205376","unsigned_value":"1267650600228229401496703205376"})",
       options);

   BOOST_REQUIRE(decoded.ok());
   BOOST_CHECK(decoded.value.signed_value == signed_value);
   BOOST_CHECK(decoded.value.unsigned_value == unsigned_value);

   const auto overflow = forge::codec::json::read<forge_json_tests::exact_wide_integer_record>(
       R"({"signed_value":"0","unsigned_value":"340282366920938463463374607431768211456"})", options);
   BOOST_REQUIRE(!overflow.ok());
   BOOST_TEST(overflow.diagnostics.front().code == "json.range");
   BOOST_TEST(overflow.diagnostics.front().path == "unsigned_value");

   const auto leading_zero = forge::codec::json::read<forge_json_tests::exact_wide_integer_record>(
       R"({"signed_value":"0001","unsigned_value":"1"})", options);
   BOOST_REQUIRE(!leading_zero.ok());
   BOOST_TEST(leading_zero.diagnostics.front().code == "json.type");
   BOOST_TEST(leading_zero.diagnostics.front().path == "signed_value");

   const auto negative_zero = forge::codec::json::read<forge_json_tests::exact_wide_integer_record>(
       R"({"signed_value":"-0","unsigned_value":"1"})", options);
   BOOST_REQUIRE(!negative_zero.ok());
   BOOST_TEST(negative_zero.diagnostics.front().code == "json.type");
   BOOST_TEST(negative_zero.diagnostics.front().path == "signed_value");

   const auto numeric_signed = forge::codec::json::read<forge_json_tests::exact_wide_integer_record>(
       R"({"signed_value":1,"unsigned_value":"1"})", options);
   BOOST_REQUIRE(!numeric_signed.ok());
   BOOST_TEST(numeric_signed.diagnostics.front().code == "json.type");
   BOOST_TEST(numeric_signed.diagnostics.front().path == "signed_value");

   const auto numeric_unsigned = forge::codec::json::read<forge_json_tests::exact_wide_integer_record>(
       R"({"signed_value":"1","unsigned_value":1})", options);
   BOOST_REQUIRE(!numeric_unsigned.ok());
   BOOST_TEST(numeric_unsigned.diagnostics.front().code == "json.type");
   BOOST_TEST(numeric_unsigned.diagnostics.front().path == "unsigned_value");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_validate_numeric_scalar_adapters) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto canonical = forge::codec::json::read<forge_json_tests::exact_varint_record>(
       R"({"signed_value":1,"unsigned_value":1})", options);
   BOOST_REQUIRE(canonical.ok());
   BOOST_TEST(canonical.value.signed_value.value == 1);
   BOOST_TEST(canonical.value.unsigned_value.value == 1U);

   const auto boolean = forge::codec::json::read<forge_json_tests::exact_varint_record>(
       R"({"signed_value":true,"unsigned_value":1})", options);
   BOOST_REQUIRE(!boolean.ok());
   BOOST_TEST(boolean.diagnostics.front().code == "json.type");
   BOOST_TEST(boolean.diagnostics.front().path == "signed_value");

   const auto overflow = forge::codec::json::read<forge_json_tests::exact_varint_record>(
       R"({"signed_value":1,"unsigned_value":4294967296})", options);
   BOOST_REQUIRE(!overflow.ok());
   BOOST_TEST(overflow.diagnostics.front().code == "json.type");
   BOOST_TEST(overflow.diagnostics.front().path == "unsigned_value");
}

BOOST_AUTO_TEST_CASE(json_exact_described_numeric_scalars_use_canonical_variant_representation) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto canonical = forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("1.5", options);
   BOOST_REQUIRE(canonical.ok());
   BOOST_TEST(canonical.value.value == 1.5);

   const auto written = forge::codec::json::write(canonical.value);
   BOOST_REQUIRE(written.ok());
   BOOST_TEST(written.text == "1.5");

   const auto integer = forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("1", options);
   BOOST_REQUIRE(integer.ok());
   BOOST_TEST(integer.value.value == 1.0);

   const auto negative_integer = forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("-1", options);
   BOOST_REQUIRE(negative_integer.ok());
   BOOST_TEST(negative_integer.value.value == -1.0);

   const auto written_integer = forge::codec::json::write(integer.value);
   BOOST_REQUIRE(written_integer.ok());
   BOOST_TEST(written_integer.text == "1");

   const auto exact_large_integer =
       forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("9007199254740992", options);
   BOOST_REQUIRE(exact_large_integer.ok());
   BOOST_TEST(exact_large_integer.value.value == 9007199254740992.0);

   const auto rounded_large_integer =
       forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("9007199254740993", options);
   BOOST_REQUIRE(!rounded_large_integer.ok());
   BOOST_TEST(rounded_large_integer.diagnostics.front().code == "json.type");
   BOOST_TEST(rounded_large_integer.diagnostics.front().path == "$");

   const auto rounded_negative_integer =
       forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("-9007199254740993", options);
   BOOST_REQUIRE(!rounded_negative_integer.ok());
   BOOST_TEST(rounded_negative_integer.diagnostics.front().code == "json.type");
   BOOST_TEST(rounded_negative_integer.diagnostics.front().path == "$");

   const auto int64_min =
       forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("-9223372036854775808", options);
   BOOST_REQUIRE(int64_min.ok());
   BOOST_TEST(int64_min.value.value == -9223372036854775808.0);

   const auto int64_max =
       forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("9223372036854775807", options);
   BOOST_REQUIRE(!int64_max.ok());
   BOOST_TEST(int64_max.diagnostics.front().code == "json.type");

   const auto uint64_max =
       forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("18446744073709551615", options);
   BOOST_REQUIRE(!uint64_max.ok());
   BOOST_TEST(uint64_max.diagnostics.front().code == "json.type");

   const auto negative_zero = forge::codec::json::read<forge_json_tests::exact_numeric_scalar>("-0.0", options);
   BOOST_REQUIRE(negative_zero.ok());
   BOOST_TEST(std::signbit(negative_zero.value.value));

   const auto string = forge::codec::json::read<forge_json_tests::exact_numeric_scalar>(R"("1.5")", options);
   BOOST_REQUIRE(!string.ok());
   BOOST_TEST(string.diagnostics.front().code == "json.object");
   BOOST_TEST(string.diagnostics.front().path == "$");

   const auto object = forge::codec::json::read<forge_json_tests::exact_numeric_scalar>(R"({"value":1.5})", options);
   BOOST_REQUIRE(!object.ok());
   BOOST_TEST(object.diagnostics.front().code == "json.type");
   BOOST_TEST(object.diagnostics.front().path == "$");

   const auto record = forge::codec::json::read<forge_json_tests::exact_leaf>("1.5", options);
   BOOST_REQUIRE(!record.ok());
   BOOST_TEST(record.diagnostics.front().code == "json.object");
   BOOST_TEST(record.diagnostics.front().path == "$");
}

BOOST_AUTO_TEST_CASE(json_exact_schema_enums_require_canonical_config_names) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto canonical =
       forge::codec::json::read<forge_json_tests::policy_config>(R"({"path-policy":"direct-only"})", options);
   BOOST_REQUIRE(canonical.ok());
   BOOST_TEST(static_cast<int>(canonical.value.policy) == static_cast<int>(forge_json_tests::path_policy::direct_only));

   const auto numeric = forge::codec::json::read<forge_json_tests::policy_config>(R"({"path-policy":0})", options);
   BOOST_REQUIRE(!numeric.ok());
   BOOST_TEST(numeric.diagnostics.front().code == "json.type");
   BOOST_TEST(numeric.diagnostics.front().path == "path-policy");

   const auto noncanonical =
       forge::codec::json::read<forge_json_tests::policy_config>(R"({"path-policy":"direct_only"})", options);
   BOOST_REQUIRE(!noncanonical.ok());
   BOOST_TEST(noncanonical.diagnostics.front().code == "json.type");
   BOOST_TEST(noncanonical.diagnostics.front().path == "path-policy");
}

BOOST_AUTO_TEST_CASE(json_exact_schema_enum_lists_decode_canonical_config_names) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto canonical = forge::codec::json::read<forge_json_tests::policy_list_config>(
       R"({"policies":["direct-only","direct-preferred"]})", options);
   BOOST_REQUIRE(canonical.ok());
   BOOST_REQUIRE_EQUAL(canonical.value.policies.size(), 2U);
   BOOST_TEST(static_cast<int>(canonical.value.policies[0]) ==
              static_cast<int>(forge_json_tests::path_policy::direct_only));
   BOOST_TEST(static_cast<int>(canonical.value.policies[1]) ==
              static_cast<int>(forge_json_tests::path_policy::direct_preferred));

   const auto numeric = forge::codec::json::read<forge_json_tests::policy_list_config>(R"({"policies":[0]})", options);
   BOOST_REQUIRE(!numeric.ok());
   BOOST_TEST(numeric.diagnostics.front().code == "json.type");
   BOOST_TEST(numeric.diagnostics.front().path == "policies[0]");
}

BOOST_AUTO_TEST_CASE(json_exact_schema_less_enums_match_public_writer_spelling) {
   const auto value = forge_json_tests::exact_enum_record{
       .policy = forge_json_tests::exact_path_policy::direct_only,
   };
   const auto written = forge::codec::json::write(value);
   BOOST_REQUIRE(written.ok());
   BOOST_TEST(written.text.find(R"("direct_only")") != std::string::npos);

   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };
   const auto roundtrip = forge::codec::json::read<forge_json_tests::exact_enum_record>(written.text, options);
   BOOST_REQUIRE(roundtrip.ok());
   BOOST_TEST(static_cast<int>(roundtrip.value.policy) ==
              static_cast<int>(forge_json_tests::exact_path_policy::direct_only));

   const auto config_spelling =
       forge::codec::json::read<forge_json_tests::exact_enum_record>(R"({"policy":"direct-only"})", options);
   BOOST_REQUIRE(!config_spelling.ok());
   BOOST_TEST(config_spelling.diagnostics.front().code == "json.type");
   BOOST_TEST(config_spelling.diagnostics.front().path == "policy");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_validate_chrono_scalar_contracts) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto canonical = forge::codec::json::read<forge_json_tests::exact_chrono_record>(
       R"({"delay":42,"timestamp":"1970-01-01T00:00:01.000"})", options);
   BOOST_REQUIRE(canonical.ok());
   BOOST_TEST(canonical.value.delay.count() == 42);

   const auto boolean_duration = forge::codec::json::read<forge_json_tests::exact_chrono_record>(
       R"({"delay":true,"timestamp":"1970-01-01T00:00:01.000"})", options);
   BOOST_REQUIRE(!boolean_duration.ok());
   BOOST_TEST(boolean_duration.diagnostics.front().code == "json.type");
   BOOST_TEST(boolean_duration.diagnostics.front().path == "delay");

   const auto malformed_timestamp = forge::codec::json::read<forge_json_tests::exact_chrono_record>(
       R"({"delay":42,"timestamp":"not-a-timestamp"})", options);
   BOOST_REQUIRE(!malformed_timestamp.ok());
   BOOST_TEST(malformed_timestamp.diagnostics.front().code == "json.type");
   BOOST_TEST(malformed_timestamp.diagnostics.front().path == "timestamp");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_validate_blob_scalar_contracts) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto canonical =
       forge::codec::json::read<forge_json_tests::exact_blob_record>(R"({"payload":"AQI="})", options);
   BOOST_REQUIRE(canonical.ok());
   BOOST_TEST(canonical.value.payload.data == std::vector<std::uint8_t>({1, 2}));

   const auto boolean = forge::codec::json::read<forge_json_tests::exact_blob_record>(R"({"payload":true})", options);
   BOOST_REQUIRE(!boolean.ok());
   BOOST_TEST(boolean.diagnostics.front().code == "json.type");
   BOOST_TEST(boolean.diagnostics.front().path == "payload");

   const auto malformed = forge::codec::json::read<forge_json_tests::exact_blob_record>(R"({"payload":"!"})", options);
   BOOST_REQUIRE(!malformed.ok());
   BOOST_TEST(malformed.diagnostics.front().code == "json.type");
   BOOST_TEST(malformed.diagnostics.front().path == "payload");

   const auto missing_padding =
       forge::codec::json::read<forge_json_tests::exact_blob_record>(R"({"payload":"AQI"})", options);
   BOOST_REQUIRE(!missing_padding.ok());
   BOOST_TEST(missing_padding.diagnostics.front().code == "json.type");
   BOOST_TEST(missing_padding.diagnostics.front().path == "payload");

   const auto excess_padding =
       forge::codec::json::read<forge_json_tests::exact_blob_record>(R"({"payload":"AQI=="})", options);
   BOOST_REQUIRE(!excess_padding.ok());
   BOOST_TEST(excess_padding.diagnostics.front().code == "json.type");
   BOOST_TEST(excess_padding.diagnostics.front().path == "payload");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_require_canonical_hex_scalar_spelling) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };

   const auto canonical =
       forge::codec::json::read<forge_json_tests::exact_byte_vector_record>(R"({"payload":"0a0b"})", options);
   BOOST_REQUIRE(canonical.ok());

   const auto uppercase =
       forge::codec::json::read<forge_json_tests::exact_byte_vector_record>(R"({"payload":"0A0B"})", options);
   BOOST_REQUIRE(!uppercase.ok());
   BOOST_TEST(uppercase.diagnostics.front().code == "json.type");
   BOOST_TEST(uppercase.diagnostics.front().path == "payload");
}

BOOST_AUTO_TEST_CASE(json_exact_described_records_canonicalize_fallback_string_adapters) {
   const auto options = forge::codec::json::read_options{
       .described_records = forge::codec::json::described_record_policy::exact,
   };
   const auto canonical_json = std::string{R"({"value":")"} + std::string(64U, '0') + R"("})";
   const auto canonical = forge::codec::json::read<forge_json_tests::exact_digest_record>(canonical_json, options);
   BOOST_REQUIRE(canonical.ok());

   const auto short_value =
       forge::codec::json::read<forge_json_tests::exact_digest_record>(R"({"value":"00"})", options);
   BOOST_REQUIRE(!short_value.ok());
   BOOST_TEST(short_value.diagnostics.front().code == "json.type");
   BOOST_TEST(short_value.diagnostics.front().path == "value");

   const auto uppercase_json = std::string{R"({"value":")"} + std::string(64U, 'A') + R"("})";
   const auto uppercase = forge::codec::json::read<forge_json_tests::exact_digest_record>(uppercase_json, options);
   BOOST_REQUIRE(!uppercase.ok());
   BOOST_TEST(uppercase.diagnostics.front().code == "json.type");
   BOOST_TEST(uppercase.diagnostics.front().path == "value");

   const auto fixed_key_canonical =
       forge::codec::json::read<forge_json_tests::exact_fixed_key_record>(canonical_json, options);
   BOOST_REQUIRE(fixed_key_canonical.ok());

   const auto fixed_key_uppercase =
       forge::codec::json::read<forge_json_tests::exact_fixed_key_record>(uppercase_json, options);
   BOOST_REQUIRE(!fixed_key_uppercase.ok());
   BOOST_TEST(fixed_key_uppercase.diagnostics.front().code == "json.type");
   BOOST_TEST(fixed_key_uppercase.diagnostics.front().path == "value");
}

BOOST_AUTO_TEST_CASE(json_exact_duplicate_scan_respects_max_depth) {
   const auto parsed = forge::codec::json::read_value(
       R"({"outer":{"inner":1}})",
       {.max_depth = 1, .described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(!parsed.ok());
   BOOST_TEST(parsed.diagnostics.front().code == "json.depth");
   BOOST_TEST(parsed.diagnostics.front().path == "outer.inner");
}

BOOST_AUTO_TEST_CASE(json_malformed_input_returns_forge_diagnostic) {
   const auto parsed = forge::codec::json::read_value(R"({"unterminated":)");
   BOOST_TEST(!parsed.ok());
   BOOST_REQUIRE_EQUAL(parsed.diagnostics.size(), 1U);
   BOOST_TEST(parsed.diagnostics.front().code == "json.parse");
   BOOST_TEST(parsed.diagnostics.front().message.find("glz::") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(json_write_escapes_control_bytes_inside_strings) {
   const auto expected = std::string{"a\x01\b\0z", 5};
   const auto written =
       forge::codec::json::write_value(forge::variant{forge::mutable_variant_object{}("text", expected)});
   BOOST_REQUIRE(written.ok());
   BOOST_TEST(written.text.find("\\u0001") != std::string::npos);
   const auto escaped_backspace =
       written.text.find("\\b") != std::string::npos || written.text.find("\\u0008") != std::string::npos;
   BOOST_TEST(escaped_backspace);
   BOOST_TEST(written.text.find("\\u0000") != std::string::npos);
   BOOST_TEST(written.text.find('\0') == std::string::npos);

   const auto parsed = forge::codec::json::read_value(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_TEST(parsed.value.get_object()["text"].get_string() == expected);
}

BOOST_AUTO_TEST_CASE(json_large_unsigned_byte_vectors_avoid_per_element_schema_materialization) {
   auto expected = forge_json_tests::byte_payload{};
   expected.value.resize(64U * 1024U);
   for (auto index = std::size_t{}; index < expected.value.size(); ++index) {
      expected.value[index] = static_cast<std::uint8_t>(index);
   }

   auto encoded = std::string{"{\"value\":["};
   encoded.reserve(expected.value.size() * 4U);
   for (auto index = std::size_t{}; index < expected.value.size(); ++index) {
      if (index != 0U) {
         encoded.push_back(',');
      }
      encoded.append(std::to_string(expected.value[index]));
   }
   encoded.append("]}");
   const auto written = forge::codec::json::write(expected);
   BOOST_REQUIRE(written.ok());
   const auto decoded = forge::codec::json::read<forge_json_tests::byte_payload>(
       encoded, {.described_records = forge::codec::json::described_record_policy::exact});
   const auto decode_failure = decoded.diagnostics.empty()
                                   ? std::string{"missing JSON diagnostic"}
                                   : decoded.diagnostics.front().path + ": " + decoded.diagnostics.front().message;
   BOOST_REQUIRE_MESSAGE(decoded.ok(), decode_failure);
   BOOST_TEST(decoded.value.value == expected.value, boost::test_tools::per_element());

   const auto invalid = forge::codec::json::read<forge_json_tests::byte_payload>(
       R"({"value":[0,256]})", {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(!invalid.ok());
   BOOST_TEST(invalid.diagnostics.front().path == "value[1]");
}

BOOST_AUTO_TEST_SUITE_END()
