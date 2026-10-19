#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import forge.crypto.jose.codec;
import forge.exceptions;
import forge.codec.base64;
import forge.codec.hex;

namespace {

namespace jose = forge::crypto::jose;
namespace p256 = forge::crypto::asymmetric::p256;
namespace base64 = forge::codec::base64;
using sha256 = forge::crypto::digest::sha256;

static_assert(!std::is_default_constructible_v<jose::verified_payload>);
static_assert(!std::is_aggregate_v<jose::verified_payload>);
static_assert(!std::is_constructible_v<jose::verified_payload, std::string, jose::trusted_keys, std::size_t>);
static_assert(!std::is_default_constructible_v<jose::prepared_message>);
static_assert(!std::is_copy_constructible_v<jose::prepared_message>);

constexpr auto header = std::string_view{R"({"alg":"ES256","typ":"test+jws","kid":"key-1"})"};
constexpr auto payload = std::string_view{R"({"message":"local JOSE verification"})"};

jose::header_policy policy() { return {.typ = "test+jws"}; }

jose::trusted_key generator_key() {
   const auto point = forge::codec::hex::decode(
       "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
       "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
   auto result = jose::trusted_key{.kid = "key-1"};
   std::copy(point.begin(), point.end(), result.public_key.begin());
   return result;
}

p256::private_key private_key(unsigned int value = 1) {
   return p256::private_key::regenerate(sha256{forge::codec::hex::encode(value, 64)});
}

jose::trusted_key rfc_key() {
   auto result = jose::trusted_key{};
   result.public_key[0] = 4;
   const auto x = base64::decode("f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU",
                               {.characters = base64::alphabet::url, .pad = base64::padding_policy::forbid});
   const auto y = base64::decode("x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0",
                               {.characters = base64::alphabet::url, .pad = base64::padding_policy::forbid});
   std::copy(x.begin(), x.end(), result.public_key.begin() + 1);
   std::copy(y.begin(), y.end(), result.public_key.begin() + 33);
   return result;
}

jose::trusted_keys snapshot(std::vector<jose::trusted_key> keys = {generator_key()},
                            jose::header_policy expected = policy(),
                            std::string domain = "test-domain", std::string profile = "test-profile") {
   return {{std::move(domain), std::move(profile), 7}, std::move(keys), std::move(expected)};
}

std::vector<std::uint8_t> sign(const sha256& hash, unsigned int secret = 1) {
   const auto signature = private_key(secret).sign_compact(hash);
   // Forge's recoverable R1 signature has a recovery byte; JWS carries only R || S.
   return {signature.begin() + 1, signature.end()};
}

std::string b64(std::string_view value) {
   return base64::encode(value, {.characters = base64::alphabet::url, .pad = base64::padding::omit});
}

std::string b64(std::span<const std::uint8_t> value) {
   return base64::encode(value, {.characters = base64::alphabet::url, .pad = base64::padding::omit});
}

// Negative fixtures sign the supplied exact header bytes, so malformed headers
// cannot be rejected merely because of an unrelated signature failure.
std::string fixture(std::string_view protected_header = header, std::string_view content = payload) {
   const auto input = b64(protected_header) + "." + b64(content);
   const auto hash = sha256::hash(input);
   return input + "." + b64(sign(hash));
}

std::string issue(std::string_view content = payload) {
   auto prepared = jose::prepare(content, generator_key(), policy());
   return prepared.complete(sign(prepared.signing_digest()));
}

} // namespace

BOOST_AUTO_TEST_CASE(crypto_jose_rfc7515_a3_known_vector) {
   // RFC 7515 Appendix A.3, also independently qualified by F0-B.
   const auto input = std::string{
       "eyJhbGciOiJFUzI1NiJ9.eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFt"
       "cGxlLmNvbS9pc19yb290Ijp0cnVlfQ"};
   const auto signature = base64::decode(
       "DtEhU3ljbEg8L38VWAfUAqOyKAM6-Xx-F4GawxaepmXFCgfTjDxw5djxLa8ISlSApmWQxfKTUJqPP3-Kg6NU1Q",
       {.characters = base64::alphabet::url, .pad = base64::padding_policy::forbid});
   const auto plaintext = std::string{"{\"iss\":\"joe\",\r\n \"exp\":1300819380,\r\n \"http://example.com/is_root\":true}"};
   const auto expected = jose::header_policy{.key_id = jose::key_id_policy::absent};
   const auto keys = snapshot({rfc_key()}, expected);
   auto prepared = jose::prepare(plaintext, rfc_key(), expected);
   BOOST_TEST(prepared.signing_input() == input);
   BOOST_CHECK(prepared.signing_digest() == sha256::hash(input));
   const auto compact = prepared.complete(signature);
   BOOST_TEST(compact == input + "." + b64(signature));
   const auto verified = jose::verifier{}.verify(compact, keys);
   BOOST_TEST(verified.payload() == plaintext);
   BOOST_TEST(verified.key_binding().identity == jose::identify_key(rfc_key()).identity);
   BOOST_TEST(verified.trust_snapshot().identity() == keys.identity());
}

BOOST_AUTO_TEST_CASE(crypto_jose_bounded_mutation_corpus_rejects_changed_signed_bytes) {
   const auto original = issue();
   const auto keys = snapshot();
   const auto verifier = jose::verifier{};
   for (std::size_t index = 0; index != 5000; ++index) {
      auto changed = original;
      const auto position = (index * 37U + 11U) % changed.size();
      changed[position] = static_cast<char>(static_cast<unsigned char>(changed[position]) ^ (1U << (index % 7U)));
      bool rejected = false;
      try {
         static_cast<void>(verifier.verify(changed, keys));
      } catch (const forge::exceptions::base&) {
         rejected = true;
      }
      BOOST_CHECK(rejected);
   }
}

BOOST_AUTO_TEST_CASE(crypto_jose_owns_prepared_inputs_and_verifies_real_signature) {
   auto key = generator_key();
   auto expected = policy();
   auto content = std::string{payload};
   auto prepared = jose::prepare(content, key, expected);
   const auto input = std::string{prepared.signing_input()};
   const auto hash = prepared.signing_digest();
   key.public_key.fill(0);
   key.kid = "substitution";
   expected.typ = "different+jws";
   content.assign(content.size(), 'x');
   BOOST_CHECK(hash == sha256::hash(input));
   BOOST_TEST(prepared.key_binding().kid == "key-1");
   auto signature = sign(hash);
   const auto compact = prepared.complete(signature);
   signature.assign(signature.size(), 0);
   BOOST_TEST(prepared.signing_input() == input);
   BOOST_TEST(compact.substr(0, compact.rfind('.')) == input);
   const auto verified = jose::verifier{}.verify(compact, snapshot());
   BOOST_TEST(verified.payload() == payload);
   BOOST_CHECK(verified.key_binding().public_key == jose::identify_key(generator_key()).public_key);
   BOOST_CHECK_THROW(static_cast<void>(prepared.complete(signature)), jose::exceptions::invalid_state);
}

BOOST_AUTO_TEST_CASE(crypto_jose_complete_rejects_substitution_and_remains_retryable) {
   auto prepared = jose::prepare(payload, generator_key(), policy());
   auto other = jose::prepare("different payload", generator_key(), policy());
   auto changed_policy = policy();
   changed_policy.typ = "different+jws";
   auto changed_header = jose::prepare(payload, generator_key(), changed_policy);
   const auto good = sign(prepared.signing_digest());
   auto tampered = good;
   tampered[0] ^= 1;
   const auto bad = std::vector<std::vector<std::uint8_t>>{
       {}, std::vector<std::uint8_t>(63), std::vector<std::uint8_t>(65), std::vector<std::uint8_t>(64), tampered,
       sign(prepared.signing_digest(), 2), sign(other.signing_digest()), sign(changed_header.signing_digest()),
       sign(sha256::hash(prepared.signing_digest()))};
   for (const auto& signature : bad) {
      BOOST_CHECK_THROW(static_cast<void>(prepared.complete(signature)), jose::exceptions::invalid_signature);
   }
   const auto der = p256::sign_der(private_key(), std::span<const std::uint8_t>{
       reinterpret_cast<const std::uint8_t*>(prepared.signing_input().data()), prepared.signing_input().size()});
   BOOST_CHECK_THROW(static_cast<void>(prepared.complete(der)), jose::exceptions::invalid_signature);
   const auto compact = prepared.complete(good);
   const auto proof = jose::verifier{}.verify(compact, snapshot());
   BOOST_TEST(proof.payload() == payload);
   const auto second = other.complete(sign(other.signing_digest()));
   const auto second_proof = jose::verifier{}.verify(second, snapshot());
   BOOST_TEST(second_proof.payload() == "different payload");
}

BOOST_AUTO_TEST_CASE(crypto_jose_prepared_move_has_typed_invalid_state) {
   auto source = jose::prepare(payload, generator_key(), policy());
   auto destination = std::move(source);
   BOOST_CHECK_THROW(static_cast<void>(source.signing_input()), jose::exceptions::invalid_state);
   BOOST_CHECK_THROW(static_cast<void>(source.signing_digest()), jose::exceptions::invalid_state);
   BOOST_CHECK_THROW(static_cast<void>(source.key_binding()), jose::exceptions::invalid_state);
   BOOST_CHECK_THROW(static_cast<void>(source.complete(std::array<std::uint8_t, 64>{})), jose::exceptions::invalid_state);
   const auto compact = destination.complete(sign(destination.signing_digest()));
   const auto proof = jose::verifier{}.verify(compact, snapshot());
   BOOST_TEST(proof.payload() == payload);
}

BOOST_AUTO_TEST_CASE(crypto_jose_retains_actual_key_and_immutable_trust_snapshot) {
   auto key = generator_key();
   auto expected = policy();
   auto descriptor = jose::snapshot_descriptor{"domain-a", "profile-a", 7};
   auto original = snapshot();
   const auto compact = issue();
   const auto proof = [&] {
      const auto keys = jose::trusted_keys{descriptor, {key}, expected};
      original = keys;
      return jose::verifier{}.verify(compact, keys);
   }();
   const auto identity = proof.trust_snapshot().identity();
   key.public_key.fill(0);
   expected.typ = "different+jws";
   descriptor.trust_domain = "mutated";
   original = snapshot();
   BOOST_TEST(proof.trust_snapshot().identity() == identity);
   BOOST_TEST(proof.trust_snapshot().descriptor().trust_domain == "domain-a");
   BOOST_TEST(proof.key_binding().identity == jose::identify_key(generator_key()).identity);
   BOOST_TEST(proof.payload() == payload);
}

BOOST_AUTO_TEST_CASE(crypto_jose_snapshot_identity_partitions_domains_profiles_keys_and_policy) {
   const auto base = snapshot();
   const auto other_domain = snapshot({generator_key()}, policy(), "domain-b");
   const auto other_profile = snapshot({generator_key()}, policy(), "test-domain", "profile-b");
   auto other_key = rfc_key();
   other_key.kid = "key-1";
   const auto replaced = snapshot({other_key});
   auto other_policy = policy();
   other_policy.typ = "other+jws";
   const auto changed_policy = snapshot({generator_key()}, other_policy);
   other_policy = policy();
   --other_policy.bounds.payload_bytes;
   const auto changed_limits = snapshot({generator_key()}, other_policy);
   const auto changed_revision = jose::trusted_keys{{"test-domain", "test-profile", 8}, {generator_key()}, policy()};
   for (const auto* alternative : {&other_domain, &other_profile, &replaced, &changed_policy, &changed_limits,
                                   &changed_revision}) {
      BOOST_TEST(alternative->identity() != base.identity());
   }
   const auto compact = issue();
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(compact, replaced)), jose::exceptions::invalid_signature);
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(compact, changed_policy)), jose::exceptions::invalid_header);
   auto deleted = generator_key();
   deleted.kid = "another-key";
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(compact, snapshot({deleted}))), jose::exceptions::unknown_key);
   const auto proof_a = jose::verifier{}.verify(compact, base);
   const auto proof_b = jose::verifier{}.verify(compact, other_domain);
   BOOST_TEST(proof_a.trust_snapshot().identity() != proof_b.trust_snapshot().identity());
}

