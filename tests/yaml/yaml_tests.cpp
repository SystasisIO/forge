#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

namespace forge_yaml_tests {

struct http_config {
   std::uint16_t bind_port = 0;
   std::string bind_host;
   bool tls_enabled = false;
   std::vector<std::string> tags;
};

} // namespace forge_yaml_tests

BOOST_DESCRIBE_STRUCT(forge_yaml_tests::http_config, (), (bind_port, bind_host, tls_enabled, tags))

import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
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
import forge.codec.yaml;
import forge.tests.codec.yaml.schema_types;

template <> struct forge::schema::rules<forge_yaml_tests::http_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_yaml_tests::http_config> define() {
      auto schema = forge::schema::object<forge_yaml_tests::http_config>();
      schema.field<&forge_yaml_tests::http_config::bind_port>("bind-port")
          .required()
          .default_value(8080)
          .range(1, 65535);
      schema.field<&forge_yaml_tests::http_config::bind_host>("bind-host").default_value("127.0.0.1");
      schema.field<&forge_yaml_tests::http_config::tls_enabled>("tls-enabled").default_value(false);
      static_cast<void>(schema.field<&forge_yaml_tests::http_config::tags>("tags"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_yaml_tests::nested_limits> {
   [[nodiscard]] static forge::schema::object_schema<forge_yaml_tests::nested_limits> define() {
      auto schema = forge::schema::object<forge_yaml_tests::nested_limits>();
      static_cast<void>(schema.field<&forge_yaml_tests::nested_limits::deadline_ms>("api.deadline-ms"));
      return schema;
   }
};

template <> struct forge::schema::rules<forge_yaml_tests::long_double_config> {
   [[nodiscard]] static forge::schema::object_schema<forge_yaml_tests::long_double_config> define() {
      auto schema = forge::schema::object<forge_yaml_tests::long_double_config>();
      static_cast<void>(schema.field<&forge_yaml_tests::long_double_config::value>("value"));
      return schema;
   }
};

BOOST_AUTO_TEST_SUITE(yaml_codec_tests)

BOOST_AUTO_TEST_CASE(yaml_scalar_bool_writer_preserves_boolean_kind_and_roundtrip) {
   const bool values[]{false, true};
   for (const auto expected : values) {
      BOOST_TEST_CONTEXT("value=" << expected) {
         const auto written = forge::codec::yaml::write(expected);
         BOOST_REQUIRE(written.ok());
         BOOST_TEST(written.text == (expected ? "true" : "false"));

         const auto parsed = forge::codec::yaml::read_value(written.text);
         BOOST_REQUIRE(parsed.ok());
         BOOST_REQUIRE(parsed.value.is_bool());
         BOOST_TEST(parsed.value.as_bool() == expected);

         const auto restored = forge::codec::yaml::read<bool>(written.text);
         BOOST_REQUIRE(restored.ok());
         BOOST_TEST(restored.value == expected);
      }
   }
}

BOOST_AUTO_TEST_CASE(yaml_schema_writer_rejects_long_double_without_narrowing) {
   const auto written = forge::codec::yaml::write(forge_yaml_tests::long_double_config{.value = 1.0L});

   BOOST_REQUIRE(!written.ok());
   BOOST_TEST(written.text.empty());
   BOOST_REQUIRE_EQUAL(written.diagnostics.size(), 1U);
   BOOST_TEST(written.diagnostics.front().path == "value");
   BOOST_TEST(written.diagnostics.front().code == "yaml.type");
   BOOST_TEST(written.diagnostics.front().message == "long double schema fields are not supported by config codecs");
}

BOOST_AUTO_TEST_CASE(yaml_schema_writer_reports_nested_encoding_path) {
   const auto input = forge_yaml_tests::long_double_parent{.nested = {.value = 1.0L}};
   const auto written = forge::codec::yaml::write(input);

   BOOST_REQUIRE(!written.ok());
   BOOST_TEST(written.text.empty());
   BOOST_REQUIRE_EQUAL(written.diagnostics.size(), 1U);
   BOOST_TEST(written.diagnostics.front().path == "nested.value");
   BOOST_TEST(written.diagnostics.front().code == "yaml.type");
   BOOST_TEST(written.diagnostics.front().message == "long double schema fields are not supported by config codecs");

   const auto saved = forge::codec::yaml::save({}, input);
   BOOST_REQUIRE(!saved.ok());
   BOOST_REQUIRE_EQUAL(saved.diagnostics.size(), 1U);
   BOOST_TEST(saved.diagnostics.front().path == "nested.value");
   BOOST_TEST(saved.diagnostics.front().code == "yaml.type");
   BOOST_TEST(saved.diagnostics.front().message == "long double schema fields are not supported by config codecs");
}

BOOST_AUTO_TEST_CASE(yaml_value_roundtrip_preserves_scalars_lists_and_maps) {
   const auto parsed = forge::codec::yaml::read_value("flag: true\n"
                                                      "i: -2\n"
                                                      "u: 7\n"
                                                      "d: 3.5\n"
                                                      "s: x\n"
                                                      "a:\n"
                                                      "  - 1\n"
                                                      "  - b\n");

   BOOST_REQUIRE(parsed.ok());
   const auto& object = parsed.value.get_object();
   BOOST_TEST(object["flag"].as_bool());
   BOOST_TEST(object["i"].as_int64() == -2);
   BOOST_TEST(object["u"].as_uint64() == 7U);
   BOOST_TEST(object["d"].as_double() == 3.5);
   BOOST_TEST(object["s"].get_string() == "x");
   BOOST_REQUIRE_EQUAL(object["a"].get_array().size(), 2U);

   const auto written = forge::codec::yaml::write_value(parsed.value);
   BOOST_REQUIRE(written.ok());
   const auto reparsed = forge::codec::yaml::read_value(written.text);
   BOOST_REQUIRE(reparsed.ok());
   BOOST_TEST(reparsed.value.get_object()["flag"].as_bool());
   BOOST_TEST(reparsed.value.get_object()["i"].as_int64() == -2);
   BOOST_TEST(reparsed.value.get_object()["u"].as_uint64() == 7U);
   BOOST_REQUIRE_EQUAL(reparsed.value.get_object()["a"].get_array().size(), 2U);
}

BOOST_AUTO_TEST_CASE(yaml_document_roundtrip_uses_config_document) {
   auto document = forge::config::core::document{};
   document.set("http.bind-host", "127.0.0.1");
   document.set("http.bind-port", 8080);
   document.set("http.tls-enabled", true);

   const auto written = forge::codec::yaml::write_document(document);
   BOOST_REQUIRE(written.ok());
   const auto parsed = forge::codec::yaml::read_document(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_REQUIRE(parsed.value.try_get("http.bind-host") != nullptr);
   BOOST_REQUIRE(parsed.value.try_get("http.bind-port") != nullptr);
}

BOOST_AUTO_TEST_CASE(yaml_document_roundtrip_preserves_empty_mappings_in_array_records) {
   auto store = forge::config::core::value::object_type{};
   store.emplace("name", "state");
   store.emplace("revision", forge::config::core::value::object_type{});

   auto document = forge::config::core::document{};
   document.set("plugins.db.store.stores",
                forge::config::core::value::array_type{forge::config::core::value{std::move(store)}});

   const auto written = forge::codec::yaml::write_document(document);
   BOOST_REQUIRE(written.ok());
   BOOST_TEST(written.text.find("revision: {}") != std::string::npos);

   const auto parsed = forge::codec::yaml::read_document(written.text);
   BOOST_REQUIRE(parsed.ok());
   const auto* stores = parsed.value.try_get("plugins.db.store.stores");
   BOOST_REQUIRE(stores != nullptr);
   BOOST_REQUIRE(stores->as_array() != nullptr);
   BOOST_REQUIRE_EQUAL(stores->as_array()->size(), 1U);
   const auto* decoded_store = stores->as_array()->front().as_object();
   BOOST_REQUIRE(decoded_store != nullptr);
   BOOST_TEST(std::get<std::string>(decoded_store->at("name").storage) == "state");
   const auto* revision = decoded_store->at("revision").as_object();
   BOOST_REQUIRE(revision != nullptr);
   BOOST_TEST(revision->empty());
}

BOOST_AUTO_TEST_CASE(yaml_typed_read_uses_schema_defaults_validation_and_unknown_policy) {
   const auto parsed = forge::codec::yaml::read<forge_yaml_tests::http_config>("bind-port: 9090\n"
                                                                               "tls-enabled: false\n"
                                                                               "tags:\n"
                                                                               "  - alpha\n"
                                                                               "extra: 1\n");
   BOOST_REQUIRE(parsed.ok());
   BOOST_TEST(parsed.value.bind_port == 9090U);
   BOOST_TEST(parsed.value.bind_host == "127.0.0.1");
   BOOST_REQUIRE_EQUAL(parsed.value.tags.size(), 1U);
   BOOST_TEST(parsed.diagnostics.size() == 1U);
   BOOST_TEST(parsed.diagnostics.front().code == "yaml.unknown");

   auto options = forge::codec::yaml::read_options{};
   options.unknown_fields = forge::codec::yaml::unknown_field_policy::error;
   const auto rejected = forge::codec::yaml::read<forge_yaml_tests::http_config>("bind-port: 9090\n"
                                                                                 "extra: 1\n",
                                                                                 options);
   BOOST_TEST(!rejected.ok());

   const auto invalid = forge::codec::yaml::read<forge_yaml_tests::http_config>("bind-port: 0\n");
   BOOST_TEST(!invalid.ok());
}

BOOST_AUTO_TEST_CASE(yaml_typed_write_uses_canonical_schema_field_names) {
   const auto input = forge_yaml_tests::http_config{
       .bind_port = 9090,
       .bind_host = "127.0.0.1",
       .tls_enabled = true,
       .tags = {"alpha"},
   };
   const auto written = forge::codec::yaml::write(input);
   BOOST_REQUIRE(written.ok());
   BOOST_TEST(written.text.find("bind-port:") != std::string::npos);
   BOOST_TEST(written.text.find("bind_port:") == std::string::npos);

   const auto roundtrip = forge::codec::yaml::read<forge_yaml_tests::http_config>(written.text);
   BOOST_REQUIRE(roundtrip.ok());
   BOOST_TEST(roundtrip.value.bind_port == input.bind_port);
   BOOST_TEST(roundtrip.value.bind_host == input.bind_host);
   BOOST_TEST(roundtrip.value.tls_enabled == input.tls_enabled);
   BOOST_TEST(roundtrip.value.tags == input.tags);
}

BOOST_AUTO_TEST_CASE(yaml_nested_schema_records_use_canonical_names_and_roundtrip) {
   const auto input = forge_yaml_tests::nested_config{
       .limits = {.deadline_ms = 2500},
   };

   const auto written = forge::codec::yaml::write(input);
   const auto write_error = written.diagnostics.empty() ? std::string{"YAML write failed without diagnostics"}
                                                        : written.diagnostics.front().message;
   BOOST_REQUIRE_MESSAGE(written.ok(), write_error);
   BOOST_TEST(written.text.find("deadline-ms:") != std::string::npos);
   BOOST_TEST(written.text.find("deadline_ms:") == std::string::npos);

   const auto roundtrip = forge::codec::yaml::read<forge_yaml_tests::nested_config>(written.text);
   BOOST_REQUIRE(roundtrip.ok());
   BOOST_TEST(roundtrip.value.limits.deadline_ms == input.limits.deadline_ms);
}

BOOST_AUTO_TEST_CASE(yaml_nested_schema_records_apply_unknown_field_policy) {
   constexpr auto input = "limits:\n"
                          "  api:\n"
                          "    deadline-ms: 2500\n"
                          "    extra: true\n";

   const auto warned = forge::codec::yaml::read<forge_yaml_tests::nested_config>(input);
   BOOST_REQUIRE(warned.ok());
   BOOST_REQUIRE_EQUAL(warned.diagnostics.size(), 1U);
   BOOST_TEST(warned.diagnostics.front().code == "yaml.unknown");
   BOOST_TEST(warned.diagnostics.front().path == "limits.api.extra");

   auto rejected_options = forge::codec::yaml::read_options{};
   rejected_options.unknown_fields = forge::codec::yaml::unknown_field_policy::error;
   const auto rejected = forge::codec::yaml::read<forge_yaml_tests::nested_config>(input, rejected_options);
   BOOST_TEST(!rejected.ok());

   auto ignored_options = forge::codec::yaml::read_options{};
   ignored_options.unknown_fields = forge::codec::yaml::unknown_field_policy::ignore;
   const auto ignored = forge::codec::yaml::read<forge_yaml_tests::nested_config>(input, ignored_options);
   BOOST_REQUIRE(ignored.ok());
   BOOST_TEST(ignored.diagnostics.empty());
   BOOST_TEST(ignored.value.limits.deadline_ms == 2500U);
}

BOOST_AUTO_TEST_CASE(yaml_nested_schema_records_report_non_object_at_child_path) {
   const auto rejected = forge::codec::yaml::read<forge_yaml_tests::nested_config>("limits: bad\n");

   BOOST_REQUIRE(!rejected.ok());
   BOOST_REQUIRE_EQUAL(rejected.diagnostics.size(), 1U);
   BOOST_TEST(rejected.diagnostics.front().code == "yaml.type");
   BOOST_TEST(rejected.diagnostics.front().path == "limits");
}

BOOST_AUTO_TEST_CASE(yaml_typed_load_uses_same_unknown_policy_as_read) {
   const auto path = std::filesystem::temp_directory_path() /
                     ("forge_yaml_unknown_policy_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".yaml");
   {
      auto out = std::ofstream{path};
      out << "bind-port: 9090\nextra: 1\n";
   }
   struct cleanup {
      std::filesystem::path path;
      ~cleanup() {
         std::error_code ignored;
         std::filesystem::remove(path, ignored);
      }
   } remove_file{path};

   const auto warned = forge::codec::yaml::load<forge_yaml_tests::http_config>(path);
   BOOST_REQUIRE(warned.ok());
   BOOST_REQUIRE_EQUAL(warned.diagnostics.size(), 1U);
   BOOST_TEST(warned.diagnostics.front().code == "yaml.unknown");

   auto rejected_options = forge::codec::yaml::read_options{};
   rejected_options.unknown_fields = forge::codec::yaml::unknown_field_policy::error;
   const auto rejected = forge::codec::yaml::load<forge_yaml_tests::http_config>(path, rejected_options);
   BOOST_TEST(!rejected.ok());
   BOOST_REQUIRE_EQUAL(rejected.diagnostics.size(), 1U);
   BOOST_TEST(rejected.diagnostics.front().code == "yaml.unknown");

   auto ignored_options = forge::codec::yaml::read_options{};
   ignored_options.unknown_fields = forge::codec::yaml::unknown_field_policy::ignore;
   const auto ignored = forge::codec::yaml::load<forge_yaml_tests::http_config>(path, ignored_options);
   BOOST_REQUIRE(ignored.ok());
   BOOST_TEST(ignored.diagnostics.empty());
   BOOST_TEST(ignored.value.bind_port == 9090U);
}

BOOST_AUTO_TEST_CASE(yaml_malformed_input_returns_forge_diagnostic) {
   const auto parsed = forge::codec::yaml::read_value("root: [unterminated\n");
   BOOST_TEST(!parsed.ok());
   BOOST_REQUIRE_EQUAL(parsed.diagnostics.size(), 1U);
   BOOST_TEST(parsed.diagnostics.front().code == "yaml.parse");
   BOOST_TEST(parsed.diagnostics.front().message.find("glz::") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
