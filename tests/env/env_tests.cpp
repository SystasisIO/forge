#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.config.env;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

namespace {

struct http_config {
   std::uint16_t bind_port = 0;
   std::string bind_host;
   bool tls_enabled = false;
   std::vector<std::string> tags;
   std::string token;
   std::string legacy_token;
};

struct flat_config {
   std::string log_level;
};

struct env_name_collision_config {
   std::string hyphen_name;
   std::string underscore_name;
};

struct flat_http_collision_config {
   std::uint16_t http_bind_port = 0;
};

struct alias_collision_config {
   std::string token;
   std::string auth_token;
};

struct same_path_alias_config {
   std::string token;
};

struct unsigned_counter_config {
   std::uint64_t count = 0;
};

#if defined(_WIN32)
class scoped_environment_variable {
 public:
   scoped_environment_variable(const wchar_t* name, const wchar_t* value) : name_{name} {
      auto size = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
      if (size != 0) {
         old_value_.resize(size);
         GetEnvironmentVariableW(name_.c_str(), old_value_.data(), size);
         old_value_.resize(size - 1);
         had_old_value_ = true;
      }
      SetEnvironmentVariableW(name_.c_str(), value);
   }

   ~scoped_environment_variable() {
      SetEnvironmentVariableW(name_.c_str(), had_old_value_ ? old_value_.c_str() : nullptr);
   }

   scoped_environment_variable(const scoped_environment_variable&) = delete;
   scoped_environment_variable& operator=(const scoped_environment_variable&) = delete;

 private:
   std::wstring name_;
   std::wstring old_value_;
   bool had_old_value_ = false;
};
#endif

[[nodiscard]] forge::config::core::component_registry make_registry() {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<http_config>("http"));
   registry.add(forge::config::core::describe_component<flat_config>(""));
   return registry;
}

[[nodiscard]] const forge::schema::diagnostic* find_diagnostic(const std::vector<forge::schema::diagnostic>& diagnostics,
                                                             std::string_view code) {
   for (const auto& entry : diagnostics) {
      if (entry.code == code) {
         return &entry;
      }
   }
   return nullptr;
}

} // namespace

BOOST_DESCRIBE_STRUCT(http_config, (), (bind_port, bind_host, tls_enabled, tags, token, legacy_token))
BOOST_DESCRIBE_STRUCT(flat_config, (), (log_level))
BOOST_DESCRIBE_STRUCT(env_name_collision_config, (), (hyphen_name, underscore_name))
BOOST_DESCRIBE_STRUCT(flat_http_collision_config, (), (http_bind_port))
BOOST_DESCRIBE_STRUCT(alias_collision_config, (), (token, auth_token))
BOOST_DESCRIBE_STRUCT(same_path_alias_config, (), (token))
BOOST_DESCRIBE_STRUCT(unsigned_counter_config, (), (count))

template <> struct forge::schema::rules<http_config> {
   [[nodiscard]] static forge::schema::object_schema<http_config> define() {
      auto schema = forge::schema::object<http_config>();
      schema.field<&http_config::bind_port>("bind-port")
          .alias("port")
          .default_value(8080)
          .description("HTTP bind port.")
          .range(1, 65535);
      schema.field<&http_config::bind_host>("bind-host").default_value("127.0.0.1").description("HTTP bind host.");
      schema.field<&http_config::tls_enabled>("tls-enabled").default_value(false).description("Enable TLS.");
      static_cast<void>(schema.field<&http_config::tags>("tags").description("Comma-separated route tags."));
      schema.field<&http_config::token>("token").secret().description("HTTP bearer token.");
      schema.field<&http_config::legacy_token>("legacy-token").deprecated("use token").description("Old token.");
      return schema;
   }
};

template <> struct forge::schema::rules<flat_config> {
   [[nodiscard]] static forge::schema::object_schema<flat_config> define() {
      auto schema = forge::schema::object<flat_config>();
      schema.field<&flat_config::log_level>("log-level").default_value("info").description("Root log level.");
      return schema;
   }
};

template <> struct forge::schema::rules<env_name_collision_config> {
   [[nodiscard]] static forge::schema::object_schema<env_name_collision_config> define() {
      auto schema = forge::schema::object<env_name_collision_config>();
      static_cast<void>(schema.field<&env_name_collision_config::hyphen_name>("log-level"));
      static_cast<void>(schema.field<&env_name_collision_config::underscore_name>("log_level"));
      return schema;
   }
};