BOOST_AUTO_TEST_CASE(crypto_jose_keyset_order_is_stable_and_actual_selected_key_is_preserved) {
   auto first = generator_key();
   auto second = rfc_key();
   second.kid = "key-2";
   const auto left = snapshot({first, second});
   const auto right = snapshot({second, first});
   BOOST_TEST(left.identity() == right.identity());
   const auto proof = jose::verifier{}.verify(issue(), right);
   BOOST_TEST(proof.key_binding().kid == "key-1");
   BOOST_TEST(proof.key_binding().identity != jose::identify_key(second).identity);
   BOOST_CHECK_THROW(static_cast<void>(snapshot({first, first})), jose::exceptions::invalid_key);
   second.kid = first.kid;
   BOOST_CHECK_THROW(static_cast<void>(snapshot({first, second})), jose::exceptions::invalid_key);
}

BOOST_AUTO_TEST_CASE(crypto_jose_rejects_signed_header_policy_bypasses) {
   const auto invalid = std::vector<std::string>{
       R"({"alg":"none","typ":"test+jws","kid":"key-1"})",
       R"({"alg":"HS256","typ":"test+jws","kid":"key-1"})",
       R"({"alg":"ES384","typ":"test+jws","kid":"key-1"})",
       R"({"alg":"ES256","typ":"at+jwt","kid":"key-1"})",
       R"({"alg":"ES256","typ":"test+jws"})",
       R"({"alg":"ES256","kid":"key-1"})",
       R"({"typ":"test+jws","kid":"key-1"})",
       R"({"alg":"ES256","typ":"test+jws","kid":""})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key/1"})",
       R"({"alg":"ES256","typ":"test+jws","kid":null})",
       R"({"alg":"ES256","typ":true,"kid":"key-1"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","kid":"key-1"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","k\u0069d":"key-1"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","jwk":{}})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","jku":"https://invalid.example/keys"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","x5u":"https://invalid.example/cert"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","x5c":[]})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","crit":[]})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","cty":"JWT"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","b64":true})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","b64":false})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1","extra":0})",
       R"(["ES256","test+jws","key-1"])",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1"}{})",
       R"({"alg":"ES256","typ":"test+jws","kid":"key-1\u0000"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"\ud800"})",
       R"({"alg":"ES256","typ":"test+jws","kid":"\udc00"})",
       std::string{R"({"alg":"ES256","typ":"test+jws","kid":")"} + char(0xff) + "\"}",
       std::string{R"({"alg":"ES256","typ":"test+jws","kid":")"} + char(0) + "\"}",
       std::string{R"({"alg":"ES256","typ":"test+jws","kid":")"} + char(1) + "\"}",
       std::string{header} + "true",
       "{", "null", "true",
   };
   const auto keys = snapshot();
   for (const auto& candidate : invalid) {
      BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(fixture(candidate), keys)), jose::exceptions::invalid_header);
   }
   const auto valid = fixture(" \n { \"k\\u0069d\" : \"key-1\", \"typ\" : \"test+jws\", \"alg\" : \"ES256\" }\r\n");
   const auto proof = jose::verifier{}.verify(valid, keys);
   BOOST_TEST(proof.payload() == payload);
}

