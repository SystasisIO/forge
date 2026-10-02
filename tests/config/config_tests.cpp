#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <any>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <optional>
#include <vector>

import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.schema.diagnostic;
import forge.schema.exceptions;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.schema.scalar;

namespace {

struct http_config {
   std::uint16_t bind_port = 0;
   std::string bind_host;
   bool tls_enabled = false;
   std::vector<std::string> tags;
   std::string token;
};

struct flat_config {
   std::string log_level;
};

struct optional_default_config {
   std::optional<std::uint16_t> wrapped_port;
   std::optional<std::uint16_t> raw_port;
};

struct nested_key_config {
   std::string id;
   std::string private_key;
   std::string input_profile = "forge";
   std::vector<std::string> purposes;
};
BOOST_DESCRIBE_STRUCT(nested_key_config, (), (id, private_key, input_profile, purposes))
static_assert(boost::describe::has_describe_members<nested_key_config>::value);

struct nested_signer_config {
   std::vector<nested_key_config> keys;
   std::string default_output_profile = "forge";
};

struct defaulted_nested_signer_config {
   std::vector<nested_key_config> keys;
};

struct string_shorthand_item_config {
   string_shorthand_item_config() = default;
   explicit string_shorthand_item_config(std::string value) : name{value} {}

   std::string name;
};

struct string_shorthand_list_config {
   std::vector<string_shorthand_item_config> items;
};

enum class scalar_test_mode : std::uint8_t {
   fast_mode = 1,
   safe_mode = 2,
};
BOOST_DESCRIBE_ENUM(scalar_test_mode, fast_mode, safe_mode)

} // namespace

BOOST_DESCRIBE_STRUCT(http_config, (), (bind_port, bind_host, tls_enabled, tags, token))
BOOST_DESCRIBE_STRUCT(flat_config, (), (log_level))
BOOST_DESCRIBE_STRUCT(optional_default_config, (), (wrapped_port, raw_port))
BOOST_DESCRIBE_STRUCT(nested_signer_config, (), (keys, default_output_profile))
BOOST_DESCRIBE_STRUCT(defaulted_nested_signer_config, (), (keys))
BOOST_DESCRIBE_STRUCT(string_shorthand_item_config, (), (name))
BOOST_DESCRIBE_STRUCT(string_shorthand_list_config, (), (items))

template <> struct forge::schema::rules<http_config> {
   [[nodiscard]] static forge::schema::object_schema<http_config> define() {
      auto schema = forge::schema::object<http_config>();
      schema.field<&http_config::bind_port>("bind-port").alias("port").required().default_value(8080).range(1, 65535);
      schema.field<&http_config::bind_host>("bind-host").default_value("127.0.0.1");
      schema.field<&http_config::tls_enabled>("tls-enabled").default_value(false);
      static_cast<void>(schema.field<&http_config::tags>("tags"));
      schema.field<&http_config::token>("token").secret().deprecated("use vault-ref");
      return schema;
   }
};

template <> struct forge::schema::rules<flat_config> {
   [[nodiscard]] static forge::schema::object_schema<flat_config> define() {
      auto schema = forge::schema::object<flat_config>();
      schema.field<&flat_config::log_level>("log-level").default_value("info");
      return schema;
   }
};

template <> struct forge::schema::rules<optional_default_config> {
   [[nodiscard]] static forge::schema::object_schema<optional_default_config> define() {
      auto schema = forge::schema::object<optional_default_config>();
      schema.field<&optional_default_config::wrapped_port>("wrapped-port")
          .default_value(std::optional<std::uint16_t>{443})
          .range(1, 65535);
      schema.field<&optional_default_config::raw_port>("raw-port").default_value(8443).range(1, 65535);
      return schema;
   }
};

template <> struct forge::schema::rules<nested_key_config> {
   [[nodiscard]] static forge::schema::object_schema<nested_key_config> define() {
      auto schema = forge::schema::object<nested_key_config>();
      schema.field<&nested_key_config::id>("id").required().non_empty();
      schema.field<&nested_key_config::private_key>("private-key").required().non_empty().secret();
      schema.field<&nested_key_config::input_profile>("input-profile").default_value("forge");
      schema.field<&nested_key_config::purposes>("purposes").min_items(1).each_non_empty();
      return schema;
   }
};

