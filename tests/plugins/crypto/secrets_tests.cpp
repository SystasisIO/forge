#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.crypto.symmetric.aes;
import forge.codec.base64;
import forge.crypto.symmetric.kdf;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.types;
import forge.crypto.keystore.encrypted_file;
import forge.config.env;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.crypto.secrets.types;
import forge.config.program_options;
import forge.schema.value_kind;

#include "details/require_complete_file_read.hxx"

namespace crypto_secrets = forge::plugins::crypto::secrets;
namespace keystore = forge::crypto::keystore;

namespace {

constexpr auto crypto_secrets_section = "plugins.crypto.secrets";

[[nodiscard]] forge::config::core::value array(std::vector<std::string> values) {
   auto result = forge::config::core::value::array_type{};
   for (auto& value : values) {
      result.emplace_back(std::move(value));
   }
   return forge::config::core::value{std::move(result)};
}

[[nodiscard]] forge::config::core::value
source_value(std::string value, crypto_secrets::encoding encoding = crypto_secrets::encoding::raw) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("type", forge::config::core::value{"value"});
   object.emplace("encoding", forge::config::core::value{encoding == crypto_secrets::encoding::hex      ? "hex"
                                                         : encoding == crypto_secrets::encoding::base64 ? "base64"
                                                                                                        : "raw"});
   object.emplace("value", forge::config::core::value{std::move(value)});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value
source_file(const std::filesystem::path& path, crypto_secrets::encoding encoding = crypto_secrets::encoding::raw) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("type", forge::config::core::value{"file"});
   object.emplace("encoding", forge::config::core::value{encoding == crypto_secrets::encoding::hex      ? "hex"
                                                         : encoding == crypto_secrets::encoding::base64 ? "base64"
                                                                                                        : "raw"});
   object.emplace("path", forge::config::core::value{path.string()});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value source_encrypted_file(const std::filesystem::path& path,
                                                               std::string passphrase) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("type", forge::config::core::value{"encrypted_file"});
   object.emplace("path", forge::config::core::value{path.string()});
   object.emplace("passphrase-value", forge::config::core::value{std::move(passphrase)});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value
source_encrypted_file_with_passphrase_file(const std::filesystem::path& path, const std::filesystem::path& passphrase) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("type", forge::config::core::value{"encrypted_file"});
   object.emplace("path", forge::config::core::value{path.string()});
   object.emplace("passphrase-file", forge::config::core::value{passphrase.string()});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value secret_entry(std::string id, forge::config::core::value source,
                                                      std::vector<std::string> purposes,
                                                      std::vector<std::string> operations,
                                                      bool allow_raw_export = false) {
   auto object = forge::config::core::value::object_type{};
   object.emplace("id", forge::config::core::value{std::move(id)});
   object.emplace("kind", forge::config::core::value{"symmetric_key"});
   object.emplace("source", std::move(source));
   object.emplace("purposes", array(std::move(purposes)));
   object.emplace("operations", array(std::move(operations)));
   object.emplace("allow-raw-export", forge::config::core::value{allow_raw_export});
   return forge::config::core::value{std::move(object)};
}

[[nodiscard]] forge::config::core::value secret_entry_with_limit(std::string limit_name, std::uint64_t limit) {
   auto entry = secret_entry("data-key", source_value(std::string(32, 'K')), {"payload.encrypt"}, {"encrypt_aes_gcm"});
   auto& object = std::get<forge::config::core::value::object_type>(entry.storage);
   object.emplace(std::move(limit_name), forge::config::core::value{limit});
   return entry;
}

[[nodiscard]] forge::config::core::document secrets_config(std::vector<forge::config::core::value> secrets) {
   auto document = forge::config::core::document{};
   document.set("plugins.crypto.secrets.secrets",
                forge::config::core::value::array_type(secrets.begin(), secrets.end()));
   return document;
}

void write_secret_file(const std::filesystem::path& path, const forge::crypto::core::bytes& value) {
   auto out = std::ofstream{path, std::ios::binary | std::ios::trunc};
   out.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
}

[[nodiscard]] forge::api::core::handle<crypto_secrets::api>
configured_api(forge::asio::runtime& runtime, crypto_secrets::plugin& plugin,
               const forge::config::core::document& document) {
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, crypto_secrets_section}));
   auto registry = forge::api::core::registry{};
   auto installer = forge::api::core::installer{registry};
   forge::asio::blocking::run(runtime, plugin.provide(installer));
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto events = forge::app::event_journal{};
   auto context = forge::app::plugin_context{scheduler, registry, events};
   forge::asio::blocking::run(runtime, plugin.initialize(context));
   forge::asio::blocking::run(runtime, plugin.startup());
   return registry.get<crypto_secrets::api>(crypto_secrets::api::ref());
}