template <> struct forge::schema::rules<flat_http_collision_config> {
   [[nodiscard]] static forge::schema::object_schema<flat_http_collision_config> define() {
      auto schema = forge::schema::object<flat_http_collision_config>();
      static_cast<void>(schema.field<&flat_http_collision_config::http_bind_port>("http-bind-port"));
      return schema;
   }
};

template <> struct forge::schema::rules<alias_collision_config> {
   [[nodiscard]] static forge::schema::object_schema<alias_collision_config> define() {
      auto schema = forge::schema::object<alias_collision_config>();
      schema.field<&alias_collision_config::token>("token").alias("auth-token");
      static_cast<void>(schema.field<&alias_collision_config::auth_token>("auth_token"));
      return schema;
   }
};

template <> struct forge::schema::rules<same_path_alias_config> {
   [[nodiscard]] static forge::schema::object_schema<same_path_alias_config> define() {
      auto schema = forge::schema::object<same_path_alias_config>();
      schema.field<&same_path_alias_config::token>("token").alias("auth-token").alias("auth_token");
      return schema;
   }
};

template <> struct forge::schema::rules<unsigned_counter_config> {
   [[nodiscard]] static forge::schema::object_schema<unsigned_counter_config> define() {
      auto schema = forge::schema::object<unsigned_counter_config>();
      static_cast<void>(schema.field<&unsigned_counter_config::count>("count"));
      return schema;
   }
};

BOOST_AUTO_TEST_CASE(env_reads_dotenv_document_with_aliases_flat_fields_and_lists) {
   const auto registry = make_registry();
   const auto input = std::string{
       "# local development overrides\n"
       "export FORGE_HTTP_PORT=9090\n"
       "FORGE_HTTP_TLS_ENABLED=yes\n"
       "FORGE_HTTP_TAGS=alpha\\,one,beta\n"
       "FORGE_LOG_LEVEL=debug\n"};

   const auto parsed = forge::config::env::read_document(
       input, registry, forge::config::env::read_options{.prefix = "FORGE", .source_name = "workspace/.env"});
   BOOST_TEST(parsed.ok());

   const auto decoded_http = forge::config::core::decode<http_config>(parsed.value, "http");
   BOOST_TEST(decoded_http.ok());
   BOOST_TEST(decoded_http.value.bind_port == 9090U);
   BOOST_TEST(decoded_http.value.tls_enabled);
   BOOST_REQUIRE_EQUAL(decoded_http.value.tags.size(), 2U);
   BOOST_TEST(decoded_http.value.tags[0] == "alpha,one");
   BOOST_TEST(decoded_http.value.tags[1] == "beta");

   const auto decoded_flat = forge::config::core::decode<flat_config>(parsed.value);
   BOOST_TEST(decoded_flat.ok());
   BOOST_TEST(decoded_flat.value.log_level == "debug");
}