template <> struct forge::schema::rules<nested_signer_config> {
   [[nodiscard]] static forge::schema::object_schema<nested_signer_config> define() {
      auto schema = forge::schema::object<nested_signer_config>();
      schema.field<&nested_signer_config::keys>("keys")
          .items<nested_key_config>()
          .secret()
          .unique_by<&nested_key_config::id>()
          .description("Configured local signing keys");
      schema.field<&nested_signer_config::default_output_profile>("default-output-profile").default_value("forge");
      return schema;
   }
};

template <> struct forge::schema::rules<defaulted_nested_signer_config> {
   [[nodiscard]] static forge::schema::object_schema<defaulted_nested_signer_config> define() {
      auto schema = forge::schema::object<defaulted_nested_signer_config>();
      schema.field<&defaulted_nested_signer_config::keys>("keys").items<nested_key_config>().default_value(
          std::vector<nested_key_config>{nested_key_config{
              .id = "default",
              .private_key = "PVT_DEFAULT",
              .input_profile = "forge",
              .purposes = {"storage.receipt"},
          }});
      return schema;
   }
};

template <> struct forge::schema::rules<string_shorthand_item_config> {
   [[nodiscard]] static forge::schema::object_schema<string_shorthand_item_config> define() {
      auto schema = forge::schema::object<string_shorthand_item_config>();
      schema.field<&string_shorthand_item_config::name>("name").non_empty();
      return schema;
   }
};

template <> struct forge::schema::rules<string_shorthand_list_config> {
   [[nodiscard]] static forge::schema::object_schema<string_shorthand_list_config> define() {
      auto schema = forge::schema::object<string_shorthand_list_config>();
      schema.field<&string_shorthand_list_config::items>("items").items<string_shorthand_item_config>();
      return schema;
   }
};

[[nodiscard]] bool has_diagnostic(const std::vector<forge::schema::diagnostic>& entries, std::string_view path,
                                  std::string_view code) {
   return std::ranges::any_of(
       entries, [&](const forge::schema::diagnostic& entry) { return entry.path == path && entry.code == code; });
}

BOOST_AUTO_TEST_CASE(config_key_path_splits_dotted_keys) {
   auto segments = forge::config::core::key_path{.value = "http.bind-port"}.segments();
   BOOST_REQUIRE_EQUAL(segments.size(), 2U);
   BOOST_TEST(segments[0] == "http");
   BOOST_TEST(segments[1] == "bind-port");

   auto compacted = forge::config::core::key_path{.value = ".http..tls-enabled."}.segments();
   BOOST_REQUIRE_EQUAL(compacted.size(), 2U);
   BOOST_TEST(compacted[0] == "http");
   BOOST_TEST(compacted[1] == "tls-enabled");
}

BOOST_AUTO_TEST_CASE(config_document_paths_merge_and_decode) {
   auto defaults = forge::config::core::defaults_for<http_config>("http");
   auto file = forge::config::core::document{};
   file.set("http.bind-port", 8081);
   auto cli = forge::config::core::document{};
   cli.set("http.bind-port", 9090);
   cli.set("http.tls-enabled", false);
   cli.set("http.tags", forge::config::core::value::array_type{forge::config::core::value{"alpha"},
                                                               forge::config::core::value{"beta"}});

   const auto merged = forge::config::core::merge({defaults, file, cli});
   const auto decoded = forge::config::core::decode<http_config>(merged, "http");
   BOOST_TEST(decoded.ok());
   BOOST_TEST(decoded.value.bind_port == 9090U);
   BOOST_TEST(decoded.value.bind_host == "127.0.0.1");
   BOOST_TEST(!decoded.value.tls_enabled);
   BOOST_REQUIRE_EQUAL(decoded.value.tags.size(), 2U);
   BOOST_TEST(decoded.value.tags[1] == "beta");
}

