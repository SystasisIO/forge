module;

#include <forge/exceptions/policy.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

module forge.auth.attestation.document;

namespace forge::auth::attestation::detail {

void validate_document_fields(std::string_view iss, std::string_view aud, std::string_view jti,
                              std::int64_t iat, std::int64_t nbf, std::int64_t exp, std::int64_t version,
                              std::string_view purpose, std::string_view schema,
                              const verification_policy& expected, std::chrono::sys_seconds now) {
   expected.validate();
   if (!valid_uri_identifier(iss) || !valid_uri_identifier(aud) || !valid_document_id(jti) ||
       !valid_purpose(purpose) || !valid_uri_identifier(schema)) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_document, "attestation document identifiers are invalid");
   }
   if (version != profile_version) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::unsupported_version, "attestation document version is unsupported");
   }
   if (iss != expected.issuer || aud != expected.audience || purpose != expected.purpose || schema != expected.schema ||
       version != expected.version) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::profile_mismatch, "attestation document does not match expected profile");
   }
   if (iat < 0 || nbf < 0 || exp < 0 || iat > max_numeric_date || nbf > max_numeric_date || exp > max_numeric_date ||
       iat > nbf || nbf >= exp) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_document, "attestation validity interval is invalid");
   }
   // Subtraction is safe only after each timestamp has passed its bounded wire range.
   if (exp - iat > expected.max_lifetime.count()) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_document, "attestation lifetime exceeds expected profile");
   }
   const auto current = now.time_since_epoch().count();
   if (current < 0) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_profile, "attestation clock is outside supported range");
   }
   if (current >= exp) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::expired, "attestation document has expired");
   }
   // Skew admits bounded future issuance/activation, never extra time after expiry.
   if ((iat > current && iat - current > expected.clock_skew.count()) ||
       (nbf > current && nbf - current > expected.clock_skew.count())) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::not_yet_valid, "attestation document is not yet valid");
   }
}

} // namespace forge::auth::attestation::detail