[[nodiscard]] forge::crypto::core::bytes bytes(std::string_view value) {
   return forge::crypto::core::bytes{value.begin(), value.end()};
}

[[nodiscard]] forge::crypto::core::bytes encrypted_container(forge::crypto::core::bytes plaintext,
                                                             std::string_view password) {
   return keystore::encrypt_file({
       .plaintext = forge::crypto::core::secret_bytes{std::move(plaintext)},
       .password = forge::crypto::core::secret_string{std::string{password}},
       .encryption = {.scrypt_n = 1024, .scrypt_max_memory_bytes = 8ULL * 1024ULL * 1024ULL},
   });
}

[[nodiscard]] std::string write_temp_file(std::string name, std::string_view value) {
   const auto path = std::filesystem::temp_directory_path() / name;
   auto out = std::ofstream{path, std::ios::binary | std::ios::trunc};
   out.write(value.data(), static_cast<std::streamsize>(value.size()));
   return path.string();
}

} // namespace

BOOST_AUTO_TEST_SUITE(crypto_secrets_tests)

BOOST_AUTO_TEST_CASE(crypto_secrets_configure_and_provide_do_not_open_material_admission) {
   auto runtime = forge::asio::runtime{{.worker_threads = 1}};
   auto plugin = crypto_secrets::plugin{};
   auto document = forge::config::core::document{};
   forge::asio::blocking::run(runtime,
                              plugin.configure(forge::config::core::component_view{document, crypto_secrets_section}));
   auto registry = forge::api::core::registry{};
   auto installer = forge::api::core::installer{registry};
   forge::asio::blocking::run(runtime, plugin.provide(installer));
   auto api = registry.get<crypto_secrets::api>(crypto_secrets::api::ref());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->get_bytes({"uninitialized", "fixture.read"})),
                     crypto_secrets::exceptions::stopped);
}

