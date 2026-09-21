#include <boost/test/unit_test.hpp>
#include <boost/describe.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import forge.auth.pairing.exceptions;
import forge.auth.pairing.pairing;
import forge.codec.base64;
import forge.crypto.core.secret_string;

namespace pairing = forge::auth::pairing;

namespace {

const auto test_now = pairing::time_point{std::chrono::seconds{1'700'000'000}};

[[nodiscard]] pairing::bootstrap_issuance issue(pairing::scope_set scopes = {"pair.read", "pair.write"}) {
   return pairing::begin_bootstrap({
       .now = test_now,
       .expires_at = test_now + std::chrono::minutes{5},
       .scope_baseline = std::move(scopes),
   });
}

[[nodiscard]] pairing::consume_options consume_at(pairing::time_point now = test_now + std::chrono::seconds{1},
                                                  std::size_t pending_count = 0, std::size_t pending_limit = 2) {
   return {
       .now = now,
       .request_expires_at = now + std::chrono::minutes{2},
       .pending_count = pending_count,
       .max_pending_requests = pending_limit,
   };
}

[[nodiscard]] pairing::approval_options approve_at(std::string id = "credential-test",
                                                   pairing::time_point now = test_now + std::chrono::seconds{2}) {
   return {
       .id = {.value = std::move(id)},
       .now = now,
   };
}

template <typename Exception, typename Function> void require_exception(Function&& function) {
   BOOST_CHECK_THROW(std::forward<Function>(function)(), Exception);
}

template <typename Value> constexpr bool exposes_clear_token = requires(Value value) { value.token; };
template <typename Value> constexpr bool exposes_pre_session_token = requires(Value value) { value.pre_session_token; };

} // namespace

BOOST_AUTO_TEST_SUITE(auth_pairing)

BOOST_AUTO_TEST_CASE(public_states_are_described_for_network_projections) {
   static_assert(boost::describe::has_describe_enumerators<pairing::pending_state>::value);
   static_assert(boost::describe::has_describe_enumerators<pairing::credential_state>::value);
}

BOOST_AUTO_TEST_CASE(bootstrap_uses_random_base64url_secret_and_persists_only_a_digest) {
   static_assert(!exposes_clear_token<pairing::bootstrap_record>);
   static_assert(!exposes_pre_session_token<pairing::pending_request>);

   auto issuance = issue({"pair.write", "pair.read", "pair.read"});
   const auto token = issuance.token.view();
   const auto is_base64url = std::all_of(token.begin(), token.end(), [](char value) {
      return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ||
             value == '-' || value == '_';
   });
   const auto decoded = forge::codec::base64::decode(token, {
                                                                .characters = forge::codec::base64::alphabet::url,
                                                                .pad = forge::codec::base64::padding_policy::forbid,
                                                                .ignore_ascii_whitespace = false,
                                                            });
   const auto canonical = forge::codec::base64::encode(decoded, {
                                                                    .characters = forge::codec::base64::alphabet::url,
                                                                    .pad = forge::codec::base64::padding::omit,
                                                                });
   const auto canonical_round_trip = canonical == token;

   BOOST_TEST(token.size() == 43U);
   BOOST_TEST(is_base64url);
   BOOST_TEST(decoded.size() == 32U);
   BOOST_TEST(canonical_round_trip);
   BOOST_TEST(!issuance.record.digest.value.empty());
   BOOST_TEST(issuance.record.scope_baseline == pairing::scope_set({"pair.read", "pair.write"}),
              boost::test_tools::per_element());
   BOOST_TEST(!issuance.record.consumed);
}

BOOST_AUTO_TEST_CASE(bootstrap_rejects_noncanonical_trailing_pad_bits_without_consuming_the_record) {
   auto issuance = issue();
   constexpr auto base64url_characters =
       std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};
   auto noncanonical = std::string{issuance.token.view()};
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

   const auto original = issuance.record;
   const auto presented = forge::crypto::core::secret_string{std::move(noncanonical)};
   require_exception<pairing::exceptions::token_invalid>([&] {
      return pairing::consume_bootstrap(issuance.record, presented,
                                        {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   });
   BOOST_CHECK(issuance.record == original);
}

BOOST_AUTO_TEST_CASE(bootstrap_consumption_is_one_time_bounded_and_does_not_report_clear_tokens) {
   auto issuance = issue();
   const auto malformed_token = forge::crypto::core::secret_string{std::string(42U, 'A') + "="};

   try {
      (void)pairing::consume_bootstrap(issuance.record, malformed_token,
                                       {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
      BOOST_FAIL("malformed bootstrap token must fail");
   } catch (const pairing::exceptions::token_invalid& error) {
      BOOST_TEST(error.message().find(malformed_token.view()) == std::string::npos);
      for (const auto& field : error.context()) {
         BOOST_TEST(field.value.find(malformed_token.view()) == std::string::npos);
      }
   }

   require_exception<pairing::exceptions::capacity_exceeded>([&] {
      return pairing::consume_bootstrap(issuance.record, issuance.token,
                                        {.identity = "device-a", .requested_scopes = {"pair.read"}},
                                        consume_at(test_now + std::chrono::seconds{1}, 2, 2));
   });
   BOOST_TEST(!issuance.record.consumed);

   const auto pending_issuance = pairing::consume_bootstrap(
       issuance.record, issuance.token, {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   BOOST_TEST(issuance.record.consumed);
   BOOST_CHECK(pending_issuance.record.state == pairing::pending_state::pending);

   require_exception<pairing::exceptions::replayed>([&] {
      return pairing::consume_bootstrap(issuance.record, issuance.token,
                                        {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   });

   auto expired = issue();
   require_exception<pairing::exceptions::expired>([&] {
      return pairing::consume_bootstrap(expired.record, expired.token,
                                        {.identity = "device-a", .requested_scopes = {"pair.read"}},
                                        consume_at(expired.record.expires_at));
   });
}

BOOST_AUTO_TEST_CASE(bootstrap_token_is_strictly_identifiable_for_indexed_lookup) {
   const auto issuance = issue();
   const auto identified = pairing::identify_bootstrap_token(issuance.token);

   BOOST_CHECK(identified == issuance.record.digest);

   const auto padded = forge::crypto::core::secret_string{std::string{issuance.token.view()} + "="};
   require_exception<pairing::exceptions::token_invalid>(
       [&] { return pairing::identify_bootstrap_token(padded); });

   const auto short_token = forge::crypto::core::secret_string{std::string(42U, 'A')};
   require_exception<pairing::exceptions::token_invalid>(
       [&] { return pairing::identify_bootstrap_token(short_token); });
}

BOOST_AUTO_TEST_CASE(pre_session_is_canonical_digest_only_and_strictly_identifiable) {
   auto bootstrap = issue();
   const auto pending_issuance = pairing::consume_bootstrap(
       bootstrap.record, bootstrap.token, {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   const auto& pending = pending_issuance.record;
   const auto token = pending_issuance.pre_session_token.view();
   const auto decoded = forge::codec::base64::decode(token, {
                                                                .characters = forge::codec::base64::alphabet::url,
                                                                .pad = forge::codec::base64::padding_policy::forbid,
                                                                .ignore_ascii_whitespace = false,
                                                            });
   const auto canonical = forge::codec::base64::encode(decoded, {
                                                                    .characters = forge::codec::base64::alphabet::url,
                                                                    .pad = forge::codec::base64::padding::omit,
                                                                });
   const auto identified = pairing::identify_pre_session(pending_issuance.pre_session_token);

   BOOST_TEST(token.size() == 43U);
   BOOST_TEST(decoded.size() == 32U);
   BOOST_TEST(canonical == token);
   BOOST_TEST(!pending.pre_session_digest.value.empty());
   BOOST_CHECK(pending.pre_session_digest != bootstrap.record.digest);
   BOOST_CHECK(identified == pending.pre_session_digest);
   BOOST_CHECK(token != bootstrap.token.view());
   pairing::validate_pre_session(pending, pending_issuance.pre_session_token, test_now + std::chrono::seconds{1});
}

BOOST_AUTO_TEST_CASE(pre_session_validation_and_supersession_rotate_the_secret) {
   auto bootstrap = issue();
   auto initial = pairing::consume_bootstrap(bootstrap.record, bootstrap.token,
                                             {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   const auto initial_token = initial.pre_session_token;
   const auto initial_digest = initial.record.pre_session_digest;
   const auto padded = forge::crypto::core::secret_string{std::string(42U, 'A') + "="};
   const auto initial_before = initial.record;
   try {
      pairing::validate_pre_session(initial.record, padded, test_now + std::chrono::seconds{1});
      BOOST_FAIL("malformed pre-session token must fail");
   } catch (const pairing::exceptions::token_invalid& error) {
      BOOST_TEST(error.message().find(initial_token.view()) == std::string::npos);
      for (const auto& field : error.context()) {
         BOOST_TEST(field.value.find(initial_token.view()) == std::string::npos);
      }
   }
   BOOST_CHECK(initial.record == initial_before);
   require_exception<pairing::exceptions::token_invalid>(
       [&] { pairing::validate_pre_session(initial.record, bootstrap.token, test_now + std::chrono::seconds{1}); });
   BOOST_CHECK(initial.record == initial_before);

   auto replacement = pairing::supersede_pending(
       initial.record, {.identity = "device-b", .requested_scopes = {"pair.read"}}, test_now + std::chrono::seconds{2});
   BOOST_CHECK(initial.record.state == pairing::pending_state::superseded);
   BOOST_CHECK(replacement.record.pre_session_digest != initial_digest);
   BOOST_CHECK(replacement.pre_session_token.view() != initial_token.view());

   pairing::validate_pre_session(initial.record, initial_token, test_now + std::chrono::seconds{3});
   const auto superseded_wrong_token_before = initial.record;
   require_exception<pairing::exceptions::token_invalid>(
       [&] { pairing::validate_pre_session(initial.record, bootstrap.token, test_now + std::chrono::seconds{3}); });
   BOOST_CHECK(initial.record == superseded_wrong_token_before);
   const auto superseded_before = initial.record;
   require_exception<pairing::exceptions::invalid_state>([&] {
      static_cast<void>(
          pairing::consume_approved_pre_session(initial.record, initial_token, test_now + std::chrono::seconds{3}));
   });
   BOOST_CHECK(initial.record == superseded_before);
   const auto replacement_before = replacement.record;
   require_exception<pairing::exceptions::token_invalid>(
       [&] { pairing::validate_pre_session(replacement.record, initial_token, test_now + std::chrono::seconds{3}); });
   BOOST_CHECK(replacement.record == replacement_before);
   pairing::validate_pre_session(replacement.record, replacement.pre_session_token, test_now + std::chrono::seconds{3});
}

BOOST_AUTO_TEST_CASE(approved_pre_session_exchanges_once_and_rejected_or_expired_requests_cannot_exchange) {
   auto bootstrap = issue();
   auto approved = pairing::consume_bootstrap(
       bootstrap.record, bootstrap.token, {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   const auto credential = pairing::approve_pending(approved.record, approve_at());
   BOOST_TEST(credential.id.value == "credential-test");
   BOOST_REQUIRE(approved.record.approved_credential.has_value());
   const auto expected_binding = pairing::credential_binding{.id = credential.id, .generation = credential.generation};
   BOOST_CHECK(*approved.record.approved_credential == expected_binding);
   const auto approved_wrong_token_before = approved.record;
   require_exception<pairing::exceptions::token_invalid>(
       [&] { pairing::validate_pre_session(approved.record, bootstrap.token, test_now + std::chrono::seconds{2}); });
   BOOST_CHECK(approved.record == approved_wrong_token_before);
   pairing::validate_pre_session(approved.record, approved.pre_session_token, test_now + std::chrono::seconds{2});
   const auto binding = pairing::consume_approved_pre_session(approved.record, approved.pre_session_token,
                                                              test_now + std::chrono::seconds{2});
   BOOST_CHECK(binding == *approved.record.approved_credential);
   BOOST_TEST(approved.record.pre_session_consumed);
   const auto consumed_before = approved.record;
   require_exception<pairing::exceptions::invalid_state>([&] {
      static_cast<void>(pairing::consume_approved_pre_session(approved.record, approved.pre_session_token,
                                                              approved.record.created_at - std::chrono::seconds{1}));
   });
   BOOST_CHECK(approved.record == consumed_before);
   require_exception<pairing::exceptions::replayed>([&] {
      static_cast<void>(pairing::consume_approved_pre_session(approved.record, approved.pre_session_token,
                                                              approved.record.expires_at));
   });
   BOOST_CHECK(approved.record == consumed_before);
   require_exception<pairing::exceptions::token_invalid>([&] {
      static_cast<void>(
          pairing::consume_approved_pre_session(approved.record, bootstrap.token, approved.record.expires_at));
   });
   BOOST_CHECK(approved.record == consumed_before);

   auto rejected_bootstrap = issue();
   auto rejected =
       pairing::consume_bootstrap(rejected_bootstrap.record, rejected_bootstrap.token,
                                  {.identity = "device-rejected", .requested_scopes = {"pair.read"}}, consume_at());
   pairing::reject_pending(rejected.record, test_now + std::chrono::seconds{2});
   pairing::validate_pre_session(rejected.record, rejected.pre_session_token, test_now + std::chrono::seconds{3});
   const auto rejected_before = rejected.record;
   require_exception<pairing::exceptions::token_invalid>([&] {
      pairing::validate_pre_session(rejected.record, rejected_bootstrap.token, test_now + std::chrono::seconds{3});
   });
   BOOST_CHECK(rejected.record == rejected_before);
   require_exception<pairing::exceptions::invalid_state>([&] {
      static_cast<void>(pairing::consume_approved_pre_session(rejected.record, rejected.pre_session_token,
                                                              test_now + std::chrono::seconds{3}));
   });
   BOOST_CHECK(rejected.record == rejected_before);

   auto expired_bootstrap = issue();
   auto expired =
       pairing::consume_bootstrap(expired_bootstrap.record, expired_bootstrap.token,
                                  {.identity = "device-expired", .requested_scopes = {"pair.read"}}, consume_at());
   const auto expired_before = expired.record;
   require_exception<pairing::exceptions::token_invalid>(
       [&] { pairing::validate_pre_session(expired.record, expired_bootstrap.token, expired.record.expires_at); });
   BOOST_CHECK(expired.record == expired_before);
   require_exception<pairing::exceptions::expired>(
       [&] { pairing::validate_pre_session(expired.record, expired.pre_session_token, expired.record.expires_at); });
   BOOST_CHECK(expired.record == expired_before);
}

BOOST_AUTO_TEST_CASE(persisted_transition_clocks_reject_backdating_without_mutation) {
   auto issuance = issue();
   const auto bootstrap_before = issuance.record;
   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::consume_bootstrap(issuance.record, issuance.token,
                                        {.identity = "device-a", .requested_scopes = {"pair.read"}},
                                        consume_at(test_now - std::chrono::seconds{1}));
   });
   BOOST_CHECK(issuance.record == bootstrap_before);

   auto pending_issuance = pairing::consume_bootstrap(
       issuance.record, issuance.token, {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   auto pending = std::move(pending_issuance.record);
   const auto pending_before = pending;
   const auto backdated = pending.created_at - std::chrono::seconds{1};
   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::supersede_pending(pending, {.identity = "device-b", .requested_scopes = {"pair.read"}},
                                        backdated);
   });
   BOOST_CHECK(pending == pending_before);
   require_exception<pairing::exceptions::invalid_state>(
       [&] { return pairing::approve_pending(pending, approve_at("credential-a", backdated)); });
   BOOST_CHECK(pending == pending_before);
   require_exception<pairing::exceptions::invalid_state>([&] { pairing::reject_pending(pending, backdated); });
   BOOST_CHECK(pending == pending_before);
   require_exception<pairing::exceptions::invalid_state>(
       [&] { pairing::validate_pre_session(pending, pending_issuance.pre_session_token, backdated); });
   BOOST_CHECK(pending == pending_before);
}

BOOST_AUTO_TEST_CASE(malformed_persisted_terminal_timestamps_are_rejected_without_mutation) {
   auto lower_issuance = issue();
   auto lower_pending_issuance =
       pairing::consume_bootstrap(lower_issuance.record, lower_issuance.token,
                                  {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   auto lower_pending = std::move(lower_pending_issuance.record);
   lower_pending.state = pairing::pending_state::rejected;
   lower_pending.resolved_at = lower_pending.created_at - std::chrono::seconds{1};
   const auto lower_before = lower_pending;
   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::supersede_pending(lower_pending, {.identity = "device-b", .requested_scopes = {"pair.read"}},
                                        lower_pending.created_at + std::chrono::seconds{1});
   });
   BOOST_CHECK(lower_pending == lower_before);

   auto upper_issuance = issue();
   auto upper_pending_issuance =
       pairing::consume_bootstrap(upper_issuance.record, upper_issuance.token,
                                  {.identity = "device-a", .requested_scopes = {"pair.read"}}, consume_at());
   auto upper_pending = std::move(upper_pending_issuance.record);
   upper_pending.state = pairing::pending_state::approved;
   upper_pending.resolved_at = upper_pending.expires_at;
   upper_pending.approved_credential = pairing::credential_binding{
       .id = {.value = "credential-upper"},
       .generation = 1,
   };
   const auto upper_before = upper_pending;
   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::approve_pending(
          upper_pending, approve_at("credential-upper", upper_pending.created_at + std::chrono::seconds{1}));
   });
   BOOST_CHECK(upper_pending == upper_before);

   auto invalid_generation_bootstrap = issue();
   auto invalid_generation = pairing::consume_bootstrap(
       invalid_generation_bootstrap.record, invalid_generation_bootstrap.token,
       {.identity = "device-invalid-generation", .requested_scopes = {"pair.read"}}, consume_at());
   static_cast<void>(pairing::approve_pending(invalid_generation.record, approve_at("credential-generation")));
   BOOST_REQUIRE(invalid_generation.record.approved_credential.has_value());
   invalid_generation.record.approved_credential->generation = 2;
   const auto invalid_generation_before = invalid_generation.record;
   require_exception<pairing::exceptions::invalid_state>([&] {
      pairing::validate_pre_session(invalid_generation.record, invalid_generation.pre_session_token,
                                    test_now + std::chrono::seconds{3});
   });
   BOOST_CHECK(invalid_generation.record == invalid_generation_before);

   auto pending_with_binding_issuance = issue();
   auto pending_with_binding =
       pairing::consume_bootstrap(pending_with_binding_issuance.record, pending_with_binding_issuance.token,
                                  {.identity = "device-binding", .requested_scopes = {"pair.read"}}, consume_at());
   pending_with_binding.record.approved_credential =
       pairing::credential_binding{.id = {.value = "credential-binding"}, .generation = 1};
   const auto pending_with_binding_before = pending_with_binding.record;
   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::supersede_pending(pending_with_binding.record,
                                        {.identity = "device-binding-2", .requested_scopes = {"pair.read"}},
                                        test_now + std::chrono::seconds{2});
   });
   BOOST_CHECK(pending_with_binding.record == pending_with_binding_before);

   auto rejected_with_binding_issuance = issue();
   auto rejected_with_binding = pairing::consume_bootstrap(
       rejected_with_binding_issuance.record, rejected_with_binding_issuance.token,
       {.identity = "device-rejected-binding", .requested_scopes = {"pair.read"}}, consume_at());
   pairing::reject_pending(rejected_with_binding.record, test_now + std::chrono::seconds{2});
   rejected_with_binding.record.approved_credential =
       pairing::credential_binding{.id = {.value = "credential-rejected-binding"}, .generation = 1};
   const auto rejected_with_binding_before = rejected_with_binding.record;
   require_exception<pairing::exceptions::invalid_state>([&] {
      pairing::validate_pre_session(rejected_with_binding.record, rejected_with_binding.pre_session_token,
                                    test_now + std::chrono::seconds{3});
   });
   BOOST_CHECK(rejected_with_binding.record == rejected_with_binding_before);

   auto superseded_with_binding_bootstrap = issue();
   auto superseded_with_binding = pairing::consume_bootstrap(
       superseded_with_binding_bootstrap.record, superseded_with_binding_bootstrap.token,
       {.identity = "device-superseded-binding", .requested_scopes = {"pair.read"}}, consume_at());
   static_cast<void>(pairing::supersede_pending(
       superseded_with_binding.record, {.identity = "device-superseded-binding-2", .requested_scopes = {"pair.read"}},
       test_now + std::chrono::seconds{2}));
   superseded_with_binding.record.approved_credential =
       pairing::credential_binding{.id = {.value = "credential-superseded-binding"}, .generation = 1};
   const auto superseded_with_binding_before = superseded_with_binding.record;
   require_exception<pairing::exceptions::invalid_state>([&] {
      pairing::validate_pre_session(superseded_with_binding.record, superseded_with_binding.pre_session_token,
                                    test_now + std::chrono::seconds{3});
   });
   BOOST_CHECK(superseded_with_binding.record == superseded_with_binding_before);

   auto revoked_mismatch = pairing::credential{
       .id = {.value = "credential-mismatch"},
       .identity = "owner-a",
       .scopes = {"pair.read", "pair.write"},
       .issued_at = test_now,
       .updated_at = test_now + std::chrono::seconds{2},
       .state = pairing::credential_state::revoked,
       .revoked_at = test_now + std::chrono::seconds{1},
   };
   const auto revoked_mismatch_before = revoked_mismatch;
   require_exception<pairing::exceptions::invalid_state>([&] {
      pairing::rotate_credential_downscope(revoked_mismatch, {"pair.read"}, test_now + std::chrono::seconds{3});
   });
   BOOST_CHECK(revoked_mismatch == revoked_mismatch_before);

   auto revoked_before_issue = pairing::credential{
       .id = {.value = "credential-before-issue"},
       .identity = "owner-b",
       .scopes = {"pair.read", "pair.write"},
       .issued_at = test_now,
       .updated_at = test_now - std::chrono::seconds{1},
       .state = pairing::credential_state::revoked,
       .revoked_at = test_now - std::chrono::seconds{1},
   };
   const auto revoked_before_issue_before = revoked_before_issue;
   require_exception<pairing::exceptions::invalid_state>([&] {
      pairing::rotate_credential_downscope(revoked_before_issue, {"pair.read"}, test_now + std::chrono::seconds{1});
   });
   BOOST_CHECK(revoked_before_issue == revoked_before_issue_before);
}

BOOST_AUTO_TEST_CASE(pending_transitions_canonicalize_scopes_and_require_explicit_supersession) {
   auto issuance = issue();
   auto pending_issuance = pairing::consume_bootstrap(
       issuance.record, issuance.token,
       {.identity = "device-a", .requested_scopes = {"pair.write", "pair.read", "pair.read"}}, consume_at());
   auto pending = std::move(pending_issuance.record);

   BOOST_TEST(pending.requested_scopes == pairing::scope_set({"pair.read", "pair.write"}),
              boost::test_tools::per_element());
   require_exception<pairing::exceptions::scope_invalid>([&] {
      return pairing::supersede_pending(pending, {.identity = "device-b", .requested_scopes = {"pair.admin"}},
                                        test_now + std::chrono::seconds{2});
   });
   BOOST_CHECK(pending.state == pairing::pending_state::pending);

   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::supersede_pending(pending,
                                        {.identity = "device-a", .requested_scopes = {"pair.read", "pair.write"}},
                                        test_now + std::chrono::seconds{2});
   });

   const auto replacement = pairing::supersede_pending(
       pending, {.identity = "device-b", .requested_scopes = {"pair.read"}}, test_now + std::chrono::seconds{2});
   BOOST_CHECK(pending.state == pairing::pending_state::superseded);
   BOOST_REQUIRE(pending.resolved_at.has_value());
   BOOST_TEST(*pending.resolved_at == test_now + std::chrono::seconds{2});
   BOOST_TEST(replacement.record.identity == "device-b");
   BOOST_TEST(replacement.record.expires_at == pending.expires_at);
}

BOOST_AUTO_TEST_CASE(approval_rotation_and_revocation_are_terminal_and_cannot_escalate_scopes) {
   auto issuance = issue();
   auto pending_issuance = pairing::consume_bootstrap(
       issuance.record, issuance.token, {.identity = "owner-a", .requested_scopes = {"pair.read", "pair.write"}},
       consume_at());
   auto pending = std::move(pending_issuance.record);
   const auto pending_before_approval = pending;
   require_exception<pairing::exceptions::credential_id_invalid>(
       [&] { return pairing::approve_pending(pending, approve_at("", test_now + std::chrono::seconds{2})); });
   BOOST_CHECK(pending == pending_before_approval);

   auto credential = pairing::approve_pending(pending, approve_at("credential-owner-a"));

   BOOST_CHECK(pending.state == pairing::pending_state::approved);
   BOOST_TEST(credential.id.value == "credential-owner-a");
   BOOST_TEST(credential.identity == "owner-a");
   BOOST_TEST(credential.id.value != credential.identity);
   BOOST_TEST(credential.generation == 1U);
   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::approve_pending(pending, approve_at("credential-owner-a", test_now + std::chrono::seconds{3}));
   });

   auto rejected_issuance = issue();
   auto rejected_pending_issuance =
       pairing::consume_bootstrap(rejected_issuance.record, rejected_issuance.token,
                                  {.identity = "owner-rejected", .requested_scopes = {"pair.read"}}, consume_at());
   auto rejected_pending = std::move(rejected_pending_issuance.record);
   pairing::reject_pending(rejected_pending, test_now + std::chrono::seconds{2});
   BOOST_CHECK(rejected_pending.state == pairing::pending_state::rejected);
   require_exception<pairing::exceptions::invalid_state>([&] {
      return pairing::approve_pending(rejected_pending,
                                      approve_at("credential-rejected", test_now + std::chrono::seconds{3}));
   });

   pairing::rotate_credential_downscope(credential, {"pair.read"}, test_now + std::chrono::seconds{3});
   BOOST_TEST(credential.generation == 2U);
   BOOST_TEST(credential.scopes == pairing::scope_set({"pair.read"}), boost::test_tools::per_element());
   require_exception<pairing::exceptions::invalid_state>(
       [&] { pairing::rotate_credential_downscope(credential, {"pair.read"}, test_now + std::chrono::seconds{4}); });
   require_exception<pairing::exceptions::scope_invalid>([&] {
      pairing::rotate_credential_downscope(credential, {"pair.read", "pair.write"}, test_now + std::chrono::seconds{4});
   });

   pairing::revoke_credential(credential, test_now + std::chrono::seconds{4});
   BOOST_CHECK(credential.state == pairing::credential_state::revoked);
   BOOST_REQUIRE(credential.revoked_at.has_value());
   require_exception<pairing::exceptions::invalid_state>(
       [&] { pairing::revoke_credential(credential, test_now + std::chrono::seconds{5}); });

   auto exhausted = pairing::credential{
       .id = {.value = "credential-owner-b"},
       .identity = "owner-b",
       .scopes = {"pair.read", "pair.write"},
       .generation = std::numeric_limits<std::uint64_t>::max(),
       .issued_at = test_now,
       .updated_at = test_now,
   };
   require_exception<pairing::exceptions::generation_exhausted>(
       [&] { pairing::rotate_credential_downscope(exhausted, {"pair.read"}, test_now + std::chrono::seconds{1}); });
}

BOOST_AUTO_TEST_CASE(invalid_identity_scope_and_expiry_inputs_have_typed_failures) {
   require_exception<pairing::exceptions::scope_invalid>(
       [] { return pairing::canonicalize_scopes({"pair.read", ""}); });
   require_exception<pairing::exceptions::invalid_options>([] {
      return pairing::begin_bootstrap({
          .now = test_now,
          .expires_at = test_now,
          .scope_baseline = {"pair.read"},
      });
   });

   auto issuance = issue();
   require_exception<pairing::exceptions::identity_invalid>([&] {
      return pairing::consume_bootstrap(issuance.record, issuance.token,
                                        {.identity = "", .requested_scopes = {"pair.read"}}, consume_at());
   });
}

BOOST_AUTO_TEST_SUITE_END()
