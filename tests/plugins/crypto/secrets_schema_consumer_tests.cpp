#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <any>
#include <array>
#include <string>

import forge.plugins.crypto.secrets.types;
import forge.plugins.crypto.secrets.version_types;
import forge.schema.object;

namespace forge_secrets_schema_consumer_tests {

struct large_field {
   std::string prefix;
   std::string suffix;
};

static_assert(sizeof(large_field) > sizeof(std::any));

struct consumer_config {
   large_field nested;
};

BOOST_DESCRIBE_STRUCT(large_field, (), (prefix, suffix))
BOOST_DESCRIBE_STRUCT(consumer_config, (), (nested))

} // namespace forge_secrets_schema_consumer_tests

namespace crypto_secrets = forge::plugins::crypto::secrets;

BOOST_AUTO_TEST_SUITE(crypto_secrets_schema_consumer_tests)

// These instantiations belong to the importing consumer. In particular, the
// large std::any value exercises libc++'s allocation/exception guard path.
BOOST_AUTO_TEST_CASE(secrets_modules_allow_consumer_large_any_and_nested_schema) {
   using namespace forge_secrets_schema_consumer_tests;
   auto bytes = std::array<char, 64>{};
   bytes.front() = 'a';
   bytes.back() = 'z';

   const auto boxed = std::any{bytes};
   const auto copied = std::any_cast<std::array<char, 64>>(boxed);
   BOOST_TEST(copied.front() == 'a');
   BOOST_TEST(copied.back() == 'z');

   auto value = consumer_config{.nested = {.prefix = "first", .suffix = std::string(64, 'z')}};
   auto nested_schema = forge::schema::object<large_field>();
   nested_schema.field<&large_field::prefix>("prefix").description("Consumer-owned record prefix");
   nested_schema.field<&large_field::suffix>("suffix").description("Consumer-owned record suffix");
   auto schema = forge::schema::object<consumer_config>();
   schema.field<&consumer_config::nested>("nested").description("Consumer-owned nested record");

   const auto nested_suffix = nested_schema.fields()[1].read_any(value.nested);
   const auto stored_suffix = std::any_cast<std::string>(nested_suffix);
   BOOST_TEST(stored_suffix == value.nested.suffix);
   const auto nested_record = schema.fields().front().read_validation_any(value);
   BOOST_REQUIRE(nested_record.has_value());
   auto restored = consumer_config{};
   schema.fields().front().assign_any(restored, *nested_record);
   BOOST_TEST(restored.nested.prefix == value.nested.prefix);
   BOOST_TEST(restored.nested.suffix == value.nested.suffix);
}

BOOST_AUTO_TEST_CASE(exported_secrets_rules_keep_defaults_validation_and_redaction) {
   const auto source = forge::schema::rules<crypto_secrets::secret_source>::define();
   BOOST_TEST(source.fields()[2].secret);
   BOOST_TEST(source.fields()[4].secret);
   const auto entry = forge::schema::rules<crypto_secrets::secret_entry>::define();
   BOOST_TEST(entry.fields()[2].secret);

   const auto schema = forge::schema::rules<crypto_secrets::config>::define();
   auto config = crypto_secrets::config{};
   config.default_max_plaintext_bytes = 0;
   config.default_max_ciphertext_bytes = 0;
   config.default_max_aad_bytes = 0;
   schema.apply_defaults(config);
   BOOST_TEST(config.default_max_plaintext_bytes == crypto_secrets::default_max_plaintext_bytes);
   BOOST_TEST(config.default_max_ciphertext_bytes == crypto_secrets::default_max_ciphertext_bytes);
   BOOST_TEST(config.default_max_aad_bytes == crypto_secrets::default_max_aad_bytes);
   BOOST_TEST(schema.validate(config, "secrets").empty());
   config.default_max_plaintext_bytes = 0;
   const auto diagnostics = schema.validate(config, "secrets");
   BOOST_REQUIRE_EQUAL(diagnostics.size(), 1U);
   BOOST_TEST(diagnostics.front().path == "secrets.default-max-plaintext-bytes");
   BOOST_TEST(diagnostics.front().code == "schema.range");

   const auto policy = forge::schema::rules<crypto_secrets::version_policy>::define();
   auto invalid_policy = crypto_secrets::version_policy{};
   invalid_policy.max_plaintext_bytes = crypto_secrets::aes_update_bytes_ceiling + 1;
   BOOST_TEST(!policy.validate(invalid_policy, "policy").empty());
   const auto store = forge::schema::rules<crypto_secrets::version_store_config>::define();
   auto invalid_store = crypto_secrets::version_store_config{};
   invalid_store.max_versions = 0;
   BOOST_TEST(!store.validate(invalid_store, "store").empty());

   BOOST_TEST(forge::schema::rules<crypto_secrets::version_get_request>::define().fields()[1].secret);
   BOOST_TEST(forge::schema::rules<crypto_secrets::version_get_result>::define().fields()[1].secret);
   BOOST_TEST(forge::schema::rules<crypto_secrets::version_derive_request>::define().fields()[1].secret);
   BOOST_TEST(forge::schema::rules<crypto_secrets::version_derive_result>::define().fields()[1].secret);
   BOOST_TEST(forge::schema::rules<crypto_secrets::version_aead_encrypt_request>::define().fields()[1].secret);
   BOOST_TEST(forge::schema::rules<crypto_secrets::version_aead_encrypt_result>::define().fields()[1].secret);
   BOOST_TEST(forge::schema::rules<crypto_secrets::version_aead_decrypt_request>::define().fields()[1].secret);
   BOOST_TEST(forge::schema::rules<crypto_secrets::version_aead_decrypt_result>::define().fields()[1].secret);
}

BOOST_AUTO_TEST_SUITE_END()
