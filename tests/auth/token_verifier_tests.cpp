#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <concepts>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

import forge.auth.oauth2.token_verifier;

namespace {

namespace oauth2 = forge::auth::oauth2;
using namespace std::chrono_literals;

template <typename Value>
concept stream_insertable = requires(std::ostream& stream, const Value& value) { stream << value; };

template <typename Value>
concept secret_accessible = requires(const Value& value) { value.secret(); };

template <typename Value>
concept publicly_promotable =
    requires(oauth2::verified_token::claims claims) { Value::make_verified_token(std::move(claims)); };

static_assert(!std::is_default_constructible_v<oauth2::verified_token>);
static_assert(!std::is_constructible_v<oauth2::verified_token, oauth2::verified_token::claims>);
static_assert(!std::is_constructible_v<oauth2::verified_token, std::string>);
static_assert(std::is_copy_constructible_v<oauth2::verified_token>);
static_assert(std::is_nothrow_move_constructible_v<oauth2::verified_token>);
static_assert(!std::is_copy_assignable_v<oauth2::verified_token>);
static_assert(!std::is_move_assignable_v<oauth2::verified_token>);
static_assert(!secret_accessible<oauth2::verified_token>);
static_assert(!publicly_promotable<oauth2::token_verifier>);
static_assert(!stream_insertable<oauth2::verified_token>);
static_assert(!boost::describe::has_describe_members<oauth2::verified_token>::value);
static_assert(std::is_abstract_v<oauth2::token_verifier>);
static_assert(std::same_as<decltype(std::declval<oauth2::verified_token&>().issuer()), const std::string&>);
static_assert(std::same_as<decltype(std::declval<oauth2::verified_token&>().subject()), const std::string&>);
static_assert(std::same_as<decltype(std::declval<oauth2::verified_token&>().scopes()), const oauth2::scope_set&>);
static_assert(
    std::same_as<decltype(std::declval<oauth2::verified_token&>().audiences()), const std::vector<std::string>&>);
static_assert(
    std::same_as<decltype(std::declval<oauth2::token_verifier&>().async_verify(
                     std::declval<forge::crypto::core::secret_string>(), std::declval<oauth2::verification_policy>())),
                 boost::asio::awaitable<oauth2::verified_token>>);
static_assert(
    std::same_as<decltype(std::declval<oauth2::token_verifier&>().async_drain()), boost::asio::awaitable<void>>);
static_assert(noexcept(std::declval<oauth2::token_verifier&>().request_stop()));

// This exposes only the protected factory for tests, never protocol verification.
class verifier_fixture : public oauth2::token_verifier {
 public:
   static oauth2::verified_token issue(oauth2::verified_token::claims claims) {
      return make_verified_token(std::move(claims));
   }
};

oauth2::time_point now() {
   return oauth2::time_point{1700000000s};
}

oauth2::verified_token::claims claims() {
   return {
       .issuer = "https://issuer.example",
       .subject = "subject-123",
       .audiences = {"resource-b", "resource-a", "resource-b"},
       .scopes = {"write", "read", "read"},
       .expires_at = now() + 300s,
       .verified_at = now(),
       .valid_until = now() + 60s,
       .policy_version = "v1",
       .client_id = "native-client",
   };
}

oauth2::verification_policy policy() {
   return {
       .issuer = "https://issuer.example",
       .audience = "resource-a",
       .version = "v1",
       .algorithms = {oauth2::signature_algorithm::rs256},
       .required_scopes = {"read"},
   };
}

} // namespace

BOOST_AUTO_TEST_SUITE(token_verifier_tests)

