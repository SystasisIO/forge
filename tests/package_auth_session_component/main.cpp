#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>

import forge.auth.session.exceptions;
import forge.auth.session.session;
import forge.auth.session.serialization;
import forge.raw.exceptions;
import forge.raw.raw;

template <typename Exception, typename Function> [[nodiscard]] bool rejects_raw(Function function) {
   try {
      function();
   } catch (const Exception&) {
      return true;
   } catch (...) {
      return false;
   }
   return false;
}

int main() {
   namespace pairing = forge::auth::pairing;
   namespace session = forge::auth::session;

   const auto now = session::time_point{std::chrono::seconds{1}};
   const auto credential = pairing::credential{
       .id = {.value = "credential-consumer"},
       .identity = "consumer",
       .scopes = {"session.read"},
       .generation = 1,
       .issued_at = now,
       .updated_at = now,
   };
   auto issuance = session::issue_session(
       credential,
       {.now = now, .absolute_expires_at = now + std::chrono::minutes{1}, .idle_timeout = std::chrono::seconds{30}});
   session::validate_issuance(issuance);
   const auto principal =
       session::validate_session(issuance.record, issuance.session_token, credential, now + std::chrono::seconds{1});
   session::verify_csrf_secret(issuance.record, issuance.csrf_secret, now + std::chrono::seconds{1});
   const auto exception_import = session::exceptions::code::csrf_invalid;
   const auto record_round_trip = forge::raw::unpack_exact<session::session_record>(
       std::span<const std::uint8_t>{forge::raw::pack(issuance.record)});
   auto device_grant = session::issue_device_grant(
       credential,
       {.now = now, .absolute_expires_at = now + std::chrono::hours{1}, .idle_timeout = std::chrono::minutes{10}});
   session::validate_device_grant_issuance(device_grant);
   const auto device_grant_round_trip = forge::raw::unpack_exact<session::device_grant_record>(
       std::span<const std::uint8_t>{forge::raw::pack(device_grant.record)});
   auto next_device_grant =
       session::rotate_device_grant(device_grant.record, device_grant.token, credential, now + std::chrono::seconds{1});
   auto invalid_state = issuance.record;
   invalid_state.state = static_cast<session::session_state>(0xffU);
   auto invalid_duration = issuance.record;
   invalid_duration.idle_timeout = std::chrono::system_clock::duration::zero();
   auto invalid_expiry = issuance.record;
   invalid_expiry.absolute_expires_at = invalid_expiry.created_at;
   const auto invalid_state_rejected = rejects_raw<forge::raw::exceptions::codec_error>([&] {
      static_cast<void>(forge::raw::unpack_exact<session::session_record>(
          std::span<const std::uint8_t>{forge::raw::pack(invalid_state)}));
   });
   const auto invalid_duration_rejected = rejects_raw<forge::raw::exceptions::codec_error>([&] {
      static_cast<void>(forge::raw::unpack_exact<session::session_record>(
          std::span<const std::uint8_t>{forge::raw::pack(invalid_duration)}));
   });
   const auto invalid_expiry_rejected = rejects_raw<forge::raw::exceptions::codec_error>([&] {
      static_cast<void>(forge::raw::unpack_exact<session::session_record>(
          std::span<const std::uint8_t>{forge::raw::pack(invalid_expiry)}));
   });

   const auto require = [](bool condition, const char* message) {
      if (!condition) {
         std::cerr << message << '\n';
      }
      return condition;
   };
   if (!require(principal.credential_id.value == "credential-consumer", "session principal changed") ||
       !require(exception_import == session::exceptions::code::csrf_invalid, "session exception import changed") ||
       !require(record_round_trip == issuance.record, "session Raw round-trip changed") ||
       !require(device_grant_round_trip.rotation_generation == 1, "device grant Raw round-trip changed") ||
       !require(next_device_grant.record.rotation_generation == 2, "device grant rotation changed") ||
       !require(invalid_state_rejected, "invalid session state was accepted") ||
       !require(invalid_duration_rejected, "invalid session duration was accepted") ||
       !require(invalid_expiry_rejected, "invalid session expiry was accepted")) {
      return 1;
   }
   return 0;
}