BOOST_AUTO_TEST_CASE(env_reads_empty_list_value_as_empty_vector) {
   const auto registry = make_registry();

   const auto parsed = forge::config::env::read_document(
       "FORGE_HTTP_TAGS=\n", registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(parsed.ok());

   const auto decoded_http = forge::config::core::decode<http_config>(parsed.value, "http");
   BOOST_TEST(decoded_http.ok());
   BOOST_TEST(decoded_http.value.tags.empty());
}

BOOST_AUTO_TEST_CASE(env_reports_dotenv_parse_duplicate_and_source_locations) {
   const auto registry = make_registry();
   const auto input = std::string{
       "FORGE_HTTP_BIND_HOST=\"127.0.0.1\"\n"
       "broken line\n"
       "FORGE_HTTP_BIND_HOST='0.0.0.0'\n"};

   const auto parsed =
       forge::config::env::read_document(input, registry, forge::config::env::read_options{.prefix = "FORGE", .source_name = ".env"});
   BOOST_TEST(!parsed.ok());
   const auto* parse_error = find_diagnostic(parsed.diagnostics, "env.parse");
   BOOST_REQUIRE(parse_error != nullptr);
   BOOST_TEST(parse_error->path == ".env");
   BOOST_TEST(parse_error->line == 2U);

   const auto* duplicate = find_diagnostic(parsed.diagnostics, "env.duplicate");
   BOOST_REQUIRE(duplicate != nullptr);
   BOOST_TEST(static_cast<int>(duplicate->level) == static_cast<int>(forge::schema::severity::warning));

   const auto* host = parsed.value.try_get("http.bind-host");
   BOOST_REQUIRE(host != nullptr);
   BOOST_TEST(std::get<std::string>(host->storage) == "0.0.0.0");
}

BOOST_AUTO_TEST_CASE(env_reads_injected_environment_snapshot_without_global_mutation) {
   const auto registry = make_registry();
   const auto variables = std::vector<forge::config::env::environment_variable>{
       {.name = "FORGE_HTTP_BIND_PORT", .value = "7777", .location = {.source = "test-env"}},
       {.name = "FORGE_UNKNOWN_FLAG", .value = "1", .location = {.source = "test-env"}},
       {.name = "UNRELATED_HTTP_BIND_PORT", .value = "9999", .location = {.source = "test-env"}},
   };

   auto parsed =
       forge::config::env::read_variables(variables, registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(parsed.ok());
   BOOST_REQUIRE(find_diagnostic(parsed.diagnostics, "env.unknown") != nullptr);

   const auto decoded = forge::config::core::decode<http_config>(parsed.value, "http");
   BOOST_TEST(decoded.ok());
   BOOST_TEST(decoded.value.bind_port == 7777U);

   parsed = forge::config::env::read_variables(
       variables, registry,
       forge::config::env::read_options{.prefix = "FORGE", .unknown_variables = forge::config::env::unknown_variable_policy::error});
   BOOST_TEST(!parsed.ok());
}

BOOST_AUTO_TEST_CASE(env_reports_alias_conflicts_deprecated_fields_and_conversion_errors) {
   const auto registry = make_registry();
   auto parsed = forge::config::env::read_document(
       "FORGE_HTTP_BIND_PORT=9090\n"
       "FORGE_HTTP_PORT=8080\n"
       "FORGE_HTTP_TLS_ENABLED=maybe\n"
       "FORGE_HTTP_LEGACY_TOKEN=old\n",
       registry, forge::config::env::read_options{.prefix = "FORGE"});

   BOOST_TEST(!parsed.ok());
   BOOST_REQUIRE(find_diagnostic(parsed.diagnostics, "env.alias_conflict") != nullptr);
   const auto* convert = find_diagnostic(parsed.diagnostics, "env.convert");
   BOOST_REQUIRE(convert != nullptr);
   BOOST_TEST(convert->path == "http.tls-enabled");
   BOOST_TEST(convert->message == "boolean has invalid syntax");
   BOOST_TEST(convert->line == 3U);
   const auto* deprecated = find_diagnostic(parsed.diagnostics, "env.deprecated");
   BOOST_REQUIRE(deprecated != nullptr);
   BOOST_TEST(static_cast<int>(deprecated->level) == static_cast<int>(forge::schema::severity::warning));

   const auto* port = parsed.value.try_get("http.bind-port");
   BOOST_REQUIRE(port != nullptr);
   BOOST_TEST(std::get<std::uint64_t>(port->storage) == 9090U);
}

BOOST_AUTO_TEST_CASE(env_typed_helpers_decode_schema_and_validate_ranges) {
   const auto parsed = forge::config::env::read<http_config>(
       "FORGE_HTTP_BIND_PORT=0\n", "http", forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(!parsed.ok());
   BOOST_REQUIRE(find_diagnostic(parsed.diagnostics, "schema.range") != nullptr);
}

BOOST_AUTO_TEST_CASE(env_rejects_negative_text_for_unsigned_fields_before_decode) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<unsigned_counter_config>("counter"));

   const auto rejected = forge::config::env::read_document(
       "FORGE_COUNTER_COUNT=-1\n", registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(!rejected.ok());
   const auto* convert = find_diagnostic(rejected.diagnostics, "env.convert");
   BOOST_REQUIRE(convert != nullptr);
   BOOST_TEST(convert->path == "counter.count");
   BOOST_TEST(convert->message == "integer has invalid syntax");

   const auto accepted = forge::config::env::read_document(
       "FORGE_COUNTER_COUNT=42\n", registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(accepted.ok());
   const auto decoded = forge::config::core::decode<unsigned_counter_config>(accepted.value, "counter");
   BOOST_TEST(decoded.ok());
   BOOST_TEST(decoded.value.count == 42U);
}

BOOST_AUTO_TEST_CASE(env_preserves_integer_conversion_reason_and_source_position) {
   const auto registry = make_registry();
   const auto variables = std::vector<forge::config::env::environment_variable>{
      {.name = "FORGE_HTTP_BIND_PORT", .value = "conversion-secret-marker",
       .location = {.source = "source-secret-marker", .line = 7, .column = 11}},
   };
   const auto parsed = forge::config::env::read_variables(
       variables, registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(!parsed.ok());
   BOOST_REQUIRE_EQUAL(parsed.diagnostics.size(), 1U);
   const auto& diagnostic = parsed.diagnostics.front();
   BOOST_TEST(diagnostic.code == "env.convert");
   BOOST_TEST(diagnostic.path == "http.bind-port");
   BOOST_TEST(diagnostic.message == "integer has invalid syntax");
   BOOST_TEST(diagnostic.line == 7U);
   BOOST_TEST(diagnostic.column == 11U);
}

BOOST_AUTO_TEST_CASE(env_writes_dotenv_and_examples_with_secret_redaction) {
   const auto registry = make_registry();
   auto document = forge::config::core::document{};
   document.set("http.bind-port", 9090);
   document.set("http.tags", forge::config::core::value::array_type{forge::config::core::value{"blue"}, forge::config::core::value{"green"}});
   document.set("http.token", "secret-value");

   const auto written = forge::config::env::write_document(document, registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(written.ok());
   BOOST_TEST(written.text.find("FORGE_HTTP_BIND_PORT=9090") != std::string::npos);
   BOOST_TEST(written.text.find("FORGE_HTTP_TAGS=blue,green") != std::string::npos);
   BOOST_TEST(written.text.find("secret-value") == std::string::npos);
   BOOST_TEST(written.text.find("FORGE_HTTP_TOKEN=<redacted>") != std::string::npos);

   const auto example = forge::config::env::write_example(registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(example.ok());
   BOOST_TEST(example.text.find("# HTTP bind port.") != std::string::npos);
   BOOST_TEST(example.text.find("FORGE_HTTP_BIND_PORT=8080") != std::string::npos);
   BOOST_TEST(example.text.find("FORGE_HTTP_TOKEN=") != std::string::npos);
   BOOST_TEST(example.text.find("<redacted>") == std::string::npos);
   BOOST_TEST(example.text.find("FORGE_LOG_LEVEL=info") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(env_write_document_empty_list_roundtrips_as_empty_vector) {
   const auto registry = make_registry();
   auto document = forge::config::core::document{};
   document.set("http.tags", forge::config::core::value::array_type{});

   const auto written = forge::config::env::write_document(document, registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(written.ok());
   BOOST_TEST(written.text.find("FORGE_HTTP_TAGS=\n") != std::string::npos);

   const auto parsed =
       forge::config::env::read_document(written.text, registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(parsed.ok());

   const auto decoded_http = forge::config::core::decode<http_config>(parsed.value, "http");
   BOOST_TEST(decoded_http.ok());
   BOOST_TEST(decoded_http.value.tags.empty());
}

BOOST_AUTO_TEST_CASE(env_rejects_canonical_name_collisions_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<env_name_collision_config>(""));

   const auto parsed = forge::config::env::read_document(
       "FORGE_LOG_LEVEL=debug\n", registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(!parsed.ok());

   const auto* conflict = find_diagnostic(parsed.diagnostics, "env.name_conflict");
   BOOST_REQUIRE(conflict != nullptr);
   BOOST_TEST(conflict->message.find("FORGE_LOG_LEVEL") != std::string::npos);
   BOOST_TEST(conflict->message.find("log-level") != std::string::npos);
   BOOST_TEST(conflict->message.find("log_level") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(env_rejects_flat_and_sectioned_name_collisions_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<http_config>("http"));
   registry.add(forge::config::core::describe_component<flat_http_collision_config>(""));

   const auto parsed = forge::config::env::read_document(
       "FORGE_HTTP_BIND_PORT=9090\n", registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(!parsed.ok());

   const auto* conflict = find_diagnostic(parsed.diagnostics, "env.name_conflict");
   BOOST_REQUIRE(conflict != nullptr);
   BOOST_TEST(conflict->message.find("FORGE_HTTP_BIND_PORT") != std::string::npos);
   BOOST_TEST(conflict->message.find("http.bind-port") != std::string::npos);
   BOOST_TEST(conflict->message.find("http-bind-port") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(env_rejects_alias_name_collisions_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<alias_collision_config>("auth"));

   const auto parsed = forge::config::env::read_document(
       "FORGE_AUTH_AUTH_TOKEN=value\n", registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(!parsed.ok());

   const auto* conflict = find_diagnostic(parsed.diagnostics, "env.name_conflict");
   BOOST_REQUIRE(conflict != nullptr);
   BOOST_TEST(conflict->message.find("FORGE_AUTH_AUTH_TOKEN") != std::string::npos);
   BOOST_TEST(conflict->message.find("auth.token") != std::string::npos);
   BOOST_TEST(conflict->message.find("auth.auth_token") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(env_write_example_rejects_name_collisions_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<env_name_collision_config>(""));

   const auto example = forge::config::env::write_example(registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(!example.ok());

   const auto* conflict = find_diagnostic(example.diagnostics, "env.name_conflict");
   BOOST_REQUIRE(conflict != nullptr);
   BOOST_TEST(conflict->message.find("FORGE_LOG_LEVEL") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(env_write_example_rejects_alias_name_collisions_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<alias_collision_config>("auth"));

   const auto example = forge::config::env::write_example(registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(!example.ok());

   const auto* conflict = find_diagnostic(example.diagnostics, "env.name_conflict");
   BOOST_REQUIRE(conflict != nullptr);
   BOOST_TEST(conflict->message.find("FORGE_AUTH_AUTH_TOKEN") != std::string::npos);
   BOOST_TEST(conflict->message.find("auth.token") != std::string::npos);
   BOOST_TEST(conflict->message.find("auth.auth_token") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(env_write_document_rejects_alias_name_collisions_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<alias_collision_config>("auth"));

   auto document = forge::config::core::document{};
   document.set("auth.token", "token-value");
   document.set("auth.auth_token", "auth-token-value");

   const auto written = forge::config::env::write_document(document, registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(!written.ok());

   const auto* conflict = find_diagnostic(written.diagnostics, "env.name_conflict");
   BOOST_REQUIRE(conflict != nullptr);
   BOOST_TEST(conflict->message.find("FORGE_AUTH_AUTH_TOKEN") != std::string::npos);
   BOOST_TEST(conflict->message.find("auth.token") != std::string::npos);
   BOOST_TEST(conflict->message.find("auth.auth_token") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(env_allows_same_path_alias_duplicates_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<same_path_alias_config>("auth"));

   const auto parsed = forge::config::env::read_document(
       "FORGE_AUTH_AUTH_TOKEN=token-value\n", registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(parsed.ok());
   BOOST_TEST(find_diagnostic(parsed.diagnostics, "env.name_conflict") == nullptr);

   const auto decoded = forge::config::core::decode<same_path_alias_config>(parsed.value, "auth");
   BOOST_TEST(decoded.ok());
   BOOST_TEST(decoded.value.token == "token-value");
}

BOOST_AUTO_TEST_CASE(env_write_paths_allow_same_path_alias_duplicates_after_normalization) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<same_path_alias_config>("auth"));

   auto document = forge::config::core::document{};
   document.set("auth.token", "token-value");

   const auto example = forge::config::env::write_example(registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(example.ok());
   BOOST_TEST(find_diagnostic(example.diagnostics, "env.name_conflict") == nullptr);

   const auto written = forge::config::env::write_document(document, registry, forge::config::env::write_options{.prefix = "FORGE"});
   BOOST_TEST(written.ok());
   BOOST_TEST(find_diagnostic(written.diagnostics, "env.name_conflict") == nullptr);
   BOOST_TEST(written.text.find("FORGE_AUTH_TOKEN=token-value") != std::string::npos);
}

#if defined(_WIN32)
BOOST_AUTO_TEST_CASE(env_windows_process_snapshot_preserves_utf16_values_as_utf8) {
   auto registry = forge::config::core::component_registry{};
   registry.add(forge::config::core::describe_component<same_path_alias_config>("win"));
   const auto variable = scoped_environment_variable{L"FORGE_WIN_TOKEN", L"\x043A\x043B\x044E\x0447-\x2713"};

   const auto parsed = forge::config::env::read_process_document(registry, forge::config::env::read_options{.prefix = "FORGE"});
   BOOST_TEST(parsed.ok());

   const auto decoded = forge::config::core::decode<same_path_alias_config>(parsed.value, "win");
   BOOST_TEST(decoded.ok());
   BOOST_TEST(decoded.value.token == "\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87-\xE2\x9C\x93");
}
#endif
