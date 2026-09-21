#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

import forge.auth.session.exceptions;
import forge.auth.session.session;
import forge.auth.session.serialization;
import forge.codec.base64;
import forge.crypto.core.secret_string;
import forge.raw.exceptions;
import forge.raw.raw;

namespace pairing = forge::auth::pairing;
namespace session = forge::auth::session;

namespace {

const auto test_now = session::time_point{std::chrono::seconds{1'700'000'000}};

[[nodiscard]] pairing::credential credential(std::string id = "credential-a", std::uint64_t generation = 1,
                                             pairing::scope_set scopes = {"session.read", "session.write"}) {
   return {
       .id = {.value = std::move(id)},
       .identity = "owner-a",
       .scopes = std::move(scopes),
       .generation = generation,
       .issued_at = test_now,
       .updated_at = test_now,
   };
}

[[nodiscard]] session::session_options
options_at(session::time_point now = test_now,
           std::chrono::system_clock::duration idle_timeout = std::chrono::minutes{2},
           std::chrono::system_clock::duration absolute_lifetime = std::chrono::minutes{10}) {
   return {
       .now = now,
       .absolute_expires_at = now + absolute_lifetime,
       .idle_timeout = idle_timeout,
   };
}

[[nodiscard]] session::device_grant_options
grant_options_at(session::time_point now = test_now,
                 std::chrono::system_clock::duration idle_timeout = std::chrono::minutes{2},
                 std::chrono::system_clock::duration absolute_lifetime = std::chrono::minutes{10}) {
   return {
       .now = now,
       .absolute_expires_at = now + absolute_lifetime,
       .idle_timeout = idle_timeout,
   };
}

template <typename Exception, typename Function> void require_exception(Function&& function) {
   BOOST_CHECK_THROW(std::forward<Function>(function)(), Exception);
}

template <typename Value> constexpr bool exposes_session_token = requires(Value value) { value.session_token; };
template <typename Value> constexpr bool exposes_csrf_secret = requires(Value value) { value.csrf_secret; };

[[nodiscard]] bool is_base64url(std::string_view value) {
   return std::all_of(value.begin(), value.end(), [](char character) {
      return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
             (character >= '0' && character <= '9') || character == '-' || character == '_';
   });
}

} // namespace

BOOST_AUTO_TEST_SUITE(auth_session)

BOOST_AUTO_TEST_CASE(issue_generates_independent_digest_only_canonical_secrets) {
   static_assert(!exposes_session_token<session::session_record>);
   static_assert(!exposes_csrf_secret<session::session_record>);
   static_assert(std::is_same_v<decltype(&session::issue_session),
                                session::session_issuance (*)(const pairing::credential&, session::session_options)>);
   static_assert(std::is_same_v<decltype(&session::validate_issuance), void (*)(const session::session_issuance&)>);
   static_assert(std::is_same_v<decltype(&session::rotate_session),
                                session::session_issuance (*)(session::session_record&, const pairing::credential&,
                                                              session::session_options)>);

   const auto owner = credential();
   const auto first = session::issue_session(owner, options_at());
   const auto second = session::issue_session(owner, options_at());

   BOOST_TEST(first.session_token.size() == 43U);
   BOOST_TEST(first.csrf_secret.size() == 43U);
   BOOST_TEST(is_base64url(first.session_token.view()));
   BOOST_TEST(is_base64url(first.csrf_secret.view()));
   BOOST_CHECK(first.session_token.view() != first.csrf_secret.view());
   BOOST_CHECK(first.session_token.view() != second.session_token.view());
   BOOST_CHECK(first.csrf_secret.view() != second.csrf_secret.view());
   BOOST_TEST(!first.record.session_digest.empty());
   BOOST_TEST(!first.record.csrf_digest.empty());
   BOOST_TEST(first.record.credential_id.value == "credential-a");
   BOOST_TEST(first.record.credential_generation == 1U);
   BOOST_TEST(first.record.identity == "owner-a");
   BOOST_TEST(first.record.scopes == pairing::scope_set({"session.read", "session.write"}),
              boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(issuance_validation_rejects_mixed_and_tampered_secret_pairs) {
   const auto owner = credential();
   const auto first = session::issue_session(owner, options_at());
   const auto second = session::issue_session(owner, options_at());
   session::validate_issuance(first);

   auto mixed_session = first;
   mixed_session.session_token = second.session_token;
   require_exception<session::exceptions::token_invalid>([&] { session::validate_issuance(mixed_session); });

   auto mixed_csrf = first;
   mixed_csrf.csrf_secret = second.csrf_secret;
   require_exception<session::exceptions::csrf_invalid>([&] { session::validate_issuance(mixed_csrf); });

   auto malformed_record = first;
   malformed_record.record.csrf_digest = malformed_record.record.session_digest;
   require_exception<session::exceptions::invalid_state>([&] { session::validate_issuance(malformed_record); });

   auto revoked = first;
   session::logout_session(revoked.record, test_now + std::chrono::seconds{1});
   require_exception<session::exceptions::invalid_state>([&] { session::validate_issuance(revoked); });

   auto rotated = first;
   static_cast<void>(session::rotate_session(rotated.record, owner, options_at(test_now + std::chrono::seconds{1})));
   require_exception<session::exceptions::invalid_state>([&] { session::validate_issuance(rotated); });
}

BOOST_AUTO_TEST_CASE(validation_rejects_bounded_malformed_and_noncanonical_secrets) {
   const auto owner = credential();
   const auto issuance = session::issue_session(owner, options_at());
   const auto now = test_now + std::chrono::seconds{1};
   const auto oversized = forge::crypto::core::secret_string{std::string(4'096U, 'A')};
   const auto padded = forge::crypto::core::secret_string{std::string(42U, 'A') + "="};

   require_exception<session::exceptions::token_invalid>(
       [&] { return session::validate_session(issuance.record, oversized, owner, now); });
   require_exception<session::exceptions::token_invalid>(
       [&] { return session::validate_session(issuance.record, padded, owner, now); });
   require_exception<session::exceptions::csrf_invalid>(
       [&] { session::verify_csrf_secret(issuance.record, padded, now); });

   constexpr auto base64url_characters =
       std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};
   auto noncanonical = std::string{issuance.session_token.view()};
   const auto final_index = base64url_characters.find(noncanonical.back());
   BOOST_REQUIRE(final_index != std::string_view::npos);
   BOOST_REQUIRE(final_index + 1U < base64url_characters.size());
   noncanonical.back() = base64url_characters[final_index + 1U];
   BOOST_CHECK_THROW(
       static_cast<void>(forge::codec::base64::decode(noncanonical,
                                                      {
                                                          .characters = forge::codec::base64::alphabet::url,
                                                          .pad = forge::codec::base64::padding_policy::forbid,
                                                          .ignore_ascii_whitespace = false,
                                                      })),
       forge::codec::base64::exceptions::invalid_input);
   const auto presented = forge::crypto::core::secret_string{std::move(noncanonical)};
   require_exception<session::exceptions::token_invalid>(
       [&] { return session::validate_session(issuance.record, presented, owner, now); });
}

BOOST_AUTO_TEST_CASE(validation_returns_principal_and_enforces_credential_binding) {
   const auto owner = credential();
   const auto issuance = session::issue_session(owner, options_at());
   const auto now = test_now + std::chrono::seconds{1};
   const auto value = session::validate_session(issuance.record, issuance.session_token, owner, now);

   BOOST_TEST(value.credential_id.value == "credential-a");
   BOOST_TEST(value.credential_generation == 1U);
   BOOST_TEST(value.identity == "owner-a");
   BOOST_TEST(value.scopes == pairing::scope_set({"session.read", "session.write"}), boost::test_tools::per_element());

   auto generation_changed = owner;
   ++generation_changed.generation;
   generation_changed.updated_at = now;
   require_exception<session::exceptions::credential_mismatch>(
       [&] { return session::validate_session(issuance.record, issuance.session_token, generation_changed, now); });

   auto revoked = owner;
   revoked.state = pairing::credential_state::revoked;
   revoked.updated_at = now;
   revoked.revoked_at = now;
   require_exception<session::exceptions::credential_revoked>(
       [&] { return session::validate_session(issuance.record, issuance.session_token, revoked, now); });

   auto identity_changed = owner;
   identity_changed.identity = "owner-b";
   require_exception<session::exceptions::identity_mismatch>(
       [&] { return session::validate_session(issuance.record, issuance.session_token, identity_changed, now); });

   auto scopes_changed = owner;
   scopes_changed.scopes = {"session.read"};
   require_exception<session::exceptions::scope_mismatch>(
       [&] { return session::validate_session(issuance.record, issuance.session_token, scopes_changed, now); });
}

BOOST_AUTO_TEST_CASE(absolute_idle_and_renewal_boundaries_preserve_secret_digests) {
   const auto owner = credential();
   const auto issuance =
       session::issue_session(owner, options_at(test_now, std::chrono::minutes{2}, std::chrono::minutes{10}));
   require_exception<session::exceptions::idle_expired>([&] {
      return session::validate_session(issuance.record, issuance.session_token, owner, issuance.record.idle_expires_at);
   });
   require_exception<session::exceptions::expired>([&] {
      return session::validate_session(issuance.record, issuance.session_token, owner,
                                       issuance.record.absolute_expires_at);
   });

   auto renewable =
       session::issue_session(owner, options_at(test_now, std::chrono::minutes{8}, std::chrono::minutes{10}));
   const auto session_digest = renewable.record.session_digest;
   const auto csrf_digest = renewable.record.csrf_digest;
   const auto renewal_time = test_now + std::chrono::minutes{7};
   session::renew_idle(renewable.record, renewal_time);
   BOOST_TEST(renewable.record.last_activity_at == renewal_time);
   BOOST_TEST(renewable.record.idle_expires_at == renewable.record.absolute_expires_at);
   BOOST_CHECK(renewable.record.session_digest == session_digest);
   BOOST_CHECK(renewable.record.csrf_digest == csrf_digest);
}

BOOST_AUTO_TEST_CASE(backdated_session_transitions_fail_without_mutation) {
   const auto owner = credential();
   auto issuance = session::issue_session(owner, options_at());
   const auto before = issuance.record;
   const auto backdated = test_now - std::chrono::seconds{1};

   require_exception<session::exceptions::invalid_state>([&] { session::renew_idle(issuance.record, backdated); });
   BOOST_CHECK(issuance.record == before);
   require_exception<session::exceptions::invalid_state>([&] { session::logout_session(issuance.record, backdated); });
   BOOST_CHECK(issuance.record == before);
   require_exception<session::exceptions::invalid_state>(
       [&] { return session::rotate_session(issuance.record, owner, options_at(backdated)); });
   BOOST_CHECK(issuance.record == before);
}

BOOST_AUTO_TEST_CASE(malformed_persisted_timestamps_are_rejected_without_mutation) {
   const auto owner = credential();

   auto noncanonical_idle_expiry = session::issue_session(owner, options_at());
   noncanonical_idle_expiry.record.idle_expires_at =
       noncanonical_idle_expiry.record.last_activity_at + std::chrono::minutes{1};
   const auto noncanonical_idle_expiry_before = noncanonical_idle_expiry.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::renew_idle(noncanonical_idle_expiry.record, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(noncanonical_idle_expiry.record == noncanonical_idle_expiry_before);

   auto idle_at_activity = session::issue_session(owner, options_at());
   idle_at_activity.record.idle_expires_at = idle_at_activity.record.last_activity_at;
   const auto idle_at_activity_before = idle_at_activity.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::renew_idle(idle_at_activity.record, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(idle_at_activity.record == idle_at_activity_before);

   auto terminal_before_activity = session::issue_session(owner, options_at());
   terminal_before_activity.record.last_activity_at = test_now + std::chrono::seconds{2};
   terminal_before_activity.record.state = session::session_state::rotated;
   terminal_before_activity.record.terminal_at = test_now + std::chrono::seconds{1};
   const auto terminal_before_activity_before = terminal_before_activity.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::logout_session(terminal_before_activity.record, test_now + std::chrono::seconds{3}); });
   BOOST_CHECK(terminal_before_activity.record == terminal_before_activity_before);

   auto terminal_at_absolute = session::issue_session(owner, options_at());
   terminal_at_absolute.record.state = session::session_state::revoked;
   terminal_at_absolute.record.terminal_at = terminal_at_absolute.record.absolute_expires_at;
   const auto terminal_at_absolute_before = terminal_at_absolute.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::logout_session(terminal_at_absolute.record, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(terminal_at_absolute.record == terminal_at_absolute_before);

   auto terminal_at_idle = session::issue_session(owner, options_at());
   terminal_at_idle.record.state = session::session_state::revoked;
   terminal_at_idle.record.terminal_at = terminal_at_idle.record.idle_expires_at;
   const auto terminal_at_idle_before = terminal_at_idle.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::logout_session(terminal_at_idle.record, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(terminal_at_idle.record == terminal_at_idle_before);

   auto identical_digests = session::issue_session(owner, options_at());
   identical_digests.record.csrf_digest = identical_digests.record.session_digest;
   const auto identical_digests_before = identical_digests.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::renew_idle(identical_digests.record, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(identical_digests.record == identical_digests_before);

   auto before_epoch = session::issue_session(owner, options_at());
   before_epoch.record.created_at = session::time_point{} - std::chrono::seconds{1};
   const auto before_epoch_before = before_epoch.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::renew_idle(before_epoch.record, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(before_epoch.record == before_epoch_before);
}

BOOST_AUTO_TEST_CASE(extreme_time_options_are_checked_without_overflow) {
   const auto epoch = session::time_point{};
   const auto maximum = session::time_point::max();
   const auto maximum_timeout = std::chrono::system_clock::duration::max();
   auto epoch_credential = credential();
   epoch_credential.issued_at = epoch;
   epoch_credential.updated_at = epoch;

   const auto issuance = session::issue_session(
       epoch_credential, {.now = epoch, .absolute_expires_at = maximum, .idle_timeout = maximum_timeout});
   BOOST_CHECK(issuance.record.idle_expires_at == maximum);

   auto previous = session::issue_session(epoch_credential, options_at(epoch));
   const auto previous_before = previous.record;
   const auto before_epoch = epoch - std::chrono::seconds{1};
   require_exception<session::exceptions::invalid_options>([&] {
      return session::rotate_session(previous.record, epoch_credential,
                                     {.now = before_epoch,
                                      .absolute_expires_at = epoch + std::chrono::minutes{1},
                                      .idle_timeout = std::chrono::minutes{1}});
   });
   BOOST_CHECK(previous.record == previous_before);
}

BOOST_AUTO_TEST_CASE(rotation_prevents_fixation_and_logout_revoke_are_terminal) {
   const auto owner = credential();
   auto previous = session::issue_session(owner, options_at());
   const auto previous_session = previous.session_token;
   const auto previous_csrf = previous.csrf_secret;
   auto next = session::rotate_session(previous.record, owner, options_at(test_now + std::chrono::seconds{1}));

   BOOST_CHECK(previous.record.state == session::session_state::rotated);
   BOOST_REQUIRE(previous.record.terminal_at.has_value());
   BOOST_CHECK(previous_session.view() != next.session_token.view());
   BOOST_CHECK(previous_csrf.view() != next.csrf_secret.view());
   require_exception<session::exceptions::replayed>([&] {
      return session::validate_session(previous.record, previous_session, owner, test_now + std::chrono::seconds{2});
   });
   require_exception<session::exceptions::csrf_invalid>(
       [&] { session::verify_csrf_secret(next.record, previous_csrf, test_now + std::chrono::seconds{2}); });
   static_cast<void>(
       session::validate_session(next.record, next.session_token, owner, test_now + std::chrono::seconds{2}));

   auto logged_out = session::issue_session(owner, options_at());
   session::logout_session(logged_out.record, test_now + std::chrono::seconds{1});
   BOOST_CHECK(logged_out.record.state == session::session_state::revoked);
   const auto logged_out_before = logged_out.record;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::logout_session(logged_out.record, test_now + std::chrono::seconds{2}); });
   BOOST_CHECK(logged_out.record == logged_out_before);

   auto revoked = session::issue_session(owner, options_at());
   session::revoke_session(revoked.record, test_now + std::chrono::seconds{1});
   BOOST_CHECK(revoked.record.state == session::session_state::revoked);
}

BOOST_AUTO_TEST_CASE(device_grant_rotation_preserves_absolute_expiry_and_detects_replay) {
   const auto owner = credential();
   auto previous = session::issue_device_grant(owner, {.now = test_now,
                                                       .absolute_expires_at = test_now + std::chrono::days{180},
                                                       .idle_timeout = std::chrono::days{30}});
   session::validate_device_grant_issuance(previous);
   BOOST_TEST(previous.token.size() == 43U);
   BOOST_TEST(previous.record.rotation_generation == 1U);
   BOOST_CHECK(session::identify_device_grant_token(previous.token) == previous.record.token_digest);

   const auto refresh_time = test_now + std::chrono::days{29};
   const auto absolute_expiry = previous.record.absolute_expires_at;
   const auto previous_token = previous.token;
   auto next = session::rotate_device_grant(previous.record, previous.token, owner, refresh_time);

   BOOST_CHECK(previous.record.state == session::device_grant_state::rotated);
   BOOST_REQUIRE(previous.record.terminal_at.has_value());
   BOOST_CHECK(*previous.record.terminal_at == refresh_time);
   BOOST_TEST(next.record.rotation_generation == 2U);
   BOOST_CHECK(next.record.absolute_expires_at == absolute_expiry);
   BOOST_CHECK(next.record.idle_expires_at == refresh_time + std::chrono::days{30});
   BOOST_CHECK(next.token.view() != previous_token.view());
   BOOST_CHECK(session::validate_device_grant(next.record, next.token, owner, refresh_time).id == owner.id);
   require_exception<session::exceptions::replayed>([&] {
      return session::validate_device_grant(previous.record, previous_token, owner,
                                            refresh_time + std::chrono::seconds{1});
   });
   auto unrelated = forge::crypto::core::secret_string{std::string(43U, 'A')};
   require_exception<session::exceptions::token_invalid>([&] {
      return session::validate_device_grant(previous.record, unrelated, owner, refresh_time + std::chrono::seconds{1});
   });
}

BOOST_AUTO_TEST_CASE(device_grant_is_credential_bound_and_does_not_issue_a_session) {
   const auto owner = credential();
   const auto issuance = session::issue_device_grant(owner, grant_options_at());
   static_assert(!exposes_session_token<session::device_grant_issuance>);
   static_assert(!exposes_csrf_secret<session::device_grant_issuance>);

   auto changed = owner;
   ++changed.generation;
   changed.updated_at = test_now + std::chrono::seconds{1};
   require_exception<session::exceptions::credential_mismatch>([&] {
      return session::validate_device_grant(issuance.record, issuance.token, changed,
                                            test_now + std::chrono::seconds{1});
   });

   auto revoked = owner;
   revoked.state = pairing::credential_state::revoked;
   revoked.updated_at = test_now + std::chrono::seconds{1};
   revoked.revoked_at = revoked.updated_at;
   require_exception<session::exceptions::credential_revoked>([&] {
      return session::validate_device_grant(issuance.record, issuance.token, revoked,
                                            test_now + std::chrono::seconds{1});
   });
}

BOOST_AUTO_TEST_CASE(device_grant_refresh_rotates_grant_and_issues_bound_session) {
   const auto owner = credential();
   auto previous = session::issue_device_grant(owner, grant_options_at());
   const auto refresh_time = test_now + std::chrono::seconds{1};
   auto refreshed = session::refresh_device_grant(previous.record, previous.token, owner,
                                                  {.now = refresh_time,
                                                   .absolute_expires_at = refresh_time + std::chrono::minutes{8},
                                                   .idle_timeout = std::chrono::minutes{2}});

   BOOST_CHECK(previous.record.state == session::device_grant_state::rotated);
   BOOST_TEST(refreshed.grant.record.rotation_generation == 2U);
   const auto principal = session::validate_session(refreshed.session.record, refreshed.session.session_token, owner,
                                                    refresh_time + std::chrono::seconds{1});
   BOOST_CHECK(principal.credential_id == owner.id);
   session::verify_csrf_secret(refreshed.session.record, refreshed.session.csrf_secret,
                               refresh_time + std::chrono::seconds{1});

   auto overlong = session::issue_device_grant(owner, grant_options_at());
   const auto overlong_before = overlong.record;
   require_exception<session::exceptions::invalid_options>([&] {
      return session::refresh_device_grant(
          overlong.record, overlong.token, owner,
          {.now = refresh_time,
           .absolute_expires_at = overlong.record.absolute_expires_at + std::chrono::seconds{1},
           .idle_timeout = std::chrono::minutes{2}});
   });
   BOOST_CHECK(overlong.record == overlong_before);

   auto atomic_failure = session::issue_device_grant(owner, grant_options_at());
   const auto atomic_failure_before = atomic_failure.record;
   require_exception<session::exceptions::invalid_options>([&] {
      return session::refresh_device_grant(
          atomic_failure.record, atomic_failure.token, owner,
          {.now = refresh_time, .absolute_expires_at = refresh_time, .idle_timeout = std::chrono::minutes{2}});
   });
   BOOST_CHECK(atomic_failure.record == atomic_failure_before);
}

BOOST_AUTO_TEST_CASE(device_grant_expiry_revoke_and_backdated_transitions_are_terminal) {
   const auto owner = credential();
   auto issuance = session::issue_device_grant(owner, grant_options_at());
   const auto before = issuance.record;

   require_exception<session::exceptions::invalid_state>([&] {
      return session::rotate_device_grant(issuance.record, issuance.token, owner, test_now - std::chrono::seconds{1});
   });
   BOOST_CHECK(issuance.record == before);

   session::revoke_device_grant(issuance.record, test_now + std::chrono::seconds{1});
   BOOST_CHECK(issuance.record.state == session::device_grant_state::revoked);
   require_exception<session::exceptions::invalid_state>([&] {
      return session::validate_device_grant(issuance.record, issuance.token, owner, test_now + std::chrono::seconds{2});
   });

   const auto expired = session::issue_device_grant(owner, grant_options_at());
   require_exception<session::exceptions::idle_expired>([&] {
      return session::validate_device_grant(expired.record, expired.token, owner, expired.record.idle_expires_at);
   });

   auto malformed = session::issue_device_grant(owner, grant_options_at()).record;
   malformed.rotation_generation = 0;
   const auto malformed_before = malformed;
   require_exception<session::exceptions::invalid_state>(
       [&] { session::revoke_device_grant(malformed, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(malformed == malformed_before);
}

BOOST_AUTO_TEST_CASE(device_grant_raw_round_trip_rejects_malformed_state) {
   const auto owner = credential();
   const auto issuance = session::issue_device_grant(owner, grant_options_at());
   const auto packed = forge::raw::pack(issuance.record);
   BOOST_CHECK(forge::raw::unpack_exact<session::device_grant_record>(packed) == issuance.record);

   auto malformed = issuance.record;
   malformed.state = static_cast<session::device_grant_state>(0xffU);
   BOOST_CHECK_THROW(
       static_cast<void>(forge::raw::unpack_exact<session::device_grant_record>(forge::raw::pack(malformed))),
       forge::raw::exceptions::codec_error);
}

BOOST_AUTO_TEST_SUITE_END()
