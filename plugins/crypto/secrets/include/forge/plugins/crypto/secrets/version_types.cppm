module;

#include <boost/describe.hpp>

#include <cstdint>
#include <string>
#include <vector>

export module forge.plugins.crypto.secrets.version_types;

export import forge.crypto.core.secret_bytes;
export import forge.plugins.crypto.secrets.types;
import forge.schema.object;

export namespace forge::plugins::crypto::secrets {

enum class version_state {
   available,
   revoked,
};

enum class version_open_mode {
   open_existing,
   create_new,
};

struct version_ref {
   std::string secret_id;
   std::uint64_t version = 0;

   bool operator==(const version_ref&) const = default;
};

struct version_policy {
   std::vector<std::string> purposes;
   std::vector<operation> operations;
   bool allow_raw_export = false;
   std::uint64_t max_plaintext_bytes = 0;
   std::uint64_t max_ciphertext_bytes = 0;
   std::uint64_t max_aad_bytes = 0;

   bool operator==(const version_policy&) const = default;
};

struct create_version_request {
   std::string mutation_id;
   std::string secret_id;
   std::uint64_t expected_revision = 0;
   secret_kind kind = secret_kind::bytes;
   version_policy policy;
   forge::crypto::core::secret_bytes material;
};

struct revoke_version_request {
   std::string mutation_id;
   version_ref ref;
   std::uint64_t expected_revision = 0;
};

struct version_mutation_result {
   version_ref ref;
   std::uint64_t revision = 0;