BOOST_AUTO_TEST_CASE(result_owns_only_normalized_immutable_evidence) {
   auto source = claims();
   const auto result = verifier_fixture::issue(source);
   source.subject = "changed-after-verification";
   source.scopes.clear();

   BOOST_CHECK_EQUAL(result.issuer(), "https://issuer.example");
   BOOST_CHECK_EQUAL(result.subject(), "subject-123");
   BOOST_REQUIRE_EQUAL(result.audiences().size(), 2U);
   BOOST_CHECK_EQUAL(result.audiences()[0], "resource-a");
   BOOST_CHECK_EQUAL(result.audiences()[1], "resource-b");
   BOOST_REQUIRE_EQUAL(result.scopes().size(), 2U);
   BOOST_CHECK_EQUAL(result.scopes()[0], "read");
   BOOST_CHECK_EQUAL(result.scopes()[1], "write");
   BOOST_CHECK_EQUAL(result.policy_version(), "v1");
   BOOST_REQUIRE(result.client_id().has_value());
   BOOST_CHECK_EQUAL(*result.client_id(), "native-client");
   BOOST_CHECK(result.expires_at() == now() + 300s);
   BOOST_CHECK(result.verified_at() == now());
   BOOST_CHECK(result.valid_until() == now() + 60s);
   BOOST_CHECK(result.profile() == oauth2::token_profile::jwt_access_token);

   const auto copied = result;
   BOOST_CHECK_EQUAL(copied.subject(), result.subject());
   auto movable = verifier_fixture::issue(claims());
   const auto moved = std::move(movable);
   BOOST_CHECK_EQUAL(moved.subject(), "subject-123");
   BOOST_CHECK(moved.valid_at(now()));
   BOOST_CHECK(!movable.valid_at(now()));
   BOOST_CHECK(!movable.valid_at(now() + 1s));
}

BOOST_AUTO_TEST_CASE(freshness_is_half_open_and_never_extends_original_expiry) {
   const auto tick = oauth2::time_point::duration{1};
   const auto result = verifier_fixture::issue(claims());
   BOOST_CHECK(!result.valid_at(now() - tick));
   BOOST_CHECK(result.valid_at(now()));
   BOOST_CHECK(result.valid_at(now() + 60s - tick));
   BOOST_CHECK(!result.valid_at(now() + 60s));
   BOOST_CHECK(!result.valid_at(now() + 300s));

   auto value = claims();
   value.valid_until = value.expires_at;
   const auto expires = verifier_fixture::issue(std::move(value));
   BOOST_CHECK(expires.valid_at(now() + 300s - tick));
   BOOST_CHECK(!expires.valid_at(now() + 300s));

   for (const auto until : {now() - 1s, now(), now() + 301s}) {
      value = claims();
      value.valid_until = until;
      BOOST_CHECK_THROW((void)verifier_fixture::issue(value), oauth2::exceptions::invalid_token);
   }
}

BOOST_AUTO_TEST_CASE(factory_rejects_missing_or_unsafe_identity_evidence) {
   const auto expect_invalid = [](oauth2::verified_token::claims value) {
      BOOST_CHECK_THROW((void)verifier_fixture::issue(std::move(value)), oauth2::exceptions::invalid_token);
   };
   auto value = claims();
   value.issuer.clear();
   expect_invalid(value);
   value = claims();
   value.subject = std::string{"subject\0suffix", 14};
   expect_invalid(value);
   value = claims();
   value.audiences.clear();
   expect_invalid(value);
   value = claims();
   value.audiences.push_back("");
   expect_invalid(value);
   value = claims();
   value.policy_version.clear();
   expect_invalid(value);
   value = claims();
   value.client_id = "client\nunsafe";
   expect_invalid(value);
   value = claims();
   value.scopes = {"read write"};
   expect_invalid(value);
   value = claims();
   value.scopes = {"read\\write"};
   expect_invalid(value);
   value = claims();
   value.profile = static_cast<oauth2::token_profile>(1234);
   expect_invalid(value);
}

BOOST_AUTO_TEST_CASE(optional_binding_and_empty_grant_do_not_fabricate_claims) {
   auto value = claims();
   value.scopes.clear();
   value.client_id.reset();
   value.profile = oauth2::token_profile::opaque_introspection;
   const auto result = verifier_fixture::issue(std::move(value));
   BOOST_CHECK(result.scopes().empty());
   BOOST_CHECK(!result.client_id());
   BOOST_CHECK(result.profile() == oauth2::token_profile::opaque_introspection);
}

