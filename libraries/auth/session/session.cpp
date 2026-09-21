module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.auth.session.session;

import forge.auth.pairing.types;
import forge.codec.base64;
import forge.crypto.core.constant_time;
import forge.crypto.core.random;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.secret_string;
import forge.crypto.digest.sha256;
import forge.exceptions;

namespace forge::auth::session {
namespace {

constexpr auto secret_bytes = std::size_t{32};
constexpr auto secret_characters = std::size_t{43};

enum class time_input {
   options,
   record,
};

[[nodiscard]] std::span<const std::uint8_t> byte_view(std::string_view value) {
   return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

[[nodiscard]] forge::codec::base64::encode_options secret_encode_options() {
   return {
       .characters = forge::codec::base64::alphabet::url,
       .pad = forge::codec::base64::padding::omit,
   };
}

[[nodiscard]] forge::codec::base64::decode_options secret_decode_options() {
   return {
       .characters = forge::codec::base64::alphabet::url,
       .pad = forge::codec::base64::padding_policy::forbid,
       .ignore_ascii_whitespace = false,
   };
}

void require_identity(std::string_view identity) {
   if (identity.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session identity must not be empty");
   }
}

void require_credential_id(const forge::auth::pairing::credential_id& id) {
   if (id.value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session credential ID must not be empty");
   }
}

void require_canonical_scopes(const forge::auth::pairing::scope_set& scopes) {
   if (scopes.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session scopes must not be empty");
   }
   for (const auto& scope : scopes) {
      if (scope.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session scope must not be empty");
      }
   }
   if (!std::is_sorted(scopes.begin(), scopes.end()) ||
       std::adjacent_find(scopes.begin(), scopes.end()) != scopes.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session scopes must be canonical");
   }
}

void require_active_credential(const forge::auth::pairing::credential& credential, time_point now) {
   require_credential_id(credential.id);
   require_identity(credential.identity);
   require_canonical_scopes(credential.scopes);
   if (credential.generation == 0 || credential.updated_at < credential.issued_at || now < credential.updated_at) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "credential record is invalid for session");
   }
   if (credential.state == forge::auth::pairing::credential_state::revoked) {
      FORGE_THROW_EXCEPTION(exceptions::credential_revoked, "credential is revoked");
   }
   if (credential.state != forge::auth::pairing::credential_state::active || credential.revoked_at.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "credential record is invalid for session");
   }
}

[[noreturn]] void throw_invalid_time(time_input input) {
   if (input == time_input::options) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "session options contain an unsupported timestamp");
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session record contains an unsupported timestamp");
}

void require_supported_time(time_point value, time_input input) {
   if (value < time_point{}) {
      throw_invalid_time(input);
   }
}

[[nodiscard]] time_point canonical_idle_expiry(time_point last_activity_at, time_point absolute_expires_at,
                                               std::chrono::system_clock::duration idle_timeout, time_input input) {
   require_supported_time(last_activity_at, input);
   require_supported_time(absolute_expires_at, input);
   if (absolute_expires_at <= last_activity_at || idle_timeout <= std::chrono::system_clock::duration::zero()) {
      throw_invalid_time(input);
   }

   const auto remaining = absolute_expires_at - last_activity_at;
   const auto extension = std::min(idle_timeout, remaining);
   return last_activity_at + extension;
}

void require_session_record(const session_record& record) {
   if (!record.session_digest.empty() && !record.csrf_digest.empty() &&
       forge::crypto::core::constant_time_equal(record.session_digest.to_uint8_span(),
                                                record.csrf_digest.to_uint8_span())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session and CSRF digests must differ");
   }

   require_credential_id(record.credential_id);
   require_identity(record.identity);
   require_canonical_scopes(record.scopes);
   if (record.credential_generation == 0 || record.session_digest.empty() || record.csrf_digest.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session record is invalid");
   }
   require_supported_time(record.created_at, time_input::record);
   require_supported_time(record.last_activity_at, time_input::record);
   require_supported_time(record.idle_expires_at, time_input::record);
   require_supported_time(record.absolute_expires_at, time_input::record);
   if (record.terminal_at.has_value()) {
      require_supported_time(*record.terminal_at, time_input::record);
   }
   if (record.last_activity_at < record.created_at || record.absolute_expires_at <= record.created_at) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session record is invalid");
   }
   if (record.idle_expires_at != canonical_idle_expiry(record.last_activity_at, record.absolute_expires_at,
                                                       record.idle_timeout, time_input::record)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session record has a non-canonical idle expiry");
   }

   switch (record.state) {
   case session_state::active:
      if (record.terminal_at.has_value()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "active session has a terminal time");
      }
      break;
   case session_state::rotated:
   case session_state::revoked:
      if (!record.terminal_at.has_value() || *record.terminal_at < record.created_at ||
          *record.terminal_at < record.last_activity_at || *record.terminal_at >= record.idle_expires_at ||
          *record.terminal_at >= record.absolute_expires_at) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "terminal session has an invalid terminal time");
      }
      break;
   default:
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session record has an unknown state");
   }
}