BOOST_AUTO_TEST_CASE(config_optional_defaults_export_and_decode_consistently) {
   const auto descriptor = forge::config::core::describe_component<optional_default_config>("http");
   BOOST_REQUIRE_EQUAL(descriptor.fields.size(), 2U);
   BOOST_TEST(std::get<std::uint64_t>(descriptor.fields[0].default_value.storage) == 443U);
   BOOST_TEST(std::get<std::uint64_t>(descriptor.fields[1].default_value.storage) == 8443U);

   const auto defaults = forge::config::core::defaults_for<optional_default_config>("http");
   const auto* wrapped = defaults.try_get("http.wrapped-port");
   const auto* raw = defaults.try_get("http.raw-port");
   BOOST_REQUIRE(wrapped != nullptr);
   BOOST_REQUIRE(raw != nullptr);
   BOOST_TEST(std::get<std::uint64_t>(wrapped->storage) == 443U);
   BOOST_TEST(std::get<std::uint64_t>(raw->storage) == 8443U);

   const auto decoded = forge::config::core::decode<optional_default_config>(defaults, "http");
   BOOST_REQUIRE(decoded.ok());
   BOOST_REQUIRE(decoded.value.wrapped_port.has_value());
   BOOST_TEST(*decoded.value.wrapped_port == 443U);
   BOOST_REQUIRE(decoded.value.raw_port.has_value());
   BOOST_TEST(*decoded.value.raw_port == 8443U);
}

BOOST_AUTO_TEST_CASE(config_decode_rejects_integer_overflow_before_range_validation) {
   auto numeric = forge::config::core::document{};
   numeric.set("http.bind-port", 70000);
   const auto decoded_numeric = forge::config::core::decode<http_config>(numeric, "http");

   BOOST_TEST(!decoded_numeric.ok());
   BOOST_TEST(has_diagnostic(decoded_numeric.diagnostics.entries, "http.bind-port", "config.type"));
   BOOST_TEST(decoded_numeric.value.bind_port == 8080U);

   auto text = forge::config::core::document{};
   text.set("http.bind-port", std::string{"70000"});
   const auto decoded_text = forge::config::core::decode<http_config>(text, "http");

   BOOST_TEST(!decoded_text.ok());
   BOOST_TEST(has_diagnostic(decoded_text.diagnostics.entries, "http.bind-port", "config.type"));
   BOOST_TEST(decoded_text.value.bind_port == 8080U);

   auto trailing = forge::config::core::document{};
   trailing.set("http.bind-port", std::string{"123abc"});
   const auto decoded_trailing = forge::config::core::decode<http_config>(trailing, "http");

   BOOST_TEST(!decoded_trailing.ok());
   BOOST_TEST(has_diagnostic(decoded_trailing.diagnostics.entries, "http.bind-port", "config.type"));
   BOOST_TEST(decoded_trailing.value.bind_port == 8080U);
}