   bool operator==(const version_mutation_result&) const = default;
};

struct version_metadata {
   version_ref ref;
   std::uint64_t revision = 0;
   secret_kind kind = secret_kind::bytes;
   version_state state = version_state::available;
   version_policy policy;
};

struct version_query {
   version_ref ref;
};

struct version_list_request {
   std::string secret_id;
   std::uint64_t after_version = 0;
   std::uint32_t limit = 32;
};

struct version_page {
   std::vector<version_metadata> versions;
   std::uint64_t next_after_version = 0;
};

struct version_snapshot {
   bool enabled = false;
   bool ready = false;
   bool stopping = false;
   bool fault = false;
   std::uint64_t versions = 0;
   std::uint64_t receipts = 0;
};

struct version_get_request {
   std::uint64_t version = 0;
   get_request request;
};

struct version_get_result {
   std::uint64_t version = 0;
   get_result result;
};

struct version_derive_request {
   std::uint64_t version = 0;
   derive_request request;
};

struct version_derive_result {
   std::uint64_t version = 0;
   derive_result result;
};

struct version_aead_encrypt_request {
   std::uint64_t version = 0;
   aead_encrypt_request request;
};

struct version_aead_encrypt_result {
   std::uint64_t version = 0;
   aead_encrypt_result result;
};

struct version_aead_decrypt_request {
   std::uint64_t version = 0;
   aead_decrypt_request request;
};

struct version_aead_decrypt_result {
   std::uint64_t version = 0;
   aead_decrypt_result result;
};

struct version_store_config {
   bool enabled = false;
   version_open_mode mode = version_open_mode::open_existing;
   std::string path;
   std::string bootstrap_passphrase_file;
   version_policy ceiling{
       .max_plaintext_bytes = default_max_plaintext_bytes,
       .max_ciphertext_bytes = default_max_ciphertext_bytes,
       .max_aad_bytes = default_max_aad_bytes,
   };
   std::uint64_t max_secret_ids = 256;
   std::uint64_t max_versions = 1'024;
   std::uint64_t max_versions_per_secret = 32;
   std::uint64_t max_material_bytes = 64U * 1024U;
   std::uint64_t max_total_material_bytes = 4U * 1024U * 1024U;
   std::uint64_t max_store_plaintext_bytes = 8U * 1024U * 1024U;
   std::uint64_t max_pending_calls = 16;
   std::uint64_t max_pending_bytes = 1U * 1024U * 1024U;
};

[[nodiscard]] constexpr bool diagnostic_is_secret(const create_version_request&) noexcept {
   return true;
}

[[nodiscard]] constexpr bool diagnostic_is_secret(const revoke_version_request&) noexcept {
   return true;
}

[[nodiscard]] constexpr bool diagnostic_is_secret(const version_get_result&) noexcept {
   return true;
}

[[nodiscard]] constexpr bool diagnostic_is_secret(const version_derive_result&) noexcept {
   return true;
}

[[nodiscard]] constexpr bool diagnostic_is_secret(const version_aead_encrypt_request&) noexcept {
   return true;
}

[[nodiscard]] constexpr bool diagnostic_is_secret(const version_aead_decrypt_result&) noexcept {
   return true;
}

BOOST_DESCRIBE_ENUM(version_state, available, revoked)
BOOST_DESCRIBE_ENUM(version_open_mode, open_existing, create_new)
BOOST_DESCRIBE_STRUCT(version_ref, (), (secret_id, version))
BOOST_DESCRIBE_STRUCT(version_policy, (),
                      (purposes, operations, allow_raw_export, max_plaintext_bytes, max_ciphertext_bytes,
                       max_aad_bytes))
BOOST_DESCRIBE_STRUCT(create_version_request, (), (mutation_id, secret_id, expected_revision, kind, policy, material))
BOOST_DESCRIBE_STRUCT(revoke_version_request, (), (mutation_id, ref, expected_revision))
BOOST_DESCRIBE_STRUCT(version_mutation_result, (), (ref, revision))
BOOST_DESCRIBE_STRUCT(version_metadata, (), (ref, revision, kind, state, policy))
BOOST_DESCRIBE_STRUCT(version_query, (), (ref))
BOOST_DESCRIBE_STRUCT(version_list_request, (), (secret_id, after_version, limit))
BOOST_DESCRIBE_STRUCT(version_page, (), (versions, next_after_version))
BOOST_DESCRIBE_STRUCT(version_snapshot, (), (enabled, ready, stopping, fault, versions, receipts))
BOOST_DESCRIBE_STRUCT(version_get_request, (), (version, request))
BOOST_DESCRIBE_STRUCT(version_get_result, (), (version, result))
BOOST_DESCRIBE_STRUCT(version_derive_request, (), (version, request))
BOOST_DESCRIBE_STRUCT(version_derive_result, (), (version, result))
BOOST_DESCRIBE_STRUCT(version_aead_encrypt_request, (), (version, request))
BOOST_DESCRIBE_STRUCT(version_aead_encrypt_result, (), (version, result))
BOOST_DESCRIBE_STRUCT(version_aead_decrypt_request, (), (version, request))
BOOST_DESCRIBE_STRUCT(version_aead_decrypt_result, (), (version, result))
BOOST_DESCRIBE_STRUCT(version_store_config, (),
                      (enabled, mode, path, bootstrap_passphrase_file, ceiling, max_secret_ids, max_versions,
                       max_versions_per_secret, max_material_bytes, max_total_material_bytes, max_store_plaintext_bytes,
                       max_pending_calls, max_pending_bytes))

} // namespace forge::plugins::crypto::secrets

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_policy> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_policy> define() {
      using type = forge::plugins::crypto::secrets::version_policy;
      auto schema = forge::schema::object<type>();
      schema.field<&type::purposes>("purposes").max_items(32).each_non_empty();
      schema.field<&type::operations>("operations").max_items(4);
      schema.field<&type::allow_raw_export>("allow-raw-export");
      schema.field<&type::max_plaintext_bytes>("max-plaintext-bytes")
          .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
      schema.field<&type::max_ciphertext_bytes>("max-ciphertext-bytes")
          .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
      schema.field<&type::max_aad_bytes>("max-aad-bytes")
          .range(0, forge::plugins::crypto::secrets::aes_update_bytes_ceiling);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_store_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_store_config> define() {
      using type = forge::plugins::crypto::secrets::version_store_config;
      auto schema = forge::schema::object<type>();
      schema.field<&type::enabled>("enabled");
      schema.field<&type::mode>("mode");
      schema.field<&type::path>("path").secret();
      schema.field<&type::bootstrap_passphrase_file>("bootstrap-passphrase-file").secret();
      schema.field<&type::ceiling>("ceiling");
      schema.field<&type::max_secret_ids>("max-secret-ids").range(1, 1024);
      schema.field<&type::max_versions>("max-versions").range(1, 4096);
      schema.field<&type::max_versions_per_secret>("max-versions-per-secret").range(1, 256);
      schema.field<&type::max_material_bytes>("max-material-bytes").range(1, 1048576);
      schema.field<&type::max_total_material_bytes>("max-total-material-bytes").range(1, 33554432);
      schema.field<&type::max_store_plaintext_bytes>("max-store-plaintext-bytes").range(1, 67108864);
      schema.field<&type::max_pending_calls>("max-pending-calls").range(1, 64);
      schema.field<&type::max_pending_bytes>("max-pending-bytes").range(1, 8388608);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_get_request> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_get_request> define() {
      using type = forge::plugins::crypto::secrets::version_get_request;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::request>("request").secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_get_result> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_get_result> define() {
      using type = forge::plugins::crypto::secrets::version_get_result;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::result>("result").secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_derive_request> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_derive_request> define() {
      using type = forge::plugins::crypto::secrets::version_derive_request;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::request>("request").secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_derive_result> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_derive_result> define() {
      using type = forge::plugins::crypto::secrets::version_derive_result;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::result>("result").secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_aead_encrypt_request> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_encrypt_request>
   define() {
      using type = forge::plugins::crypto::secrets::version_aead_encrypt_request;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::request>("request").secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_aead_encrypt_result> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_encrypt_result>
   define() {
      using type = forge::plugins::crypto::secrets::version_aead_encrypt_result;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::result>("result").secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_aead_decrypt_request> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_decrypt_request>
   define() {
      using type = forge::plugins::crypto::secrets::version_aead_decrypt_request;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::request>("request").secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::version_aead_decrypt_result> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::version_aead_decrypt_result>
   define() {
      using type = forge::plugins::crypto::secrets::version_aead_decrypt_result;
      auto schema = forge::schema::object<type>();
      schema.field<&type::version>("version");
      schema.field<&type::result>("result").secret();
      return schema;
   }
};