void require_active_session(const session_record& record, time_point now) {
   require_session_record(record);
   if (now < record.created_at || now < record.last_activity_at ||
       (record.terminal_at.has_value() && now < *record.terminal_at)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session transition time regressed");
   }
   if (record.state == session_state::rotated) {
      FORGE_THROW_EXCEPTION(exceptions::replayed, "session was rotated");
   }
   if (record.state == session_state::revoked) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session is revoked");
   }
   if (now >= record.absolute_expires_at) {
      FORGE_THROW_EXCEPTION(exceptions::expired, "session expired");
   }
   if (now >= record.idle_expires_at) {
      FORGE_THROW_EXCEPTION(exceptions::idle_expired, "session idle timeout expired");
   }
}

void require_matching_credential(const session_record& record, const forge::auth::pairing::credential& credential,
                                 time_point now) {
   require_active_credential(credential, now);
   if (record.credential_id != credential.id || record.credential_generation != credential.generation) {
      FORGE_THROW_EXCEPTION(exceptions::credential_mismatch, "session credential binding does not match");
   }
   if (record.identity != credential.identity) {
      FORGE_THROW_EXCEPTION(exceptions::identity_mismatch, "session identity binding does not match");
   }
   if (record.scopes != credential.scopes) {
      FORGE_THROW_EXCEPTION(exceptions::scope_mismatch, "session scope binding does not match");
   }
}

void require_matching_credential(const device_grant_record& record, const forge::auth::pairing::credential& credential,
                                 time_point now) {
   require_active_credential(credential, now);
   if (record.credential_id != credential.id || record.credential_generation != credential.generation) {
      FORGE_THROW_EXCEPTION(exceptions::credential_mismatch, "device grant credential binding does not match");
   }
}

[[noreturn]] void throw_invalid_secret(bool csrf) {
   if (csrf) {
      FORGE_THROW_EXCEPTION(exceptions::csrf_invalid, "CSRF secret is invalid");
   }
   FORGE_THROW_EXCEPTION(exceptions::token_invalid, "session token is invalid");
}

[[nodiscard]] forge::crypto::digest::sha256 identify_secret(const forge::crypto::core::secret_string& presented,
                                                            bool csrf);

void verify_secret(const forge::crypto::core::secret_string& presented, const forge::crypto::digest::sha256& expected,
                   bool csrf) {
   const auto actual = identify_secret(presented, csrf);
   if (!forge::crypto::core::constant_time_equal(expected.to_uint8_span(), actual.to_uint8_span())) {
      throw_invalid_secret(csrf);
   }
}

void require_device_grant_record(const device_grant_record& record) {
   require_credential_id(record.credential_id);
   if (record.credential_generation == 0 || record.rotation_generation == 0 || record.token_digest.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "device grant record is invalid");
   }
   require_supported_time(record.issued_at, time_input::record);
   require_supported_time(record.last_activity_at, time_input::record);
   require_supported_time(record.idle_expires_at, time_input::record);
   require_supported_time(record.absolute_expires_at, time_input::record);
   if (record.terminal_at.has_value()) {
      require_supported_time(*record.terminal_at, time_input::record);
   }
   if (record.last_activity_at < record.issued_at || record.absolute_expires_at <= record.issued_at ||
       record.idle_expires_at != canonical_idle_expiry(record.last_activity_at, record.absolute_expires_at,
                                                       record.idle_timeout, time_input::record)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "device grant timestamps are invalid");
   }

   switch (record.state) {
   case device_grant_state::active:
      if (record.terminal_at.has_value()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "active device grant has a terminal time");
      }
      break;
   case device_grant_state::rotated:
   case device_grant_state::revoked:
      if (!record.terminal_at.has_value() || *record.terminal_at < record.last_activity_at ||
          *record.terminal_at >= record.idle_expires_at || *record.terminal_at >= record.absolute_expires_at) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "terminal device grant has an invalid terminal time");
      }
      break;
   default:
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "device grant record has an unknown state");
   }
}

void require_device_grant_lifecycle(const device_grant_record& record, time_point now) {
   if (now < record.last_activity_at || (record.terminal_at.has_value() && now < *record.terminal_at)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "device grant transition time regressed");
   }
   if (record.state == device_grant_state::rotated) {
      FORGE_THROW_EXCEPTION(exceptions::replayed, "device grant token was replayed after rotation");
   }
   if (record.state == device_grant_state::revoked) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "device grant is revoked");
   }
   if (now >= record.absolute_expires_at) {
      FORGE_THROW_EXCEPTION(exceptions::expired, "device grant expired");
   }
   if (now >= record.idle_expires_at) {
      FORGE_THROW_EXCEPTION(exceptions::idle_expired, "device grant idle timeout expired");
   }
}