BOOST_AUTO_TEST_CASE(schema_scalar_text_codec_is_shared_and_checked) {
   BOOST_TEST(forge::schema::parse_scalar_text<std::uint16_t>("65535") == 65535U);
   BOOST_CHECK_THROW((void)forge::schema::parse_scalar_text<std::uint16_t>("65536"),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW((void)forge::schema::parse_scalar_text<std::uint16_t>("12tail"),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW((void)forge::schema::parse_scalar_text<std::uint16_t>("-1"),
                     forge::schema::exceptions::invalid_value);

   BOOST_TEST(forge::schema::parse_scalar_text<bool>("yes"));
   BOOST_CHECK_THROW((void)forge::schema::parse_scalar_text<bool>("maybe"), forge::schema::exceptions::invalid_value);

   BOOST_TEST(static_cast<int>(forge::schema::parse_scalar_text<scalar_test_mode>("safe-mode")) ==
              static_cast<int>(scalar_test_mode::safe_mode));
   BOOST_TEST(forge::schema::format_scalar_text(scalar_test_mode::fast_mode).value_or("") == "fast-mode");
   BOOST_TEST(forge::schema::format_scalar_text(std::optional<std::uint16_t>{7}).value_or("") == "7");
   BOOST_TEST(!forge::schema::format_scalar_text(std::optional<std::uint16_t>{}).has_value());
}

BOOST_AUTO_TEST_CASE(config_value_to_any_rejects_integer_overflow_and_trailing_junk) {
   BOOST_CHECK_THROW(static_cast<void>(forge::config::core::value_to_any(
                         forge::config::core::value{std::numeric_limits<std::uint64_t>::max()},
                         forge::schema::value_kind::signed_integer)),
                     forge::schema::exceptions::invalid_value);

   BOOST_CHECK_THROW(static_cast<void>(forge::config::core::value_to_any(
                         forge::config::core::value{std::string{"123abc"}}, forge::schema::value_kind::signed_integer)),
                     forge::schema::exceptions::invalid_value);

   BOOST_CHECK_THROW(static_cast<void>(forge::config::core::value_to_any(forge::config::core::value{std::int64_t{-1}},
                                                                         forge::schema::value_kind::unsigned_integer)),
                     forge::schema::exceptions::invalid_value);

   BOOST_CHECK_THROW(
       static_cast<void>(forge::config::core::value_to_any(forge::config::core::value{std::string{"123abc"}},
                                                           forge::schema::value_kind::unsigned_integer)),
       forge::schema::exceptions::invalid_value);

   const auto valid = forge::config::core::value_to_any(forge::config::core::value{std::string{"123"}},
                                                        forge::schema::value_kind::unsigned_integer);
   BOOST_REQUIRE(valid.has_value());
   BOOST_TEST(std::any_cast<std::uint64_t>(valid) == 123U);
}

BOOST_AUTO_TEST_CASE(config_document_erase_and_rename_nested_keys) {
   auto doc = forge::config::core::document{};
   doc.set("http.bind-port", 8080);
   doc.set("http.host", "127.0.0.1");
   doc.set("legacy.timeout", 30);

   BOOST_TEST(doc.rename("http.host", "http.bind-host"));
   BOOST_TEST(doc.try_get("http.host") == nullptr);
   const auto* host = doc.try_get("http.bind-host");
   BOOST_REQUIRE(host != nullptr);
   BOOST_TEST(std::get<std::string>(host->storage) == "127.0.0.1");

   BOOST_CHECK_THROW(static_cast<void>(doc.rename("http.bind-port", "http.bind-host")), std::invalid_argument);
   BOOST_TEST(doc.rename("http.bind-port", "http.bind-host", true));
   const auto* overwritten = doc.try_get("http.bind-host");
   BOOST_REQUIRE(overwritten != nullptr);
   BOOST_TEST(std::get<std::int64_t>(overwritten->storage) == 8080);

   BOOST_TEST(doc.erase("legacy.timeout"));
   BOOST_TEST(doc.try_get("legacy.timeout") == nullptr);
   BOOST_TEST(!doc.erase("legacy.timeout"));
   BOOST_TEST(!doc.rename("missing.value", "http.missing"));
}

BOOST_AUTO_TEST_CASE(config_reports_required_unknown_deprecated_and_redacts) {
   auto doc = forge::config::core::document{};
   doc.set("http.bind-host", "127.0.0.1");
   doc.set("http.token", "secret-value");
   doc.set("http.extra", "ignored");

   const auto decoded = forge::config::core::decode<http_config>(doc, "http");
   BOOST_TEST(!decoded.ok());
   BOOST_TEST(decoded.diagnostics.entries.size() >= 3U);

   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<http_config>("http"));
   auto redacted = forge::config::core::redact(doc, registry);
   const auto* token = redacted.try_get("http.token");
   BOOST_REQUIRE(token != nullptr);
   BOOST_TEST(std::get<std::string>(token->storage) == "<redacted>");
}

BOOST_AUTO_TEST_CASE(config_registry_rejects_duplicate_aliases) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<http_config>("http"));
   BOOST_CHECK_THROW(registry.add(forge::config::core::describe_component<http_config>("http")), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(config_ingestion_only_fields_reject_canonical_and_alias_input) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::component_descriptor{
       .section = "service",
       .fields = {forge::config::core::field_descriptor{
           .name = "removed-option",
           .aliases = {"legacy-option"},
           .kind = forge::schema::value_kind::string,
           .deprecated = true,
           .deprecated_message = "removed in Forge 8.9",
           .ingestion_only = true,
       }},
   });

   auto document = forge::config::core::document{};
   document.set("service.removed-option", "canonical");
   document.set("service.legacy-option", "alias");

   const auto diagnostics = forge::config::core::validate_ingestion(document, registry);
   BOOST_REQUIRE_EQUAL(diagnostics.size(), 2U);
   BOOST_TEST(has_diagnostic(diagnostics, "service.removed-option", "config.removed"));
   BOOST_TEST(has_diagnostic(diagnostics, "service.legacy-option", "config.removed"));
   BOOST_TEST(static_cast<int>(diagnostics.front().level) == static_cast<int>(forge::schema::severity::error));
   BOOST_TEST(diagnostics.front().message == "removed in Forge 8.9");
}

