#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>

import forge.auth.pairing.exceptions;
import forge.auth.pairing.pairing;
import forge.auth.pairing.serialization;
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
   namespace credentials = forge::auth::credential;
   namespace pairing = forge::auth::pairing;

   const auto now = pairing::time_point{std::chrono::seconds{1}};
   auto issuance = pairing::begin_bootstrap({
       .now = now,
       .expires_at = now + std::chrono::minutes{1},
       .scope_baseline = {"pair.read"},
   });
   const auto bootstrap_digest = pairing::identify_bootstrap_token(issuance.token);
   auto pending_issuance = pairing::consume_bootstrap(issuance.record, issuance.token,
                                                      {.identity = "consumer", .requested_scopes = {"pair.read"}},
                                                      {.now = now + std::chrono::seconds{1},
                                                       .request_expires_at = now + std::chrono::seconds{30},
                                                       .pending_count = 0,
                                                       .max_pending_requests = 1});
   auto pending = std::move(pending_issuance.record);
   const auto credential = pairing::approve_pending(
       pending, {.id = {.value = "credential-consumer"}, .now = now + std::chrono::seconds{2}});
   const auto pre_session_digest = pairing::identify_pre_session(pending_issuance.pre_session_token);
   const auto credential_binding = pairing::consume_approved_pre_session(pending, pending_issuance.pre_session_token,
                                                                         now + std::chrono::seconds{2});
   const auto exception_import = pairing::exceptions::code::replayed;
   const auto bootstrap_round_trip = forge::raw::unpack_exact<pairing::bootstrap_record>(
       std::span<const std::uint8_t>{forge::raw::pack(issuance.record)});
   const auto pending_round_trip =
       forge::raw::unpack_exact<pairing::pending_request>(std::span<const std::uint8_t>{forge::raw::pack(pending)});
   const auto credential_round_trip =
       forge::raw::unpack_exact<credentials::record>(std::span<const std::uint8_t>{forge::raw::pack(credential)});
   auto invalid_bootstrap = issuance.record;
   invalid_bootstrap.expires_at = invalid_bootstrap.created_at;
   auto invalid_pending = pending;
   invalid_pending.state = static_cast<pairing::pending_state>(0xffU);
   auto invalid_credential = credential;
   invalid_credential.state = static_cast<credentials::state>(0xffU);
   const auto invalid_bootstrap_rejected = rejects_raw<forge::raw::exceptions::codec_error>([&] {
      static_cast<void>(forge::raw::unpack_exact<pairing::bootstrap_record>(
          std::span<const std::uint8_t>{forge::raw::pack(invalid_bootstrap)}));
   });
   const auto invalid_pending_rejected = rejects_raw<forge::raw::exceptions::codec_error>([&] {
      static_cast<void>(forge::raw::unpack_exact<pairing::pending_request>(
          std::span<const std::uint8_t>{forge::raw::pack(invalid_pending)}));
   });
   const auto invalid_credential_rejected = rejects_raw<forge::raw::exceptions::codec_error>([&] {
      static_cast<void>(forge::raw::unpack_exact<credentials::record>(
          std::span<const std::uint8_t>{forge::raw::pack(invalid_credential)}));
   });

   const auto require = [](bool condition, const char* message) {
      if (!condition) {
         std::cerr << message << '\n';
      }
      return condition;
   };
   if (!require(pending.state == pairing::pending_state::approved, "pending state was not approved") ||
       !require(bootstrap_digest == issuance.record.digest, "bootstrap digest changed") ||
       !require(credential.id.value == "credential-consumer", "credential ID changed") ||
       !require(pending.pre_session_consumed, "pre-session was not consumed") ||
       !require(pre_session_digest == pending.pre_session_digest, "pre-session digest changed") ||
       !require(pending.approved_credential.has_value(), "approved credential binding is missing") ||
       !require(pending.approved_credential.has_value() && credential_binding == *pending.approved_credential,
                "approved credential binding changed") ||
       !require(exception_import == pairing::exceptions::code::replayed, "pairing exception import changed") ||
       !require(bootstrap_round_trip == issuance.record, "bootstrap Raw round-trip changed") ||
       !require(pending_round_trip == pending, "pending Raw round-trip changed") ||
       !require(credential_round_trip == credential, "credential Raw round-trip changed") ||
       !require(invalid_bootstrap_rejected, "invalid bootstrap timestamps were accepted") ||
       !require(invalid_pending_rejected, "invalid pending state was accepted") ||
       !require(invalid_credential_rejected, "invalid credential state was accepted")) {
      return 1;
   }
   return 0;
}