[[nodiscard]] forge::crypto::digest::sha256 identify_secret(const forge::crypto::core::secret_string& presented,
                                                            bool csrf) {
   if (presented.size() != secret_characters) {
      throw_invalid_secret(csrf);
   }

   auto decoded = forge::crypto::core::secret_bytes{};
   try {
      decoded.assign(forge::codec::base64::decode(presented.view(), secret_decode_options()));
   } catch (const forge::exceptions::base&) {
      throw_invalid_secret(csrf);
   }
   if (decoded.size() != secret_bytes) {
      throw_invalid_secret(csrf);
   }
   const auto canonical =
       forge::crypto::core::secret_string{forge::codec::base64::encode(decoded.span(), secret_encode_options())};
   if (!forge::crypto::core::constant_time_equal(byte_view(presented.view()), byte_view(canonical.view()))) {
      throw_invalid_secret(csrf);
   }
   return forge::crypto::digest::sha256::hash(decoded.span());
}

} // namespace

session_issuance issue_session(const forge::auth::pairing::credential& credential, session_options options) {
   const auto initial_idle_expires_at =
       canonical_idle_expiry(options.now, options.absolute_expires_at, options.idle_timeout, time_input::options);
   require_active_credential(credential, options.now);

   auto session_material = forge::crypto::core::secret_bytes{forge::crypto::core::random_bytes(secret_bytes)};
   auto csrf_material = forge::crypto::core::secret_bytes{forge::crypto::core::random_bytes(secret_bytes)};
   if (forge::crypto::core::constant_time_equal(session_material.span(), csrf_material.span())) {
      FORGE_THROW_EXCEPTION(exceptions::secret_collision, "session and CSRF secret material collided");
   }
   const auto session_digest = forge::crypto::digest::sha256::hash(session_material.span());
   const auto csrf_digest = forge::crypto::digest::sha256::hash(csrf_material.span());
   auto session_token = forge::crypto::core::secret_string{
       forge::codec::base64::encode(session_material.span(), secret_encode_options())};
   auto csrf_secret =
       forge::crypto::core::secret_string{forge::codec::base64::encode(csrf_material.span(), secret_encode_options())};

   return {
       .record =
           {
               .session_digest = session_digest,
               .csrf_digest = csrf_digest,
               .credential_id = credential.id,
               .credential_generation = credential.generation,
               .identity = credential.identity,
               .scopes = credential.scopes,
               .created_at = options.now,
               .last_activity_at = options.now,
               .idle_expires_at = initial_idle_expires_at,
               .absolute_expires_at = options.absolute_expires_at,
               .idle_timeout = options.idle_timeout,
           },
       .session_token = std::move(session_token),
       .csrf_secret = std::move(csrf_secret),
   };
}

void validate_issuance(const session_issuance& issuance) {
   require_session_record(issuance.record);
   if (issuance.record.state != session_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "session issuance record must be active");
   }
   verify_secret(issuance.session_token, issuance.record.session_digest, false);
   verify_secret(issuance.csrf_secret, issuance.record.csrf_digest, true);
}

forge::crypto::digest::sha256 identify_session_token(const forge::crypto::core::secret_string& session_token) {
   return identify_secret(session_token, false);
}

principal validate_session(const session_record& record, const forge::crypto::core::secret_string& session_token,
                           const forge::auth::pairing::credential& credential, time_point now) {
   require_active_session(record, now);
   const auto actual = identify_session_token(session_token);
   if (!forge::crypto::core::constant_time_equal(record.session_digest.to_uint8_span(), actual.to_uint8_span())) {
      FORGE_THROW_EXCEPTION(exceptions::token_invalid, "session token is invalid");
   }
   require_matching_credential(record, credential, now);
   return {
       .credential_id = record.credential_id,
       .credential_generation = record.credential_generation,
       .identity = record.identity,
       .scopes = record.scopes,
   };
}

void verify_csrf_secret(const session_record& record, const forge::crypto::core::secret_string& csrf_secret,
                        time_point now) {
   require_active_session(record, now);
   verify_secret(csrf_secret, record.csrf_digest, true);
}

void renew_idle(session_record& record, time_point now) {
   require_active_session(record, now);
   const auto next_idle_expires_at =
       canonical_idle_expiry(now, record.absolute_expires_at, record.idle_timeout, time_input::record);
   record.last_activity_at = now;
   record.idle_expires_at = next_idle_expires_at;
}