BOOST_AUTO_TEST_CASE(config_registry_supports_empty_component_sections) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<flat_config>(""));

   auto doc = forge::config::core::document{};
   doc.set("log-level", "debug");

   const auto view = forge::config::core::component_view{doc, ""};
   BOOST_TEST(view.get_or<std::string>("log-level", "info") == "debug");

   const auto decoded = forge::config::core::decode<flat_config>(doc);
   BOOST_TEST(decoded.ok());
   BOOST_TEST(decoded.value.log_level == "debug");
   BOOST_REQUIRE_EQUAL(registry.components().front().fields.size(), 1U);
   BOOST_TEST(registry.components().front().fields.front().has_default);
}

BOOST_AUTO_TEST_CASE(config_component_view_rejects_integer_overflow) {
   auto doc = forge::config::core::document{};
   doc.set("http.small", std::uint64_t{70000});
   doc.set("http.negative", std::int64_t{-1});

   const auto view = forge::config::core::component_view{doc, "http"};
   BOOST_CHECK_THROW(static_cast<void>(view.get_or<std::uint16_t>("small", 0)),
                     forge::schema::exceptions::invalid_value);
   BOOST_CHECK_THROW(static_cast<void>(view.get_or<std::uint16_t>("negative", 0)),
                     forge::schema::exceptions::invalid_value);
   BOOST_TEST(view.get_or<std::uint16_t>("missing", 42) == 42U);
}

BOOST_AUTO_TEST_CASE(config_decodes_nested_object_lists_with_item_defaults_and_paths) {
   auto key = forge::config::core::value::object_type{};
   key["id"] = forge::config::core::value{"provider"};
   key["private-key"] = forge::config::core::value{"PVT_FAKE"};
   key["purposes"] = forge::config::core::value::array_type{forge::config::core::value{"storage.receipt"}};
   key["unknown"] = forge::config::core::value{"ignored"};

   auto doc = forge::config::core::document{};
   doc.set("plugins.crypto.signer.keys", forge::config::core::value::array_type{forge::config::core::value{key}});

   const auto decoded = forge::config::core::decode<nested_signer_config>(doc, "plugins.crypto.signer");
   BOOST_TEST(decoded.ok());
   BOOST_REQUIRE_EQUAL(decoded.value.keys.size(), 1U);
   BOOST_TEST(decoded.value.keys.front().id == "provider");
   BOOST_TEST(decoded.value.keys.front().private_key == "PVT_FAKE");
   BOOST_TEST(decoded.value.keys.front().input_profile == "forge");
   BOOST_TEST(decoded.value.default_output_profile == "forge");
   BOOST_TEST(has_diagnostic(decoded.diagnostics.entries, "plugins.crypto.signer.keys[0].unknown", "config.unknown"));
}

