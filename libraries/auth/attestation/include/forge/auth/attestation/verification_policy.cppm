module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

export module forge.auth.attestation.verification_policy;

export import forge.auth.attestation.exceptions;

export namespace forge::auth::attestation {

inline constexpr std::int64_t profile_version = 1;
inline constexpr std::string_view token_type = "forge-attestation+jwt";
inline constexpr std::string_view signing_algorithm = "ES256";
inline constexpr std::int64_t max_numeric_date = 253402300799;
inline constexpr std::size_t max_compact_bytes = 16384;
inline constexpr std::size_t max_header_bytes = 512;
inline constexpr std::size_t max_payload_bytes = 8192;
inline constexpr std::size_t max_json_depth = 8;
inline constexpr std::size_t max_json_nodes = 512;
inline constexpr std::size_t max_container_size = 128;
inline constexpr std::size_t max_string_bytes = 2048;
inline constexpr std::size_t max_identifier_bytes = 256;
inline constexpr std::size_t max_purpose_bytes = 64;
inline constexpr std::size_t document_id_bytes = 66;

struct verification_policy {
   std::string issuer;
   std::string audience;
   std::string purpose;
   std::string schema;
   std::int64_t version = profile_version;
   std::chrono::seconds max_lifetime{};
   std::chrono::seconds clock_skew{};

   void validate() const;
};

namespace detail {

inline constexpr auto uri_identifier_pattern =
    "^[A-Za-z][A-Za-z0-9+.-]*:[!-~]*$";

[[nodiscard]] bool valid_uri_identifier(std::string_view value) noexcept;
[[nodiscard]] bool valid_purpose(std::string_view value) noexcept;
[[nodiscard]] bool valid_document_id(std::string_view value) noexcept;

} // namespace detail

} // namespace forge::auth::attestation
