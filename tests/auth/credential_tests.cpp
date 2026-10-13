#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.auth.credential.record;
import forge.auth.credential.serialization;
import forge.raw.exceptions;
import forge.raw.raw;
import forge.variant.described;
import forge.variant.value;

namespace credentials = forge::auth::credential;

namespace {

const auto test_now = credentials::time_point{std::chrono::seconds{1'700'000'000}};

[[nodiscard]] credentials::record active_record() {
   return {
       .id = {.value = "configured-credential"},
       .identity = "local-subject",
       .scopes = {"resource.read", "resource.write"},
       .issued_at = test_now,
       .updated_at = test_now,
   };
}

template <typename Exception, typename Function> void require_unchanged_failure(credentials::record& value,
                                                                               Function&& function) {
   const auto before = value;
   BOOST_CHECK_THROW(std::forward<Function>(function)(), Exception);
   BOOST_CHECK(value == before);
}

} // namespace

BOOST_AUTO_TEST_SUITE(auth_credential)

BOOST_AUTO_TEST_CASE(neutral_records_validate_without_pairing_or_token_issuance) {
   static_assert(boost::describe::has_describe_enumerators<credentials::state>::value);
   static_assert(static_cast<int>(credentials::state::active) == 0);
   static_assert(static_cast<int>(credentials::state::revoked) == 1);
   const auto owner = active_record();
   credentials::validate_record(owner);
   credentials::validate_active(owner, test_now);
   BOOST_CHECK_THROW(credentials::validate_id({}), credentials::exceptions::credential_id_invalid);
   BOOST_CHECK_THROW(credentials::validate_identity(""), credentials::exceptions::identity_invalid);
   BOOST_CHECK_THROW(credentials::validate_scopes({}), credentials::exceptions::scope_invalid);
   BOOST_CHECK_THROW(credentials::validate_scopes({"resource.read", ""}), credentials::exceptions::scope_invalid);
   BOOST_CHECK_THROW(credentials::validate_scopes({"resource.write", "resource.read"}),
                     credentials::exceptions::scope_invalid);
   BOOST_CHECK_THROW(credentials::validate_scopes({"resource.read", "resource.read"}),
                     credentials::exceptions::scope_invalid);
   const auto canonical = credentials::canonicalize_scopes({"resource.write", "resource.read", "resource.read"});
   BOOST_CHECK(canonical == owner.scopes);
   BOOST_TEST(credentials::scopes_subset_of({"resource.read"}, canonical));
   BOOST_TEST(!credentials::scopes_subset_of({"resource.admin"}, canonical));
   BOOST_CHECK_THROW(static_cast<void>(credentials::canonicalize_scopes({})), credentials::exceptions::scope_invalid);
   BOOST_CHECK_THROW(static_cast<void>(credentials::scopes_subset_of({"resource.read", "resource.read"}, canonical)),
                     credentials::exceptions::scope_invalid);
}

BOOST_AUTO_TEST_CASE(validation_has_neutral_typed_codes_without_secret_or_identity_context) {
   try {
      credentials::validate_id({});
      BOOST_FAIL("empty ID must fail");
   } catch (const credentials::exceptions::credential_id_invalid& error) {
      BOOST_CHECK(credentials::exceptions::is(error, credentials::exceptions::code::credential_id_invalid));
      BOOST_CHECK(credentials::exceptions::code_of(error) == credentials::exceptions::code::credential_id_invalid);
      BOOST_TEST(std::string{error.code().category().name()} == "forge.auth.credential");
      BOOST_TEST(error.code().value() == 10);
      BOOST_TEST(error.context().empty());
   }
}

BOOST_AUTO_TEST_CASE(downscope_and_revoke_preserve_generations_and_failure_atomicity) {
   auto owner = active_record();
   require_unchanged_failure<credentials::exceptions::invalid_state>(owner, [&] {
      credentials::rotate_downscope(owner, owner.scopes, test_now);
   });
   require_unchanged_failure<credentials::exceptions::scope_invalid>(owner, [&] {
      credentials::rotate_downscope(owner, {"resource.admin"}, test_now);
   });
   require_unchanged_failure<credentials::exceptions::invalid_state>(owner, [&] {
      credentials::rotate_downscope(owner, {"resource.read"}, test_now - std::chrono::seconds{1});
   });
   credentials::rotate_downscope(owner, {"resource.read", "resource.read"}, test_now + std::chrono::seconds{1});
   BOOST_TEST(owner.generation == 2U);
   BOOST_CHECK(owner.scopes == credentials::scope_set{"resource.read"});
   BOOST_CHECK(owner.updated_at == test_now + std::chrono::seconds{1});
   credentials::revoke(owner, test_now + std::chrono::seconds{2});
   BOOST_CHECK(owner.state == credentials::state::revoked);
   BOOST_CHECK(owner.revoked_at == owner.updated_at);
   BOOST_TEST(owner.generation == 2U);
   credentials::validate_record(owner);
   require_unchanged_failure<credentials::exceptions::invalid_state>(owner, [&] {
      credentials::revoke(owner, test_now + std::chrono::seconds{3});
   });
   require_unchanged_failure<credentials::exceptions::invalid_state>(owner, [&] {
      credentials::rotate_downscope(owner, {"resource.read"}, test_now + std::chrono::seconds{3});
   });
   auto exhausted = active_record();
   exhausted.generation = std::numeric_limits<std::uint64_t>::max();
   require_unchanged_failure<credentials::exceptions::generation_exhausted>(exhausted, [&] {
      credentials::rotate_downscope(exhausted, {"resource.read"}, test_now);
   });
}

BOOST_AUTO_TEST_CASE(malformed_records_cannot_be_admitted_or_mutated) {
   auto owner = active_record();
   owner.generation = 0;
   BOOST_CHECK_THROW(credentials::validate_record(owner), credentials::exceptions::invalid_state);
   owner = active_record();
   owner.state = static_cast<credentials::state>(0xffU);
   require_unchanged_failure<credentials::exceptions::invalid_state>(owner, [&] { credentials::revoke(owner, test_now); });
   owner = active_record();
   owner.revoked_at = test_now;
   BOOST_CHECK_THROW(credentials::validate_record(owner), credentials::exceptions::invalid_state);
   owner.state = credentials::state::revoked;
   owner.revoked_at.reset();
   BOOST_CHECK_THROW(credentials::validate_record(owner), credentials::exceptions::invalid_state);
   owner.revoked_at = test_now - std::chrono::seconds{1};
   BOOST_CHECK_THROW(credentials::validate_record(owner), credentials::exceptions::invalid_state);
   owner.revoked_at = test_now + std::chrono::seconds{1};
   BOOST_CHECK_THROW(credentials::validate_record(owner), credentials::exceptions::invalid_state);
   owner = active_record();
   owner.updated_at = test_now - std::chrono::seconds{1};
   BOOST_CHECK_THROW(credentials::validate_record(owner), credentials::exceptions::invalid_state);
}

BOOST_AUTO_TEST_CASE(raw_decode_retains_strict_enum_and_timestamp_failures) {
   auto owner = active_record();
   const auto decode = [&] {
      const auto bytes = forge::raw::pack(owner);
      return forge::raw::unpack_exact<credentials::record>(std::span<const std::uint8_t>{bytes});
   };
   BOOST_CHECK(decode() == owner);
   owner.state = static_cast<credentials::state>(0xffU);
   BOOST_CHECK_THROW(decode(), forge::raw::exceptions::codec_error);
   owner = active_record();
   owner.updated_at = test_now - std::chrono::seconds{1};
   BOOST_CHECK_THROW(decode(), forge::raw::exceptions::codec_error);
   owner = active_record();
   owner.revoked_at = test_now - std::chrono::seconds{1};
   BOOST_CHECK_THROW(decode(), forge::raw::exceptions::codec_error);
   owner.revoked_at = test_now + std::chrono::seconds{1};
   BOOST_CHECK_THROW(decode(), forge::raw::exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(variant_state_spellings_and_values_do_not_follow_the_namespace_rename) {
   for (const auto value : {credentials::state::active, credentials::state::revoked}) {
      forge::variant encoded;
      forge::to_variant(value, encoded);
      const auto spelling = value == credentials::state::active ? "active" : "revoked";
      BOOST_TEST(encoded.as_string() == spelling);
      auto decoded = credentials::state{};
      forge::from_variant(forge::variant{spelling}, decoded);
      BOOST_CHECK(decoded == value);
   }
   auto value = credentials::state{};
   BOOST_CHECK_THROW(forge::from_variant(forge::variant{"pending"}, value), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