BOOST_AUTO_TEST_CASE(config_nested_object_list_validators_report_stable_diagnostics) {
   auto invalid = forge::config::core::value::object_type{};
   invalid["id"] = forge::config::core::value{""};
   invalid["private-key"] = forge::config::core::value{""};
   invalid["purposes"] = forge::config::core::value::array_type{forge::config::core::value{""}};

   auto duplicate = forge::config::core::value::object_type{};
   duplicate["id"] = forge::config::core::value{"duplicate"};
   duplicate["private-key"] = forge::config::core::value{"PVT_ONE"};
   duplicate["purposes"] = forge::config::core::value::array_type{forge::config::core::value{"storage.receipt"}};

   auto duplicate_two = duplicate;
   duplicate_two["private-key"] = forge::config::core::value{"PVT_TWO"};

   auto doc = forge::config::core::document{};
   doc.set("plugins.crypto.signer.keys", forge::config::core::value::array_type{
                                             forge::config::core::value{invalid},
                                             forge::config::core::value{duplicate},
                                             forge::config::core::value{duplicate_two},
                                         });

   const auto decoded = forge::config::core::decode<nested_signer_config>(doc, "plugins.crypto.signer");
   BOOST_TEST(!decoded.ok());
   BOOST_TEST(has_diagnostic(decoded.diagnostics.entries, "plugins.crypto.signer.keys[0].id", "schema.non_empty"));
   BOOST_TEST(
       has_diagnostic(decoded.diagnostics.entries, "plugins.crypto.signer.keys[0].private-key", "schema.non_empty"));
   BOOST_TEST(
       has_diagnostic(decoded.diagnostics.entries, "plugins.crypto.signer.keys[0].purposes[0]", "schema.non_empty"));
   BOOST_TEST(has_diagnostic(decoded.diagnostics.entries, "plugins.crypto.signer.keys", "schema.unique"));
}

BOOST_AUTO_TEST_CASE(config_string_shorthand_object_list_entries_run_nested_validation) {
   auto doc = forge::config::core::document{};
   doc.set("test.items", forge::config::core::value::array_type{forge::config::core::value{""}});

   const auto decoded = forge::config::core::decode<string_shorthand_list_config>(doc, "test");
   BOOST_TEST(!decoded.ok());
   BOOST_TEST(has_diagnostic(decoded.diagnostics.entries, "test.items[0].name", "schema.non_empty"));
}