BOOST_AUTO_TEST_CASE(crypto_jose_profile_neutral_policy_requires_exact_header_shape) {
   const auto untyped_policy = jose::header_policy{};
   auto untyped = jose::prepare(payload, generator_key(), untyped_policy);
   const auto untyped_token = untyped.complete(sign(untyped.signing_digest()));
   const auto untyped_keys = snapshot({generator_key()}, untyped_policy);
   const auto untyped_proof = jose::verifier{}.verify(untyped_token, untyped_keys);
   BOOST_TEST(untyped_proof.payload() == payload);
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(issue(), untyped_keys)), jose::exceptions::invalid_header);
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(untyped_token, snapshot())), jose::exceptions::invalid_header);
   auto no_kid = policy();
   no_kid.key_id = jose::key_id_policy::absent;
   auto key = generator_key();
   key.kid.clear();
   auto no_kid_prepared = jose::prepare(payload, key, no_kid);
   const auto no_kid_token = no_kid_prepared.complete(sign(no_kid_prepared.signing_digest()));
   const auto no_kid_proof = jose::verifier{}.verify(no_kid_token, snapshot({key}, no_kid));
   BOOST_TEST(no_kid_proof.key_binding().kid.empty());
   BOOST_CHECK_THROW(static_cast<void>(jose::prepare(payload, generator_key(), no_kid)), jose::exceptions::invalid_key);
   BOOST_CHECK_THROW(static_cast<void>(snapshot({generator_key()}, no_kid)), jose::exceptions::invalid_policy);
   BOOST_CHECK_THROW(static_cast<void>(snapshot({key, key}, no_kid)), jose::exceptions::invalid_policy);
}

