module;

#include <boost/describe.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

export module forge.plugins.crypto.secrets.types;

import forge.crypto.core.types;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

export namespace forge::plugins::crypto::secrets {

inline constexpr auto default_max_plaintext_bytes = std::uint64_t{1'048'576};
inline constexpr auto aes_update_bytes_ceiling = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
inline constexpr auto max_aad_bytes_ceiling = aes_update_bytes_ceiling;
inline constexpr auto default_max_aad_bytes = std::uint64_t{1'048'576};
inline constexpr auto encrypted_file_v1_magic_bytes = std::uint64_t{8};
inline constexpr auto encrypted_file_v1_u64_fields = std::uint64_t{7};
inline constexpr auto encrypted_file_default_salt_bytes = std::uint64_t{16};
inline constexpr auto encrypted_file_default_nonce_bytes = std::uint64_t{12};
inline constexpr auto encrypted_file_aes_gcm_tag_bytes = std::uint64_t{16};
inline constexpr auto encrypted_file_default_container_overhead_bytes =
    encrypted_file_v1_magic_bytes + encrypted_file_v1_u64_fields * std::uint64_t{8} +
    encrypted_file_default_salt_bytes + encrypted_file_default_nonce_bytes + encrypted_file_aes_gcm_tag_bytes;
inline constexpr auto default_max_ciphertext_bytes =
    default_max_plaintext_bytes + encrypted_file_default_container_overhead_bytes;
inline constexpr auto default_scrypt_n = std::uint64_t{16'384};
inline constexpr auto default_scrypt_r = std::uint64_t{8};
inline constexpr auto default_scrypt_p = std::uint64_t{1};
inline constexpr auto default_scrypt_max_memory_bytes = std::uint64_t{32ULL * 1024ULL * 1024ULL};
inline constexpr auto default_encrypted_file_max_scrypt_n = default_scrypt_n;
inline constexpr auto default_encrypted_file_max_scrypt_r = default_scrypt_r;
inline constexpr auto default_encrypted_file_max_scrypt_p = default_scrypt_p;
inline constexpr auto default_encrypted_file_max_scrypt_memory_bytes = default_scrypt_max_memory_bytes;

enum class secret_kind {
   bytes,
   symmetric_key,
};

enum class source_type {
   value,
   file,
   encrypted_file,
};

enum class encoding {
   raw,
   hex,
   base64,
};

enum class operation {
   get_bytes,
   derive_hkdf_sha256,
   encrypt_aes_gcm,
   decrypt_aes_gcm,
};

struct secret_source {
   source_type type = source_type::value;
   encoding encoding = encoding::raw;
   std::string value;
   std::string path;
   std::string passphrase_value;
   std::string passphrase_file;
};

struct secret_entry {
   std::string id;
   secret_kind kind = secret_kind::symmetric_key;
   secret_source source;
   std::vector<std::string> purposes;
   std::vector<operation> operations;
   bool allow_raw_export = false;
   std::uint64_t max_plaintext_bytes = 0;
   std::uint64_t max_ciphertext_bytes = 0;
   std::uint64_t max_aad_bytes = 0;
};

struct config {
   std::vector<secret_entry> secrets;
   std::uint64_t default_max_plaintext_bytes = forge::plugins::crypto::secrets::default_max_plaintext_bytes;
   std::uint64_t default_max_ciphertext_bytes = forge::plugins::crypto::secrets::default_max_ciphertext_bytes;
   std::uint64_t default_max_aad_bytes = forge::plugins::crypto::secrets::default_max_aad_bytes;
   std::uint64_t encrypted_file_max_scrypt_n = forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_n;
   std::uint64_t encrypted_file_max_scrypt_r = forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_r;
   std::uint64_t encrypted_file_max_scrypt_p = forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_p;
   std::uint64_t encrypted_file_max_scrypt_memory_bytes =
       forge::plugins::crypto::secrets::default_encrypted_file_max_scrypt_memory_bytes;
};

struct query {};

struct secret_summary {
   std::string id;
   secret_kind kind = secret_kind::symmetric_key;
   std::vector<std::string> purposes;
   std::vector<operation> operations;
   bool allow_raw_export = false;
};

struct snapshot {
   std::uint64_t configured_secrets = 0;
   bool stopping = false;
   std::vector<secret_summary> secrets;
};

struct get_request {
   std::string secret_id;
   std::string purpose;
};

struct get_result {
   std::string secret_id;
   forge::crypto::core::bytes bytes;
};

struct derive_request {
   std::string secret_id;
   std::string purpose;
   forge::crypto::core::bytes salt;
   forge::crypto::core::bytes info;
   std::uint64_t output_size = 32;
};

struct derive_result {
   std::string secret_id;
   forge::crypto::core::bytes bytes;
};

struct aead_encrypt_request {
   std::string secret_id;
   std::string purpose;
   forge::crypto::core::bytes nonce;
   forge::crypto::core::bytes plaintext;
   forge::crypto::core::bytes aad;
};

struct aead_encrypt_result {
   std::string secret_id;
   forge::crypto::core::bytes nonce;
   forge::crypto::core::bytes tag;
   forge::crypto::core::bytes ciphertext;
};

struct aead_decrypt_request {
   std::string secret_id;
   std::string purpose;
   forge::crypto::core::bytes nonce;
   forge::crypto::core::bytes tag;
   forge::crypto::core::bytes ciphertext;
   forge::crypto::core::bytes aad;
};

struct aead_decrypt_result {
   std::string secret_id;
   forge::crypto::core::bytes plaintext;
};

BOOST_DESCRIBE_ENUM(secret_kind, bytes, symmetric_key)
BOOST_DESCRIBE_ENUM(source_type, value, file, encrypted_file)
BOOST_DESCRIBE_ENUM(encoding, raw, hex, base64)
BOOST_DESCRIBE_ENUM(operation, get_bytes, derive_hkdf_sha256, encrypt_aes_gcm, decrypt_aes_gcm)
BOOST_DESCRIBE_STRUCT(secret_source, (), (type, encoding, value, path, passphrase_value, passphrase_file))
BOOST_DESCRIBE_STRUCT(secret_entry, (),
                      (id, kind, source, purposes, operations, allow_raw_export, max_plaintext_bytes,
                       max_ciphertext_bytes, max_aad_bytes))
BOOST_DESCRIBE_STRUCT(config, (),
                      (secrets, default_max_plaintext_bytes, default_max_ciphertext_bytes, default_max_aad_bytes,
                       encrypted_file_max_scrypt_n, encrypted_file_max_scrypt_r, encrypted_file_max_scrypt_p,
                       encrypted_file_max_scrypt_memory_bytes))
BOOST_DESCRIBE_STRUCT(query, (), ())
BOOST_DESCRIBE_STRUCT(secret_summary, (), (id, kind, purposes, operations, allow_raw_export))
BOOST_DESCRIBE_STRUCT(snapshot, (), (configured_secrets, stopping, secrets))
BOOST_DESCRIBE_STRUCT(get_request, (), (secret_id, purpose))
BOOST_DESCRIBE_STRUCT(get_result, (), (secret_id, bytes))
BOOST_DESCRIBE_STRUCT(derive_request, (), (secret_id, purpose, salt, info, output_size))
BOOST_DESCRIBE_STRUCT(derive_result, (), (secret_id, bytes))
BOOST_DESCRIBE_STRUCT(aead_encrypt_request, (), (secret_id, purpose, nonce, plaintext, aad))
BOOST_DESCRIBE_STRUCT(aead_encrypt_result, (), (secret_id, nonce, tag, ciphertext))
BOOST_DESCRIBE_STRUCT(aead_decrypt_request, (), (secret_id, purpose, nonce, tag, ciphertext, aad))
BOOST_DESCRIBE_STRUCT(aead_decrypt_result, (), (secret_id, plaintext))
} // namespace forge::plugins::crypto::secrets

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::secret_source> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::secret_source> define();
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::secret_entry> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::secret_entry> define();
};

export template <> struct forge::schema::rules<forge::plugins::crypto::secrets::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::crypto::secrets::config> define();
};
