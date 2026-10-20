#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.auth.attestation.document_schema;
import forge.codec.base64;
import forge.codec.hex;
import forge.codec.json;
import forge.crypto.asymmetric.p256;
import forge.crypto.digest.sha256;
import forge.crypto.jose.codec;
import forge.exceptions;
import forge.schema.constraint;
import forge.variant.schema;
import forge.variant.value;

namespace attestation_policy_fixture {
namespace att = forge::auth::attestation;
namespace jose = forge::crypto::jose;
namespace json = forge::codec::json;
namespace schema = forge::schema;
using document = att::document<forge::variant_object>;
enum class refusal { key = 1, scope, revoked, stale };
FORGE_DECLARE_EXCEPTION_CATEGORY(refusal, "fixture.attestation.policy")
using key_denied = forge::exceptions::coded_exception<refusal, refusal::key>;
using scope_denied = forge::exceptions::coded_exception<refusal, refusal::scope>;
using revoked = forge::exceptions::coded_exception<refusal, refusal::revoked>;
using stale = forge::exceptions::coded_exception<refusal, refusal::stale>;

jose::trusted_key key_a() {
   const auto point = forge::codec::hex::decode(
       "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
       "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
   jose::trusted_key result{.kid = "key-a"};
   std::copy(point.begin(), point.end(), result.public_key.begin());
   return result;
}
jose::trusted_key key_b() {
   jose::trusted_key result{.kid = "key-b"};
   result.public_key[0] = 4;
   const auto options = forge::codec::base64::decode_options{.characters = forge::codec::base64::alphabet::url,
                                                            .pad = forge::codec::base64::padding_policy::forbid};
   const auto x = forge::codec::base64::decode("f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU", options);
   const auto y = forge::codec::base64::decode("x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0", options);
   std::copy(x.begin(), x.end(), result.public_key.begin() + 1);
   std::copy(y.begin(), y.end(), result.public_key.begin() + 33);
   return result;
}
jose::trusted_keys snapshot(std::string domain = "domain-a", std::string profile = "profile-a",
                            std::vector<jose::trusted_key> keys = {key_a(), key_b()},
                            std::string typ = std::string{att::token_type}) {
   return {{std::move(domain), std::move(profile), 7}, std::move(keys), {.typ = std::move(typ)}};
}
att::verification_policy profile() {
   return {.issuer = "urn:fixture:issuer", .audience = "urn:fixture:resource", .purpose = "fixture.read",
           .schema = "urn:fixture:grant:v1", .max_lifetime = std::chrono::seconds{300}};
}
json::read_options strict_options() {
   return {.max_depth = att::max_json_depth, .unknown_fields = json::unknown_field_policy::error,
           .described_records = json::described_record_policy::exact, .max_bytes = att::max_payload_bytes,
           .max_nodes = att::max_json_nodes, .max_container_size = att::max_container_size,
           .max_string_bytes = att::max_string_bytes, .reject_nul = true};
}
document grant(std::string delegation = "delegation-a", std::string generation = "7", std::string object = "object-1") {
   auto range = forge::mutable_variant_object{}("offset", "9007199254740993")("length", "1024");
   auto resource = forge::mutable_variant_object{}("object", object)("range", range)("provider", "provider-1");
   auto binding = forge::mutable_variant_object{}("id", delegation)("generation", generation);
   auto claims = forge::mutable_variant_object{}("delegation", binding)("resource", resource);
   const auto expected = profile();
   return {.iss = expected.issuer, .aud = expected.audience, .jti = "g_" + std::string(64, 'a'),
           .iat = 1000, .nbf = 1000, .exp = 1300,
           .att = {.purpose = expected.purpose, .schema = expected.schema, .claims = claims}};
}
std::string seal(std::string_view payload) {
   auto prepared = jose::prepare(payload, key_a(), {.typ = std::string{att::token_type}});
   const auto key = forge::crypto::asymmetric::p256::private_key::regenerate(
       forge::crypto::digest::sha256{std::string(63, '0') + '1'}); // Disposable generator scalar, never a product key.
   const auto signature = key.sign_compact(prepared.signing_digest());
   return prepared.complete(std::span<const std::uint8_t>{signature}.subspan(1));
}
std::string issue(document value = grant()) {
   const auto encoded = json::write(value);
   BOOST_REQUIRE(encoded.ok());
   return seal(encoded.text);
}
schema::artifact claims_schema() {
   schema::constraint id{.kind = schema::constraint_kind::string};
   id.min_size = 1; id.max_size = 128; id.ascii = true;
   auto decimal = id;
   decimal.pattern = "^(0|[1-9][0-9]*)$";
   decimal.decimal_minimum = 0; decimal.decimal_maximum = std::numeric_limits<std::uint64_t>::max();
   schema::constraint range{.kind = schema::constraint_kind::object};
   range.properties = {{"offset", decimal}, {"length", decimal}}; range.required = {"offset", "length"};
   schema::constraint resource{.kind = schema::constraint_kind::object};
   resource.properties = {{"object", id}, {"range", range}, {"provider", id}};
   resource.required = {"object", "range", "provider"};
   schema::constraint delegation{.kind = schema::constraint_kind::object};
   delegation.properties = {{"id", id}, {"generation", decimal}}; delegation.required = {"id", "generation"};
   schema::constraint root{.kind = schema::constraint_kind::object};
   root.properties = {{"delegation", delegation}, {"resource", resource}}; root.required = {"delegation", "resource"};
   return {.root = std::move(root)};
}
struct request { std::string object = "object-1", offset = "9007199254740993", length = "1024", provider = "provider-1"; };
struct delegation { std::string id, key_identity, generation = "7"; bool revoked = false; };
struct control_state {
   std::array<delegation, 2> bindings{{{"delegation-a", jose::identify_key(key_a()).identity},
                                      {"delegation-b", jose::identify_key(key_b()).identity}}};
   std::int64_t fresh_until = 1301;
};
// Explicit J10 reference fixture only: not a reusable policy/cache/runtime or a bearer/PoP decision.
struct single_entry_cache {
   struct entry { std::string compact, snapshot_identity; jose::verified_payload verified; };
   std::optional<entry> cached;
   unsigned verifications = 0, hits = 0;
   const jose::verified_payload& authenticate(std::string_view token, const jose::trusted_keys& keys) {
      if (!cached || cached->compact != token || cached->snapshot_identity != keys.identity()) {
         ++verifications;
         const auto verified = jose::verifier{}.verify(token, keys);
         cached.emplace(entry{std::string{token}, keys.identity(), verified});
      } else { ++hits; }
      return cached->verified;
   }
};
void check(single_entry_cache& cache, std::string_view token, const jose::trusted_keys& keys,
           const request& actual, const control_state& control, std::int64_t now = 1100) {
   const auto& verified = cache.authenticate(token, keys);
   const auto parsed = json::read<document>(verified.payload(), strict_options());
   if (!parsed.ok()) FORGE_THROW_EXCEPTION(att::exceptions::invalid_document, "fixture JSON rejected");
   parsed.value.validate(profile(), std::chrono::sys_seconds{std::chrono::seconds{now}});
   const auto& claims = parsed.value.att.claims;
   if (!schema::validate(claims_schema(), forge::variant_schema::to_input(forge::variant{claims})).empty())
      FORGE_THROW_EXCEPTION(att::exceptions::invalid_document, "fixture claims rejected");
   if (now >= control.fresh_until) FORGE_THROW_EXCEPTION(stale, "fixture control state is stale");
   const auto& binding = claims["delegation"].get_object();
   const auto selected = std::ranges::find(control.bindings, binding["id"].get_string(), &delegation::id);
   if (selected == control.bindings.end() || selected->key_identity != verified.key_binding().identity)
      FORGE_THROW_EXCEPTION(key_denied, "fixture actual key does not own delegation");
   if (selected->revoked || selected->generation != binding["generation"].get_string())
      FORGE_THROW_EXCEPTION(revoked, "fixture delegation generation is revoked");
   const auto& resource = claims["resource"].get_object();
   const auto& range = resource["range"].get_object();
   if (resource["object"].get_string() != actual.object || resource["provider"].get_string() != actual.provider ||
       range["offset"].get_string() != actual.offset || range["length"].get_string() != actual.length)
      FORGE_THROW_EXCEPTION(scope_denied, "fixture request does not match exact grant scope");
}
} // namespace attestation_policy_fixture

using namespace attestation_policy_fixture;

BOOST_AUTO_TEST_CASE(j10_actual_key_must_match_delegation) {
   single_entry_cache cache; const auto keys = snapshot(); const control_state control;
   BOOST_REQUIRE_NO_THROW(check(cache, issue(), keys, {}, control));
   BOOST_CHECK_THROW(check(cache, issue(grant("delegation-b")), keys, {}, control), key_denied);
   BOOST_TEST(cache.verifications == 2U); BOOST_TEST(cache.hits == 0U);
   BOOST_TEST(cache.cached->verified.key_binding().identity == control.bindings[0].key_identity);
   BOOST_TEST(cache.cached->verified.key_binding().identity != control.bindings[1].key_identity);
   BOOST_TEST(cache.cached->verified.trust_snapshot().keys().size() == 2U);
}

BOOST_AUTO_TEST_CASE(j10_scope_revoke_staleness_and_expiry_are_rechecked_on_cache_hits) {
   single_entry_cache cache; const auto keys = snapshot(); const auto token = issue(); control_state control;
   BOOST_REQUIRE_NO_THROW(check(cache, token, keys, {}, control));
   BOOST_CHECK_NO_THROW(check(cache, token, keys, {}, control));
   for (const auto& change : {std::pair{&request::object, "object-2"}, {&request::offset, "9007199254740994"},
                              {&request::length, "1025"}, {&request::provider, "provider-2"}}) {
      request changed; changed.*change.first = change.second;
      BOOST_CHECK_THROW(check(cache, token, keys, changed, control), scope_denied);
   }
   control.bindings[0].generation = "8";
   BOOST_CHECK_THROW(check(cache, token, keys, {}, control), revoked);
   control.bindings[0].generation = "7"; control.bindings[0].revoked = true;
   BOOST_CHECK_THROW(check(cache, token, keys, {}, control), revoked);
   control.bindings[0].revoked = false; control.fresh_until = 1100;
   BOOST_CHECK_THROW(check(cache, token, keys, {}, control), stale);
   control.fresh_until = 1301;
   BOOST_CHECK_THROW(check(cache, token, keys, {}, control, 1300), att::exceptions::expired);
   BOOST_TEST(cache.verifications == 1U); BOOST_TEST(cache.hits == 9U);
}

BOOST_AUTO_TEST_CASE(j10_equal_revision_domain_profile_and_keyset_changes_partition_cache) {
   single_entry_cache cache; const auto token = issue(); const control_state control;
   const std::array<jose::trusted_keys, 4> partitions{snapshot(), snapshot("domain-b"),
       snapshot("domain-b", "profile-b"), snapshot("domain-b", "profile-b", {key_a()})};
   for (const auto& keys : partitions) {
      const auto previous = cache.verifications;
      BOOST_REQUIRE_NO_THROW(check(cache, token, keys, {}, control));
      BOOST_TEST(keys.descriptor().revision == 7U); BOOST_TEST(cache.verifications == previous + 1);
      BOOST_TEST(cache.cached->snapshot_identity == keys.identity());
      BOOST_TEST(cache.cached->verified.trust_snapshot().identity() == keys.identity());
      BOOST_CHECK_NO_THROW(check(cache, token, keys, {}, control));
      BOOST_TEST(cache.verifications == previous + 1);
   }
   BOOST_TEST(cache.verifications == 4U); BOOST_TEST(cache.hits == 4U);
   BOOST_CHECK_NO_THROW(check(cache, token, partitions.front(), {}, control));
   BOOST_TEST(cache.verifications == 5U); // One entry: returning to a prior partition must verify again.
   BOOST_CHECK_THROW(check(cache, token, snapshot("domain-c", "profile-c", {key_b()}), {}, control),
                     jose::exceptions::unknown_key);
   BOOST_TEST(cache.verifications == 6U); BOOST_TEST(cache.hits == 4U);
}

BOOST_AUTO_TEST_CASE(j10_key_delete_replace_and_header_switch_cannot_reuse_cached_success) {
   single_entry_cache cache; const auto token = issue(); const auto original = snapshot(); const control_state control;
   BOOST_REQUIRE_NO_THROW(check(cache, token, original, {}, control));
   BOOST_CHECK_THROW(check(cache, token, snapshot("domain-a", "profile-a", {key_b()}), {}, control),
                     jose::exceptions::unknown_key);
   auto replacement = key_b(); replacement.kid = "key-a";
   BOOST_CHECK_THROW(check(cache, token, snapshot("domain-a", "profile-a", {replacement, key_b()}), {}, control),
                     jose::exceptions::invalid_signature);
   BOOST_CHECK_THROW(check(cache, token, snapshot("domain-a", "profile-a", {key_a(), key_b()}, "other+jws"), {}, control),
                     jose::exceptions::invalid_header);
   BOOST_TEST(cache.verifications == 4U); BOOST_TEST(cache.hits == 0U);
   BOOST_TEST(cache.cached->snapshot_identity == original.identity()); // Failed verification publishes no replacement.
}

BOOST_AUTO_TEST_CASE(j10_exact_token_not_jti_keys_cache_and_strict_claim_profile_is_enforced) {
   single_entry_cache cache; const auto keys = snapshot(); const control_state control; const auto token = issue();
   BOOST_REQUIRE_NO_THROW(check(cache, token, keys, {}, control));
   const auto changed_token = issue(grant("delegation-a", "7", "object-2")); // Same issuer/jti, different signed scope.
   BOOST_CHECK_THROW(check(cache, changed_token, keys, {}, control), scope_denied);
   BOOST_TEST(cache.verifications == 2U); BOOST_TEST(cache.hits == 0U);
   request changed; changed.object = "object-2";
   BOOST_CHECK_NO_THROW(check(cache, changed_token, keys, changed, control));
   BOOST_TEST(cache.hits == 1U);
   BOOST_CHECK_NO_THROW(check(cache, token, keys, {}, control));
   BOOST_TEST(cache.verifications == 3U);
   for (const auto generation : {"07", "18446744073709551616"})
      BOOST_CHECK_THROW(check(cache, issue(grant("delegation-a", generation)), keys, {}, control), att::exceptions::invalid_document);
   auto wrong_profile = grant(); wrong_profile.aud = "urn:fixture:other-resource";
   BOOST_CHECK_THROW(check(cache, issue(wrong_profile), keys, {}, control), att::exceptions::profile_mismatch);
   const auto encoded = json::write(grant()); BOOST_REQUIRE(encoded.ok());
   auto duplicate = encoded.text; duplicate.insert(1, R"("aud":"urn:fixture:resource",)");
   BOOST_CHECK_THROW(check(cache, seal(duplicate), keys, {}, control), att::exceptions::invalid_document);
   BOOST_TEST(cache.verifications == 7U); BOOST_TEST(cache.hits == 1U);
}