BOOST_AUTO_TEST_CASE(crypto_jose_rejects_compact_tampering_and_noncanonical_segments) {
   const auto compact = fixture();
   const auto last = compact.rfind('.');
   const auto input = compact.substr(0, last);
   const auto signature = compact.substr(last + 1);
   const auto first = compact.find('.');
   const auto encoded_header = compact.substr(0, first);
   const auto encoded_payload = compact.substr(first + 1, last - first - 1);
   const auto invalid = std::vector<std::string>{
       "", ".", "..", compact + ".x", compact + " ", compact + std::string(1, '\0'),
       "." + encoded_payload + "." + signature, encoded_header + ".." + signature, input + ".",
       encoded_header + "=." + encoded_payload + "." + signature,
       encoded_header + "." + encoded_payload + "=." + signature,
       input + "." + signature + "=", input + "." + signature.substr(0, 85),
       input + "." + signature + "A", " " + compact, compact + "\r\n",
       encoded_header + "." + encoded_payload + "." + std::string(86, '+'),
   };
   const auto keys = snapshot();
   for (const auto& candidate : invalid) {
      BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(candidate, keys)), forge::exceptions::context_error);
   }
   const auto changed_payload = encoded_header + "." + b64("different payload") + "." + signature;
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(changed_payload, keys)), jose::exceptions::invalid_signature);
   const auto changed_header = b64(R"({"kid":"key-1","typ":"test+jws","alg":"ES256"})") +
                               "." + encoded_payload + "." + signature;
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(changed_header, keys)), jose::exceptions::invalid_signature);
   auto tampered = compact;
   tampered[last + 1] = tampered[last + 1] == 'A' ? 'B' : 'A';
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(tampered, keys)), jose::exceptions::invalid_signature);
   auto noncanonical = compact;
   const auto alphabet = std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};
   const auto trailing = alphabet.find(noncanonical.back());
   BOOST_REQUIRE(trailing != alphabet.npos);
   noncanonical.back() = alphabet[trailing | 1U];
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(noncanonical, keys)), jose::exceptions::invalid_compact);
}

