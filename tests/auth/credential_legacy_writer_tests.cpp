#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

import forge.auth.credential.serialization;
import forge.auth.pairing.serialization;
import forge.auth.session.serialization;
import forge.codec.hex;
import forge.raw.raw;
import forge.variant.described;
import forge.variant.value;

namespace credentials = forge::auth::credential;
namespace pairing = forge::auth::pairing;
namespace session = forge::auth::session;

namespace {

// Frozen writer from bfeebe5c81b9aa77b9bdf6e73e150382724b2418. It writes
// primitives in the old order, never new Auth codecs or duplicate record types.
template <typename... Values> std::vector<std::uint8_t> legacy_pack(const Values&... values) {
   auto bytes = std::vector<std::uint8_t>{};
   const auto append = [&](const auto& value) {
      const auto part = forge::raw::pack(value);
      bytes.insert(bytes.end(), part.begin(), part.end());
   };
   (append(values), ...);
   return bytes;
}

const auto legacy_id = std::string{"credential-legacy"};
const auto legacy_identity = std::string{"owner-legacy"};
const auto legacy_scopes = std::vector<std::string>{"read", "write"};
const auto first_digest = forge::crypto::digest::sha256{std::string(64U, '1')};
const auto second_digest = forge::crypto::digest::sha256{std::string(64U, '2')};

constexpr auto created_ns = std::int64_t{1'000'000'000};
constexpr auto updated_ns = std::int64_t{2'000'000'000};
constexpr auto terminal_ns = std::int64_t{3'000'000'000};
constexpr auto idle_ns = std::int64_t{5'000'000'000};
constexpr auto absolute_ns = std::int64_t{11'000'000'000};
constexpr auto timeout_ns = std::int64_t{3'000'000'000};

credentials::time_point timestamp(std::int64_t nanoseconds) {
   return credentials::time_point{
       std::chrono::duration_cast<credentials::time_point::duration>(std::chrono::nanoseconds{nanoseconds})};
}

void append_legacy_optional_time(std::vector<std::uint8_t>& bytes, bool present, std::int64_t value) {
   auto part = legacy_pack(present);
   bytes.insert(bytes.end(), part.begin(), part.end());
   if (present) {
      part = legacy_pack(value);
      bytes.insert(bytes.end(), part.begin(), part.end());
   }
}

std::vector<std::uint8_t> legacy_credential_writer(std::uint8_t state) {
   auto bytes = legacy_pack(legacy_id, legacy_identity, legacy_scopes, std::uint64_t{7}, created_ns, updated_ns, state);
   append_legacy_optional_time(bytes, state == 1U, updated_ns);
   return bytes;
}

std::vector<std::uint8_t> legacy_pending_writer(std::uint8_t state) {
   auto bytes = legacy_pack(legacy_identity, std::vector<std::string>{"read"}, legacy_scopes, first_digest,
                            created_ns, absolute_ns, state);
   append_legacy_optional_time(bytes, state != 0U, updated_ns);
   auto part = legacy_pack(state == 1U);
   bytes.insert(bytes.end(), part.begin(), part.end());
   if (state == 1U) {
      part = legacy_pack(legacy_id, std::uint64_t{1});
      bytes.insert(bytes.end(), part.begin(), part.end());
   }
   part = legacy_pack(false);
   bytes.insert(bytes.end(), part.begin(), part.end());
   return bytes;
}

std::vector<std::uint8_t> legacy_session_writer(std::uint8_t state) {
   auto bytes = legacy_pack(first_digest, second_digest, legacy_id, std::uint64_t{7}, legacy_identity, legacy_scopes,
                            created_ns, updated_ns, idle_ns, absolute_ns, timeout_ns, state);
   append_legacy_optional_time(bytes, state != 0U, terminal_ns);
   return bytes;
}

std::vector<std::uint8_t> legacy_device_writer(std::uint8_t state) {
   auto bytes = legacy_pack(first_digest, legacy_id, std::uint64_t{7}, std::uint64_t{3}, created_ns, updated_ns,
                            idle_ns, absolute_ns, timeout_ns, state);
   append_legacy_optional_time(bytes, state != 0U, terminal_ns);
   return bytes;
}

template <typename Record> void require_legacy_bytes(const std::vector<std::uint8_t>& bytes, const Record& expected) {
   const auto decoded = forge::raw::unpack_exact<Record>(std::span<const std::uint8_t>{bytes});
   BOOST_CHECK(decoded == expected);
   BOOST_CHECK(forge::raw::pack(expected) == bytes);
   BOOST_CHECK(forge::raw::pack(decoded) == bytes);
}

} // namespace

BOOST_AUTO_TEST_SUITE(auth_credential_legacy_writer)

BOOST_AUTO_TEST_CASE(credential_id_binding_and_records_preserve_pre_relocation_raw_bytes) {
   require_legacy_bytes(legacy_pack(legacy_id), credentials::id{.value = legacy_id});
   require_legacy_bytes(legacy_pack(legacy_id, std::uint64_t{7}),
                        credentials::binding{.id = {.value = legacy_id}, .generation = 7});
   const auto active_golden = forge::codec::hex::decode(
       "1163726564656e7469616c2d6c65676163790c6f776e65722d6c6567616379020472656164057772697465"
       "070000000000000000ca9a3b0000000000943577000000000000");
   const auto revoked_golden = forge::codec::hex::decode(
       "1163726564656e7469616c2d6c65676163790c6f776e65722d6c6567616379020472656164057772697465"
       "070000000000000000ca9a3b00000000009435770000000001010094357700000000");
   for (const auto state : {std::uint8_t{0}, std::uint8_t{1}}) {
      const auto bytes = legacy_credential_writer(state);
      BOOST_CHECK(bytes == (state == 0U ? active_golden : revoked_golden));
      const auto expected = credentials::record{
          .id = {.value = legacy_id},
          .identity = legacy_identity,
          .scopes = legacy_scopes,
          .generation = 7,
          .issued_at = timestamp(created_ns),
          .updated_at = timestamp(updated_ns),
          .state = static_cast<credentials::state>(state),
          .revoked_at = state == 1U ? std::optional{timestamp(updated_ns)} : std::nullopt,
      };
      require_legacy_bytes(bytes, expected);
      credentials::validate_record(expected);
   }
}

BOOST_AUTO_TEST_CASE(pairing_persisted_records_keep_digest_field_enum_and_optional_binding_order) {
   require_legacy_bytes(legacy_pack(first_digest, legacy_scopes, created_ns, absolute_ns, true),
                        pairing::bootstrap_record{.digest = {.value = first_digest},
                                                  .scope_baseline = legacy_scopes,
                                                  .created_at = timestamp(created_ns),
                                                  .expires_at = timestamp(absolute_ns),
                                                  .consumed = true});
   for (const auto state : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}}) {
      const auto expected = pairing::pending_request{
          .identity = legacy_identity,
          .requested_scopes = {"read"},
          .scope_baseline = legacy_scopes,
          .pre_session_digest = {.value = first_digest},
          .created_at = timestamp(created_ns),
          .expires_at = timestamp(absolute_ns),
          .state = static_cast<pairing::pending_state>(state),
          .resolved_at = state != 0U ? std::optional{timestamp(updated_ns)} : std::nullopt,
          .approved_credential = state == 1U
                                     ? std::optional{credentials::binding{.id = {.value = legacy_id}, .generation = 1}}
                                     : std::nullopt,
      };
      require_legacy_bytes(legacy_pending_writer(state), expected);
   }
}

BOOST_AUTO_TEST_CASE(session_and_device_records_keep_the_complete_old_writer_layout) {
   static_assert(static_cast<int>(session::session_state::active) == 0);
   static_assert(static_cast<int>(session::session_state::rotated) == 1);
   static_assert(static_cast<int>(session::session_state::revoked) == 2);
   static_assert(static_cast<int>(session::device_grant_state::active) == 0);
   static_assert(static_cast<int>(session::device_grant_state::rotated) == 1);
   static_assert(static_cast<int>(session::device_grant_state::revoked) == 2);
   for (const auto state : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}}) {
      const auto terminal = state != 0U ? std::optional{timestamp(terminal_ns)} : std::nullopt;
      require_legacy_bytes(legacy_session_writer(state), session::session_record{
          .session_digest = first_digest,
          .csrf_digest = second_digest,
          .credential_id = {.value = legacy_id},
          .credential_generation = 7,
          .identity = legacy_identity,
          .scopes = legacy_scopes,
          .created_at = timestamp(created_ns),
          .last_activity_at = timestamp(updated_ns),
          .idle_expires_at = timestamp(idle_ns),
          .absolute_expires_at = timestamp(absolute_ns),
          .idle_timeout = std::chrono::duration_cast<session::time_point::duration>(std::chrono::nanoseconds{timeout_ns}),
          .state = static_cast<session::session_state>(state),
          .terminal_at = terminal,
      });
      require_legacy_bytes(legacy_device_writer(state), session::device_grant_record{
          .token_digest = first_digest,
          .credential_id = {.value = legacy_id},
          .credential_generation = 7,
          .rotation_generation = 3,
          .issued_at = timestamp(created_ns),
          .last_activity_at = timestamp(updated_ns),
          .idle_expires_at = timestamp(idle_ns),
          .absolute_expires_at = timestamp(absolute_ns),
          .idle_timeout = std::chrono::duration_cast<session::time_point::duration>(std::chrono::nanoseconds{timeout_ns}),
          .state = static_cast<session::device_grant_state>(state),
          .terminal_at = terminal,
      });
   }
}

BOOST_AUTO_TEST_CASE(variant_enum_values_keep_the_legacy_strings_and_raw_variant_bytes) {
   for (const auto state : {std::uint8_t{0}, std::uint8_t{1}}) {
      forge::variant encoded;
      forge::to_variant(static_cast<credentials::state>(state), encoded);
      const auto legacy = forge::variant{state == 0U ? "active" : "revoked"};
      BOOST_CHECK(encoded == legacy);
      BOOST_CHECK(forge::raw::pack(encoded) == forge::raw::pack(legacy));
   }
   const auto legacy_names = std::vector<std::string>{"pending", "approved", "rejected", "superseded"};
   for (auto state = std::uint8_t{0}; state < legacy_names.size(); ++state) {
      forge::variant encoded;
      forge::to_variant(static_cast<pairing::pending_state>(state), encoded);
      const auto legacy = forge::variant{legacy_names[state]};
      BOOST_CHECK(encoded == legacy);
      BOOST_CHECK(forge::raw::pack(encoded) == forge::raw::pack(legacy));
   }
}

BOOST_AUTO_TEST_SUITE_END()