BOOST_AUTO_TEST_CASE(crypto_secrets_config_direct_defaults_match_schema_constants) try {
   const auto value = crypto_secrets::config{};

   BOOST_TEST(value.default_max_plaintext_bytes == crypto_secrets::default_max_plaintext_bytes);
   BOOST_TEST(value.default_max_ciphertext_bytes == crypto_secrets::default_max_ciphertext_bytes);
   BOOST_TEST(value.default_max_aad_bytes == crypto_secrets::default_max_aad_bytes);
   BOOST_TEST(value.default_max_plaintext_bytes <= crypto_secrets::aes_update_bytes_ceiling);
   BOOST_TEST(value.default_max_ciphertext_bytes <= crypto_secrets::aes_update_bytes_ceiling);
   BOOST_TEST(value.default_max_aad_bytes <= crypto_secrets::aes_update_bytes_ceiling);
   BOOST_TEST(value.encrypted_file_max_scrypt_n == crypto_secrets::default_encrypted_file_max_scrypt_n);
   BOOST_TEST(value.encrypted_file_max_scrypt_r == crypto_secrets::default_encrypted_file_max_scrypt_r);
   BOOST_TEST(value.encrypted_file_max_scrypt_p == crypto_secrets::default_encrypted_file_max_scrypt_p);
   BOOST_TEST(value.encrypted_file_max_scrypt_memory_bytes ==
              crypto_secrets::default_encrypted_file_max_scrypt_memory_bytes);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_descriptor_redacts_config_and_keeps_api_local) try {
   static_assert(forge::api::core::local_interface<crypto_secrets::api>);
   static_assert(!forge::api::core::remote_interface<crypto_secrets::api>);

   auto plugin = crypto_secrets::plugin{};
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == crypto_secrets_section);

   const auto secrets =
       std::ranges::find_if(descriptor->fields, [](const auto& field) { return field.name == "secrets"; });
   BOOST_REQUIRE(secrets != descriptor->fields.end());
   BOOST_TEST(secrets->secret);
   BOOST_TEST(static_cast<int>(secrets->kind) == static_cast<int>(forge::schema::value_kind::object_list));

   const auto has_default_field = [&](std::string_view name, std::uint64_t expected) {
      const auto found =
          std::ranges::find_if(descriptor->fields, [&](const auto& field) { return field.name == name; });
      BOOST_REQUIRE(found != descriptor->fields.end());
      BOOST_TEST(found->has_default);
      BOOST_TEST(std::get<std::uint64_t>(found->default_value.storage) == expected);
   };
   has_default_field("default-max-plaintext-bytes", crypto_secrets::default_max_plaintext_bytes);
   has_default_field("default-max-ciphertext-bytes", crypto_secrets::default_max_ciphertext_bytes);
   has_default_field("default-max-aad-bytes", crypto_secrets::default_max_aad_bytes);
   has_default_field("encrypted-file-max-scrypt-n", crypto_secrets::default_encrypted_file_max_scrypt_n);
   has_default_field("encrypted-file-max-scrypt-r", crypto_secrets::default_encrypted_file_max_scrypt_r);
   has_default_field("encrypted-file-max-scrypt-p", crypto_secrets::default_encrypted_file_max_scrypt_p);
   has_default_field("encrypted-file-max-scrypt-memory-bytes",
                     crypto_secrets::default_encrypted_file_max_scrypt_memory_bytes);

   auto registry = forge::config::core::component_registry{};
   registry.add(*descriptor);
   const auto redacted =
       forge::config::core::redact(secrets_config({secret_entry("session", source_value("super-secret"),
                                                                {"payload.decrypt"}, {"get_bytes"}, true)}),
                                   registry);
   const auto* value = redacted.try_get("plugins.crypto.secrets.secrets");
   BOOST_REQUIRE(value != nullptr);
   const auto* text = std::get_if<std::string>(&value->storage);
   BOOST_REQUIRE(text != nullptr);
   BOOST_TEST(*text == "<redacted>");
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_rejects_default_payload_limits_above_aes_update_ceiling) try {
   const auto too_large = crypto_secrets::aes_update_bytes_ceiling + 1U;

   auto plaintext_document = secrets_config(
       {secret_entry("data-key", source_value(std::string(32, 'K')), {"payload.encrypt"}, {"encrypt_aes_gcm"})});
   plaintext_document.set("plugins.crypto.secrets.default-max-plaintext-bytes", forge::config::core::value{too_large});
   auto plaintext_plugin = crypto_secrets::plugin{};
   auto plaintext_runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(plaintext_runtime, plaintext_plugin.configure(forge::config::core::component_view{
                                                         plaintext_document, crypto_secrets_section})),
       crypto_secrets::exceptions::invalid_config);

   auto ciphertext_document = secrets_config(
       {secret_entry("data-key", source_value(std::string(32, 'K')), {"payload.decrypt"}, {"decrypt_aes_gcm"})});
   ciphertext_document.set("plugins.crypto.secrets.default-max-ciphertext-bytes",
                           forge::config::core::value{too_large});
   auto ciphertext_plugin = crypto_secrets::plugin{};
   auto ciphertext_runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(ciphertext_runtime, ciphertext_plugin.configure(forge::config::core::component_view{
                                                          ciphertext_document, crypto_secrets_section})),
       crypto_secrets::exceptions::invalid_config);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_rejects_per_secret_payload_limits_above_aes_update_ceiling) try {
   const auto too_large = crypto_secrets::aes_update_bytes_ceiling + 1U;

   auto plaintext_plugin = crypto_secrets::plugin{};
   auto plaintext_runtime = forge::asio::runtime{};
   const auto plaintext_document = secrets_config({secret_entry_with_limit("max-plaintext-bytes", too_large)});
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(plaintext_runtime, plaintext_plugin.configure(forge::config::core::component_view{
                                                         plaintext_document, crypto_secrets_section})),
       crypto_secrets::exceptions::invalid_config);

   auto ciphertext_plugin = crypto_secrets::plugin{};
   auto ciphertext_runtime = forge::asio::runtime{};
   const auto ciphertext_document = secrets_config({secret_entry_with_limit("max-ciphertext-bytes", too_large)});
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(ciphertext_runtime, ciphertext_plugin.configure(forge::config::core::component_view{
                                                          ciphertext_document, crypto_secrets_section})),
       crypto_secrets::exceptions::invalid_config);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_value_source_denies_raw_export_by_default) try {
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(
       runtime, plugin,
       secrets_config({secret_entry("session", source_value("super-secret"), {"payload.decrypt"}, {"get_bytes"})}));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->get_bytes(crypto_secrets::get_request{
                                                             .secret_id = "session",
                                                             .purpose = "payload.decrypt",
                                                         })),
                     crypto_secrets::exceptions::operation_denied);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_value_source_exports_only_when_allowed) try {
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(runtime, plugin,
                             secrets_config({secret_entry("session", source_value("super-secret"), {"payload.decrypt"},
                                                          {"get_bytes"}, true)}));

   const auto result = forge::asio::blocking::run(runtime, api->get_bytes(crypto_secrets::get_request{
                                                               .secret_id = "session",
                                                               .purpose = "payload.decrypt",
                                                           }));

   BOOST_TEST(result.secret_id == "session");
   BOOST_TEST(result.bytes == bytes("super-secret"));
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_file_source_loads_bounded_secret_without_env_parser) try {
   const auto path = write_temp_file("forge-crypto-secrets-file-source.txt", "file-secret");
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(
       runtime, plugin,
       secrets_config({secret_entry("file", source_file(path), {"payload.decrypt"}, {"get_bytes"}, true)}));

   const auto result = forge::asio::blocking::run(runtime, api->get_bytes(crypto_secrets::get_request{
                                                               .secret_id = "file",
                                                               .purpose = "payload.decrypt",
                                                           }));

   BOOST_TEST(result.bytes == bytes("file-secret"));
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_file_source_rejects_short_read) try {
   auto input = std::istringstream{"abc"};
   auto output = forge::crypto::core::bytes(5U);
   input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));

   BOOST_CHECK_THROW(crypto_secrets::require_complete_file_read(input, output.size(), "/tmp/short-secret", "short"),
                     crypto_secrets::exceptions::invalid_source);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_hkdf_and_aes_gcm_are_purpose_gated) try {
   const auto key = std::string(32, 'K');
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(runtime, plugin,
                             secrets_config({secret_entry("data-key", source_value(key), {"payload.decrypt"},
                                                          {"derive_hkdf_sha256", "decrypt_aes_gcm"})}));

   const auto derived = forge::asio::blocking::run(runtime, api->derive_hkdf_sha256(crypto_secrets::derive_request{
                                                                .secret_id = "data-key",
                                                                .purpose = "payload.decrypt",
                                                                .salt = bytes("salt"),
                                                                .info = bytes("info"),
                                                                .output_size = 32,
                                                            }));
   BOOST_TEST(derived.bytes.size() == 32U);

   auto aes_key = forge::crypto::symmetric::aes::make_aes256_key(
       std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
   const auto encrypted = forge::crypto::symmetric::aes::encrypt_aes256_gcm({
       .key = aes_key,
       .nonce = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
       .plaintext = bytes("payload"),
       .aad = bytes("aad"),
   });

   const auto decrypted = forge::asio::blocking::run(runtime, api->decrypt_aes_gcm(crypto_secrets::aead_decrypt_request{
                                                                  .secret_id = "data-key",
                                                                  .purpose = "payload.decrypt",
                                                                  .nonce = encrypted.nonce,
                                                                  .tag = encrypted.tag,
                                                                  .ciphertext = encrypted.ciphertext,
                                                                  .aad = bytes("aad"),
                                                              }));
   BOOST_TEST(decrypted.plaintext == bytes("payload"));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->derive_hkdf_sha256(crypto_secrets::derive_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "other",
                                                             .salt = bytes("salt"),
                                                             .info = bytes("info"),
                                                             .output_size = 32,
                                                         })),
                     crypto_secrets::exceptions::purpose_denied);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_hkdf_invalid_output_size_maps_to_invalid_secret) try {
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(runtime, plugin,
                             secrets_config({secret_entry("data-key", source_value("derive-secret"),
                                                          {"data-key.derivation"}, {"derive_hkdf_sha256"})}));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->derive_hkdf_sha256(crypto_secrets::derive_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "data-key.derivation",
                                                             .salt = bytes("salt"),
                                                             .info = bytes("info"),
                                                             .output_size = 0,
                                                         })),
                     crypto_secrets::exceptions::invalid_secret);

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->derive_hkdf_sha256(crypto_secrets::derive_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "data-key.derivation",
                                                             .salt = bytes("salt"),
                                                             .info = bytes("info"),
                                                             .output_size = 255U * 32U + 1U,
                                                         })),
                     crypto_secrets::exceptions::invalid_secret);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_encrypt_aes_gcm_maps_malformed_nonce) try {
   const auto key = std::string(32, 'K');
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(
       runtime, plugin,
       secrets_config({secret_entry("data-key", source_value(key), {"payload.encrypt"}, {"encrypt_aes_gcm"})}));

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->encrypt_aes_gcm(crypto_secrets::aead_encrypt_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "payload.encrypt",
                                                             .nonce = {0, 1, 2},
                                                             .plaintext = bytes("payload"),
                                                             .aad = bytes("aad"),
                                                         })),
                     crypto_secrets::exceptions::invalid_secret);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_encrypt_aes_gcm_rejects_oversized_aad_before_crypto) try {
   const auto key = std::string(32, 'K');
   auto document =
       secrets_config({secret_entry("data-key", source_value(key), {"payload.encrypt"}, {"encrypt_aes_gcm"})});
   document.set("plugins.crypto.secrets.default-max-aad-bytes", forge::config::core::value{4U});

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(runtime, plugin, document);

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->encrypt_aes_gcm(crypto_secrets::aead_encrypt_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "payload.encrypt",
                                                             .plaintext = bytes("payload"),
                                                             .aad = bytes("oversized-aad"),
                                                         })),
                     crypto_secrets::exceptions::size_limit_exceeded);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_decrypt_aes_gcm_maps_malformed_parameters) try {
   const auto key = std::string(32, 'K');
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(
       runtime, plugin,
       secrets_config({secret_entry("data-key", source_value(key), {"payload.decrypt"}, {"decrypt_aes_gcm"})}));

   auto aes_key = forge::crypto::symmetric::aes::make_aes256_key(
       std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
   const auto encrypted = forge::crypto::symmetric::aes::encrypt_aes256_gcm({
       .key = aes_key,
       .nonce = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
       .plaintext = bytes("payload"),
       .aad = bytes("aad"),
   });

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->decrypt_aes_gcm(crypto_secrets::aead_decrypt_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "payload.decrypt",
                                                             .nonce = {0, 1, 2},
                                                             .tag = encrypted.tag,
                                                             .ciphertext = encrypted.ciphertext,
                                                             .aad = bytes("aad"),
                                                         })),
                     crypto_secrets::exceptions::invalid_secret);

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->decrypt_aes_gcm(crypto_secrets::aead_decrypt_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "payload.decrypt",
                                                             .nonce = encrypted.nonce,
                                                             .tag = {0, 1, 2},
                                                             .ciphertext = encrypted.ciphertext,
                                                             .aad = bytes("aad"),
                                                         })),
                     crypto_secrets::exceptions::invalid_secret);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_decrypt_aes_gcm_rejects_oversized_aad_before_crypto) try {
   const auto key = std::string(32, 'K');
   auto document =
       secrets_config({secret_entry("data-key", source_value(key), {"payload.decrypt"}, {"decrypt_aes_gcm"})});
   document.set("plugins.crypto.secrets.default-max-aad-bytes", forge::config::core::value{4U});

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(runtime, plugin, document);

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->decrypt_aes_gcm(crypto_secrets::aead_decrypt_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "payload.decrypt",
                                                             .nonce = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
                                                             .tag = std::vector<std::uint8_t>(
                                                                 forge::crypto::symmetric::aes::aes_gcm_tag_size, 0),
                                                             .ciphertext = bytes("ciphertext"),
                                                             .aad = bytes("oversized-aad"),
                                                         })),
                     crypto_secrets::exceptions::size_limit_exceeded);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_decrypt_aes_gcm_rejects_oversized_plaintext_before_crypto) try {
   const auto key = std::string(32, 'K');
   auto document =
       secrets_config({secret_entry("data-key", source_value(key), {"payload.decrypt"}, {"decrypt_aes_gcm"})});
   document.set("plugins.crypto.secrets.default-max-plaintext-bytes", forge::config::core::value{32U});

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(runtime, plugin, document);

   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, api->decrypt_aes_gcm(crypto_secrets::aead_decrypt_request{
                        .secret_id = "data-key",
                        .purpose = "payload.decrypt",
                        .nonce = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
                        .tag = std::vector<std::uint8_t>(forge::crypto::symmetric::aes::aes_gcm_tag_size, 0),
                        .ciphertext = forge::crypto::core::bytes(33U, std::uint8_t{0}),
                    })),
       crypto_secrets::exceptions::size_limit_exceeded);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_decrypt_aes_gcm_keeps_authentication_failure_typed) try {
   const auto key = std::string(32, 'K');
   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(
       runtime, plugin,
       secrets_config({secret_entry("data-key", source_value(key), {"payload.decrypt"}, {"decrypt_aes_gcm"})}));

   auto aes_key = forge::crypto::symmetric::aes::make_aes256_key(
       std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
   auto encrypted = forge::crypto::symmetric::aes::encrypt_aes256_gcm({
       .key = aes_key,
       .nonce = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
       .plaintext = bytes("payload"),
       .aad = bytes("aad"),
   });
   encrypted.tag.front() ^= 0xffU;

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, api->decrypt_aes_gcm(crypto_secrets::aead_decrypt_request{
                                                             .secret_id = "data-key",
                                                             .purpose = "payload.decrypt",
                                                             .nonce = encrypted.nonce,
                                                             .tag = encrypted.tag,
                                                             .ciphertext = encrypted.ciphertext,
                                                             .aad = bytes("aad"),
                                                         })),
                     crypto_secrets::exceptions::crypto_failed);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_encrypted_file_roundtrips_and_rejects_wrong_passphrase) try {
   const auto path = std::filesystem::temp_directory_path() / "forge-crypto-secrets-encrypted-source.bin";
   const auto container = encrypted_container(bytes("encrypted-secret"), "correct horse battery staple");
   write_secret_file(path, container);

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(
       runtime, plugin,
       secrets_config({secret_entry("encrypted", source_encrypted_file(path, "correct horse battery staple"),
                                    {"payload.decrypt"}, {"get_bytes"}, true)}));
   const auto result = forge::asio::blocking::run(runtime, api->get_bytes(crypto_secrets::get_request{
                                                               .secret_id = "encrypted",
                                                               .purpose = "payload.decrypt",
                                                           }));
   BOOST_TEST(result.bytes == bytes("encrypted-secret"));

   auto wrong = crypto_secrets::plugin{};
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, wrong.configure(forge::config::core::component_view{
                        secrets_config({secret_entry("encrypted", source_encrypted_file(path, "wrong passphrase"),
                                                     {"payload.decrypt"}, {"get_bytes"}, true)}),
                        crypto_secrets_section})),
       crypto_secrets::exceptions::invalid_secret);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_encrypted_file_plaintext_limit_is_size_limit_exceeded) try {
   const auto path = std::filesystem::temp_directory_path() / "forge-crypto-secrets-encrypted-source-too-large.bin";
   const auto container = encrypted_container(bytes("encrypted-secret"), "correct horse battery staple");
   write_secret_file(path, container);

   auto document =
       secrets_config({secret_entry("encrypted", source_encrypted_file(path, "correct horse battery staple"),
                                    {"payload.decrypt"}, {"get_bytes"}, true)});
   document.set("plugins.crypto.secrets.default-max-plaintext-bytes", forge::config::core::value{4U});

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                             document, crypto_secrets_section})),
                     crypto_secrets::exceptions::size_limit_exceeded);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_encrypted_file_default_ciphertext_limit_allows_max_plaintext) try {
   const auto path = std::filesystem::temp_directory_path() / "forge-crypto-secrets-encrypted-source-default-limit.bin";
   const auto passphrase = std::string{"correct horse battery staple"};
   const auto plaintext = forge::crypto::core::bytes(crypto_secrets::default_max_plaintext_bytes, std::uint8_t{'x'});
   const auto container = encrypted_container(plaintext, passphrase);
   BOOST_TEST(container.size() > crypto_secrets::default_max_plaintext_bytes);
   BOOST_TEST(container.size() <= crypto_secrets::default_max_ciphertext_bytes);
   write_secret_file(path, container);

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   auto api = configured_api(runtime, plugin,
                             secrets_config({secret_entry("encrypted", source_encrypted_file(path, passphrase),
                                                          {"payload.decrypt"}, {"get_bytes"}, true)}));

   const auto exported = forge::asio::blocking::run(runtime, api->get_bytes(crypto_secrets::get_request{
                                                                 .secret_id = "encrypted",
                                                                 .purpose = "payload.decrypt",
                                                             }));
   BOOST_TEST(exported.bytes == plaintext);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_encrypted_file_missing_passphrase_file_is_invalid_source) try {
   const auto path = std::filesystem::temp_directory_path() / "forge-crypto-secrets-encrypted-missing-passphrase.bin";
   const auto missing_passphrase =
       std::filesystem::temp_directory_path() / "forge-crypto-secrets-missing-passphrase-file.txt";
   std::filesystem::remove(missing_passphrase);
   const auto container = encrypted_container(bytes("encrypted-secret"), "correct horse battery staple");
   write_secret_file(path, container);

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, plugin.configure(forge::config::core::component_view{
                        secrets_config({secret_entry(
                            "encrypted", source_encrypted_file_with_passphrase_file(path, missing_passphrase),
                            {"payload.decrypt"}, {"get_bytes"}, true)}),
                        crypto_secrets_section})),
       crypto_secrets::exceptions::invalid_source);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_config_rejects_encrypted_file_scrypt_params_above_configured_ceilings) try {
   const auto path = std::filesystem::temp_directory_path() / "forge-crypto-secrets-encrypted-source-high-scrypt.bin";
   const auto container = encrypted_container(bytes("encrypted-secret"), "correct horse battery staple");
   write_secret_file(path, container);

   auto document =
       secrets_config({secret_entry("encrypted", source_encrypted_file(path, "correct horse battery staple"),
                                    {"payload.decrypt"}, {"get_bytes"}, true)});
   document.set("plugins.crypto.secrets.encrypted-file-max-scrypt-n", forge::config::core::value{512U});

   auto plugin = crypto_secrets::plugin{};
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                             document, crypto_secrets_section})),
                     crypto_secrets::exceptions::invalid_secret);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(crypto_secrets_config_value_can_be_delivered_by_forge_env_source_adapter) try {
   auto plugin = crypto_secrets::plugin{};
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   auto registry = forge::config::core::component_registry{};
   registry.add(*descriptor);

   const auto document = secrets_config(
       {secret_entry("from-env-adapter", source_value("env-secret"), {"payload.decrypt"}, {"get_bytes"}, true)});
   const auto written = forge::config::env::write_document(document, registry, {.prefix = "FORGE"});
   BOOST_TEST(written.ok());
   BOOST_TEST(written.text.find("env-secret") == std::string::npos);
   BOOST_TEST(written.text.find("FORGE_SECRET_PROVIDER_SECRETS") == std::string::npos);

   const auto help = forge::config::program_options::help(registry, "FORGE options");
   BOOST_TEST(help.find("plugins.crypto.secrets.secrets") == std::string::npos);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