BOOST_AUTO_TEST_CASE(crypto_jose_byte_limits_are_inclusive_and_only_reducible) {
   const auto keys = snapshot();
   const auto large_payload = std::string(8'192, 'x');
   const auto compact = issue(large_payload);
   const auto proof = jose::verifier{}.verify(compact, keys);
   BOOST_TEST(proof.payload().size() == 8'192U);
   BOOST_CHECK_THROW(static_cast<void>(jose::prepare(std::string(8'193, 'x'), generator_key(), policy())),
                     jose::exceptions::size_limit_exceeded);
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(fixture(header, std::string(8'193, 'x')), keys)),
                     jose::exceptions::size_limit_exceeded);
   auto padded_header = std::string{header};
   padded_header.resize(512, ' ');
   const auto at_header_limit = jose::verifier{}.verify(fixture(padded_header), keys);
   BOOST_TEST(at_header_limit.payload() == payload);
   padded_header.push_back(' ');
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(fixture(padded_header), keys)),
                     jose::exceptions::size_limit_exceeded);
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(std::string(16'385, 'A'), keys)),
                     jose::exceptions::size_limit_exceeded);
   auto exact = policy();
   const auto issued = issue();
   exact.bounds.compact_bytes = issued.size();
   auto prepared = jose::prepare(payload, generator_key(), exact);
   const auto at_limit = prepared.complete(sign(prepared.signing_digest()));
   const auto accepted = jose::verifier{}.verify(at_limit, snapshot({generator_key()}, exact));
   BOOST_TEST(accepted.payload() == payload);
   --exact.bounds.compact_bytes;
   BOOST_CHECK_THROW(static_cast<void>(jose::prepare(payload, generator_key(), exact)), jose::exceptions::size_limit_exceeded);
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(issued, snapshot({generator_key()}, exact))),
                     jose::exceptions::size_limit_exceeded);
   auto oversized = policy();
   oversized.bounds.payload_bytes = 8'193;
   BOOST_CHECK_THROW(static_cast<void>(snapshot({generator_key()}, oversized)), jose::exceptions::invalid_policy);
   oversized = policy();
   oversized.bounds.compact_bytes = 16'385;
   BOOST_CHECK_THROW(static_cast<void>(snapshot({generator_key()}, oversized)), jose::exceptions::invalid_policy);
   oversized = policy();
   oversized.bounds.protected_header_bytes = 513;
   BOOST_CHECK_THROW(static_cast<void>(snapshot({generator_key()}, oversized)), jose::exceptions::invalid_policy);
   oversized = policy();
   oversized.bounds.payload_bytes = 0;
   BOOST_CHECK_THROW(static_cast<void>(snapshot({generator_key()}, oversized)), jose::exceptions::invalid_policy);
   auto reduced = policy();
   reduced.bounds.payload_bytes = payload.size();
   const auto reduced_proof = jose::verifier{}.verify(issued, snapshot({generator_key()}, reduced));
   BOOST_TEST(reduced_proof.payload() == payload);
   --reduced.bounds.payload_bytes;
   BOOST_CHECK_THROW(static_cast<void>(jose::prepare(payload, generator_key(), reduced)), jose::exceptions::size_limit_exceeded);
   BOOST_CHECK_THROW(static_cast<void>(jose::verifier{}.verify(issued, snapshot({generator_key()}, reduced))),
                     jose::exceptions::size_limit_exceeded);
   auto maximum_kid = generator_key();
   maximum_kid.kid.assign(128, 'x');
   auto maximum_kid_prepared = jose::prepare(payload, maximum_kid, policy());
   const auto maximum_kid_compact = maximum_kid_prepared.complete(sign(maximum_kid_prepared.signing_digest()));
   const auto maximum_kid_proof = jose::verifier{}.verify(maximum_kid_compact, snapshot({maximum_kid}));
   BOOST_TEST(maximum_kid_proof.key_binding().kid == maximum_kid.kid);
}

BOOST_AUTO_TEST_CASE(crypto_jose_rejects_invalid_public_keys_and_policy_without_disclosing_input) {
   auto invalid = generator_key();
   invalid.public_key.fill(0);
   BOOST_CHECK_THROW(static_cast<void>(snapshot({invalid})), jose::exceptions::invalid_key);
   BOOST_CHECK_THROW(static_cast<void>(jose::prepare(payload, invalid, policy())), jose::exceptions::invalid_key);
   invalid.public_key[0] = 4;
   BOOST_CHECK_THROW(static_cast<void>(snapshot({invalid})), jose::exceptions::invalid_key);
   invalid = generator_key();
   invalid.kid.assign(129, 'x');
   BOOST_CHECK_THROW(static_cast<void>(snapshot({invalid})), jose::exceptions::invalid_key);
   BOOST_CHECK_THROW(static_cast<void>(snapshot({})), jose::exceptions::invalid_policy);
   BOOST_CHECK_THROW(static_cast<void>(jose::prepare("", generator_key(), policy())), jose::exceptions::invalid_compact);
   auto expected = policy();
   expected.typ = "";
   BOOST_CHECK_THROW(static_cast<void>(snapshot({generator_key()}, expected)), jose::exceptions::invalid_policy);
   const auto marker = std::string{"credential_must_not_appear_in_diagnostics"};
   try {
      static_cast<void>(jose::verifier{}.verify(fixture("{\"" + marker + "\": true}"), snapshot()));
      BOOST_FAIL("malformed header was accepted");
   } catch (const jose::exceptions::invalid_header& error) {
      BOOST_TEST(std::string_view{error.what()}.find(marker) == std::string_view::npos);
   }
   auto prepared = jose::prepare(payload, generator_key(), policy());
   const auto proof = jose::verifier{}.verify(issue(), snapshot());
   BOOST_TEST(diagnostic_is_secret(prepared));
   BOOST_TEST(diagnostic_is_secret(proof));
}