session_issuance rotate_session(session_record& record, const forge::auth::pairing::credential& credential,
                                session_options options) {
   static_cast<void>(
       canonical_idle_expiry(options.now, options.absolute_expires_at, options.idle_timeout, time_input::options));
   require_active_session(record, options.now);
   require_matching_credential(record, credential, options.now);
   auto result = issue_session(credential, options);
   record.state = session_state::rotated;
   record.terminal_at = options.now;
   return result;
}

void logout_session(session_record& record, time_point now) {
   require_active_session(record, now);
   record.state = session_state::revoked;
   record.terminal_at = now;
}

void revoke_session(session_record& record, time_point now) {
   logout_session(record, now);
}

device_grant_issuance issue_device_grant(const forge::auth::pairing::credential& credential,
                                         device_grant_options options) {
   const auto idle_expires_at =
       canonical_idle_expiry(options.now, options.absolute_expires_at, options.idle_timeout, time_input::options);
   require_active_credential(credential, options.now);

   auto material = forge::crypto::core::secret_bytes{forge::crypto::core::random_bytes(secret_bytes)};
   auto token =
       forge::crypto::core::secret_string{forge::codec::base64::encode(material.span(), secret_encode_options())};
   return {
       .record =
           {
               .token_digest = forge::crypto::digest::sha256::hash(material.span()),
               .credential_id = credential.id,
               .credential_generation = credential.generation,
               .rotation_generation = 1,
               .issued_at = options.now,
               .last_activity_at = options.now,
               .idle_expires_at = idle_expires_at,
               .absolute_expires_at = options.absolute_expires_at,
               .idle_timeout = options.idle_timeout,
           },
       .token = std::move(token),
   };
}

void validate_device_grant_issuance(const device_grant_issuance& issuance) {
   require_device_grant_record(issuance.record);
   if (issuance.record.state != device_grant_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "device grant issuance record must be active");
   }
   verify_secret(issuance.token, issuance.record.token_digest, false);
}

forge::crypto::digest::sha256 identify_device_grant_token(const forge::crypto::core::secret_string& token) {
   return identify_secret(token, false);
}

forge::auth::pairing::credential_binding validate_device_grant(const device_grant_record& record,
                                                               const forge::crypto::core::secret_string& token,
                                                               const forge::auth::pairing::credential& credential,
                                                               time_point now) {
   require_device_grant_record(record);
   verify_secret(token, record.token_digest, false);
   require_device_grant_lifecycle(record, now);
   require_matching_credential(record, credential, now);
   return {.id = record.credential_id, .generation = record.credential_generation};
}

device_grant_issuance rotate_device_grant(device_grant_record& record, const forge::crypto::core::secret_string& token,
                                          const forge::auth::pairing::credential& credential, time_point now) {
   static_cast<void>(validate_device_grant(record, token, credential, now));
   if (record.rotation_generation == std::numeric_limits<std::uint64_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "device grant rotation generation is exhausted");
   }

   const auto next_idle_expires_at =
       canonical_idle_expiry(now, record.absolute_expires_at, record.idle_timeout, time_input::record);
   auto material = forge::crypto::core::secret_bytes{forge::crypto::core::random_bytes(secret_bytes)};
   const auto digest = forge::crypto::digest::sha256::hash(material.span());
   if (forge::crypto::core::constant_time_equal(record.token_digest.to_uint8_span(), digest.to_uint8_span())) {
      FORGE_THROW_EXCEPTION(exceptions::secret_collision, "device grant token material collided during rotation");
   }
   auto result = device_grant_issuance{
       .record =
           {
               .token_digest = digest,
               .credential_id = record.credential_id,
               .credential_generation = record.credential_generation,
               .rotation_generation = record.rotation_generation + 1,
               .issued_at = now,
               .last_activity_at = now,
               .idle_expires_at = next_idle_expires_at,
               .absolute_expires_at = record.absolute_expires_at,
               .idle_timeout = record.idle_timeout,
           },
       .token =
           forge::crypto::core::secret_string{forge::codec::base64::encode(material.span(), secret_encode_options())},
   };
   record.state = device_grant_state::rotated;
   record.terminal_at = now;
   return result;
}

device_grant_refresh refresh_device_grant(device_grant_record& record, const forge::crypto::core::secret_string& token,
                                          const forge::auth::pairing::credential& credential, session_options options) {
   static_cast<void>(validate_device_grant(record, token, credential, options.now));
   if (options.absolute_expires_at > record.absolute_expires_at) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "session absolute expiry exceeds its device grant absolute expiry");
   }
   auto next_session = issue_session(credential, options);
   auto next_grant = rotate_device_grant(record, token, credential, options.now);
   return {.grant = std::move(next_grant), .session = std::move(next_session)};
}

void revoke_device_grant(device_grant_record& record, time_point now) {
   require_device_grant_record(record);
   require_device_grant_lifecycle(record, now);
   record.state = device_grant_state::revoked;
   record.terminal_at = now;
}

} // namespace forge::auth::session