BOOST_AUTO_TEST_CASE(policy_has_explicit_disjoint_jwt_and_introspection_profiles) {
   auto value = policy();
   BOOST_CHECK_NO_THROW(oauth2::validate_policy(value));
   value.algorithms.clear();
   BOOST_CHECK_THROW(oauth2::validate_policy(value), oauth2::exceptions::invalid_request);
   value.profile = oauth2::token_profile::opaque_introspection;
   BOOST_CHECK_NO_THROW(oauth2::validate_policy(value));
   value.algorithms = {oauth2::signature_algorithm::rs256};
   BOOST_CHECK_THROW(oauth2::validate_policy(value), oauth2::exceptions::invalid_request);
   value = policy();
   value.algorithms = {static_cast<oauth2::signature_algorithm>(1234)};
   BOOST_CHECK_THROW(oauth2::validate_policy(value), oauth2::exceptions::invalid_request);
   value = policy();
   value.profile = static_cast<oauth2::token_profile>(1234);
   BOOST_CHECK_THROW(oauth2::validate_policy(value), oauth2::exceptions::invalid_request);
}

BOOST_AUTO_TEST_CASE(policy_requires_resource_version_and_finite_chrono_values) {
   const auto expect_invalid = [](oauth2::verification_policy value) {
      BOOST_CHECK_THROW(oauth2::validate_policy(value), oauth2::exceptions::invalid_request);
   };
   auto value = policy();
   value.issuer.clear();
   expect_invalid(value);
   value = policy();
   value.audience = "resource\r\nunsafe";
   expect_invalid(value);
   value = policy();
   value.version.clear();
   expect_invalid(value);
   value = policy();
   value.freshness = 0s;
   expect_invalid(value);
   value = policy();
   value.freshness = std::chrono::seconds::max();
   expect_invalid(value);
   value = policy();
   value.clock_skew = -1s;
   expect_invalid(value);
   value = policy();
   value.clock_skew = std::chrono::seconds::max();
   expect_invalid(value);
   value = policy();
   value.required_scopes = {"read write"};
   expect_invalid(value);
}

BOOST_AUTO_TEST_CASE(typed_rejection_and_outage_are_distinct) {
   const auto rejection = oauth2::exceptions::invalid_token{"credential rejected"};
   const auto outage = oauth2::exceptions::issuer_unavailable{"issuer unavailable"};
   BOOST_CHECK(oauth2::exceptions::is(rejection, oauth2::exceptions::code::invalid_token));
   BOOST_CHECK(!oauth2::exceptions::is(outage, oauth2::exceptions::code::invalid_token));
   BOOST_CHECK(oauth2::exceptions::code_of(outage) == oauth2::exceptions::code::issuer_unavailable);

   const auto check_code = [](const forge::exceptions::base& failure, oauth2::exceptions::code code) {
      BOOST_CHECK(oauth2::exceptions::code_of(failure) == code);
      BOOST_CHECK(oauth2::exceptions::is(failure, code));
   };
   check_code(oauth2::exceptions::policy_denied{"denied"}, oauth2::exceptions::code::policy_denied);
   check_code(oauth2::exceptions::input_too_large{"input size"}, oauth2::exceptions::code::input_too_large);
   check_code(oauth2::exceptions::response_too_large{"response size"}, oauth2::exceptions::code::response_too_large);
   check_code(oauth2::exceptions::busy{"capacity"}, oauth2::exceptions::code::busy);
   check_code(oauth2::exceptions::stopped{"stopped"}, oauth2::exceptions::code::stopped);
   check_code(oauth2::exceptions::canceled{"canceled"}, oauth2::exceptions::code::canceled);
   BOOST_CHECK_EQUAL(static_cast<int>(oauth2::exceptions::code::canceled), 5);
}

BOOST_AUTO_TEST_CASE(invalid_evidence_never_echoes_its_claims_in_errors) {
   auto value = claims();
   value.subject = "claim-canary-never-log\n";
   auto caught = false;
   try {
      (void)verifier_fixture::issue(std::move(value));
   } catch (const oauth2::exceptions::invalid_token& error) {
      caught = true;
      BOOST_CHECK(std::string{error.what()}.find("claim-canary-never-log") == std::string::npos);
   }
   BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_SUITE_END()