BOOST_AUTO_TEST_CASE(config_formats_full_decode_diagnostics) {
   auto invalid = forge::config::core::value::object_type{};
   invalid["id"] = forge::config::core::value{""};
   invalid["private-key"] = forge::config::core::value{""};
   invalid["purposes"] = forge::config::core::value::array_type{forge::config::core::value{""}};

   auto doc = forge::config::core::document{};
   doc.set("plugins.crypto.signer.keys", forge::config::core::value::array_type{forge::config::core::value{invalid}});

   const auto decoded = forge::config::core::decode<nested_signer_config>(doc, "plugins.crypto.signer");
   BOOST_TEST(!decoded.ok());

   const auto message =
       forge::config::core::format_decode_diagnostics("invalid crypto signer config", decoded.diagnostics);
   BOOST_TEST(message.find("invalid crypto signer config") != std::string::npos);
   BOOST_TEST(message.find("plugins.crypto.signer.keys[0].id schema.non_empty") != std::string::npos);
   BOOST_TEST(message.find("plugins.crypto.signer.keys[0].private-key schema.non_empty") != std::string::npos);
   BOOST_TEST(message.find("plugins.crypto.signer.keys[0].purposes[0] schema.non_empty") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(config_describes_secret_object_list_without_nested_env_fields) {
   const auto descriptor = forge::config::core::describe_component<nested_signer_config>("plugins.crypto.signer");
   BOOST_REQUIRE_EQUAL(descriptor.fields.size(), 2U);
   BOOST_TEST(descriptor.fields[0].name == "keys");
   BOOST_TEST(static_cast<int>(descriptor.fields[0].kind) == static_cast<int>(forge::schema::value_kind::object_list));
   BOOST_TEST(descriptor.fields[0].secret);
   BOOST_TEST(descriptor.fields[1].name == "default-output-profile");
}

BOOST_AUTO_TEST_CASE(config_describes_object_list_default_values) {
   const auto descriptor =
       forge::config::core::describe_component<defaulted_nested_signer_config>("plugins.crypto.signer");
   BOOST_REQUIRE_EQUAL(descriptor.fields.size(), 1U);
   BOOST_TEST(descriptor.fields[0].has_default);

   const auto* defaults = descriptor.fields[0].default_value.as_array();
   BOOST_REQUIRE(defaults != nullptr);
   BOOST_REQUIRE_EQUAL(defaults->size(), 1U);

   const auto* first = (*defaults)[0].as_object();
   BOOST_REQUIRE(first != nullptr);
   BOOST_TEST(std::get<std::string>(first->at("id").storage) == "default");
   BOOST_TEST(std::get<std::string>(first->at("private-key").storage) == "PVT_DEFAULT");
   BOOST_TEST(std::get<std::string>(first->at("input-profile").storage) == "forge");

   const auto document = forge::config::core::defaults_for<defaulted_nested_signer_config>("plugins.crypto.signer");
   const auto decoded = forge::config::core::decode<defaulted_nested_signer_config>(document, "plugins.crypto.signer");
   BOOST_TEST(decoded.ok());
   BOOST_REQUIRE_EQUAL(decoded.value.keys.size(), 1U);
   BOOST_TEST(decoded.value.keys.front().id == "default");
   BOOST_TEST(decoded.value.keys.front().private_key == "PVT_DEFAULT");
   BOOST_TEST(decoded.value.keys.front().input_profile == "forge");
   BOOST_REQUIRE_EQUAL(decoded.value.keys.front().purposes.size(), 1U);
   BOOST_TEST(decoded.value.keys.front().purposes.front() == "storage.receipt");
}

BOOST_AUTO_TEST_CASE(config_migration_chain_updates_document_version) {
   auto doc = forge::config::core::document{};
   doc.set("http.port", 8080);

   auto plan = forge::config::core::migration_plan{};
   plan.step(0, 1, "rename port", [](forge::config::core::document& input) {
      static_cast<void>(input.rename("http.port", "http.bind-port"));
   });
   plan.step(1, 2, "add host", [](forge::config::core::document& input) { input.set("http.bind-host", "127.0.0.1"); });

   const auto migrated = forge::config::core::migrate(std::move(doc), plan);
   BOOST_TEST(migrated.ok());
   BOOST_TEST(migrated.from_version == 0U);
   BOOST_TEST(migrated.to_version == 2U);
   BOOST_TEST(migrated.value.try_get("http.port") == nullptr);
   BOOST_REQUIRE(migrated.value.try_get("http.bind-port") != nullptr);
   BOOST_REQUIRE(migrated.value.try_get("http.bind-host") != nullptr);
   const auto* version = migrated.value.try_get("version");
   BOOST_REQUIRE(version != nullptr);
   BOOST_TEST(std::get<std::uint64_t>(version->storage) == 2U);
}

BOOST_AUTO_TEST_CASE(config_migration_reports_missing_and_future_versions) {
   auto plan = forge::config::core::migration_plan{};
   plan.step(0, 1, "first", [](forge::config::core::document&) {});
   plan.step(2, 3, "gap", [](forge::config::core::document&) {});

   auto missing = forge::config::core::migrate(forge::config::core::document{}, plan);
   BOOST_TEST(!missing.ok());
   BOOST_REQUIRE_EQUAL(missing.diagnostics.size(), 1U);
   BOOST_TEST(missing.diagnostics.front().code == "config.migration.missing-step");

   auto future_doc = forge::config::core::document{};
   future_doc.set("version", 9U);
   auto future = forge::config::core::migrate(std::move(future_doc), plan);
   BOOST_TEST(!future.ok());
   BOOST_REQUIRE_EQUAL(future.diagnostics.size(), 1U);
   BOOST_TEST(future.diagnostics.front().code == "config.migration.future-version");
}
