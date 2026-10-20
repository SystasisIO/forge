#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

import forge.auth.attestation.document;
import forge.auth.attestation.document_schema;
import forge.auth.attestation.signed_document;
import forge.codec.json;
import forge.codec.hex;
import forge.raw.raw;
import forge.reflect.reflect;
import forge.schema.constraint;
import forge.schema.object;
import forge.variant.schema;
import forge.variant.value;

namespace attestation_fixture {

struct claims {
   std::string counter;
   bool operator==(const claims&) const = default;
};

struct credential_holder {
   forge::auth::attestation::signed_document credential;
};

BOOST_DESCRIBE_STRUCT(claims, (), (counter))
BOOST_DESCRIBE_STRUCT(credential_holder, (), (credential))

} // namespace attestation_fixture

namespace forge::schema {

template <> struct rules<attestation_fixture::claims> {
   static object_schema<attestation_fixture::claims> define() {
      return object<attestation_fixture::claims>()
          .field<&attestation_fixture::claims::counter>("counter")
          .required().length(1, 20).decimal_range(0, std::numeric_limits<std::uint64_t>::max());
   }
};

} // namespace forge::schema

namespace {

namespace att = forge::auth::attestation;
namespace errors = forge::auth::attestation::exceptions;
using typed_document = att::document<attestation_fixture::claims>;
using native_document = att::document<forge::variant_object>;

att::verification_policy policy() {
   return {.issuer = "https://issuer.example",
           .audience = "urn:resource:fixture",
           .purpose = "fixture.read",
           .schema = "urn:fixture:claims:v1",
           .version = 1,
           .max_lifetime = std::chrono::seconds{60},
           .clock_skew = std::chrono::seconds{5}};
}

std::chrono::sys_seconds clock_at(std::int64_t seconds) {
   return std::chrono::sys_seconds{std::chrono::seconds{seconds}};
}

typed_document document() {
   const auto profile = policy();
   return {.iss = profile.issuer,
           .aud = profile.audience,
           .jti = "g_0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
           .iat = 100,
           .nbf = 105,
           .exp = 160,
           .att = {.v = 1, .purpose = profile.purpose, .schema = profile.schema, .claims = {.counter = "9007199254740993"}}};
}

forge::codec::json::read_options strict_options() {
   return {.max_depth = att::max_json_depth,
           .unknown_fields = forge::codec::json::unknown_field_policy::error,
           .described_records = forge::codec::json::described_record_policy::exact,
           .max_bytes = att::max_payload_bytes,
           .max_nodes = att::max_json_nodes,
           .max_container_size = att::max_container_size,
           .max_string_bytes = att::max_string_bytes};
}

constexpr auto payload = std::string_view{
    R"({"iss":"https://issuer.example","aud":"urn:resource:fixture","jti":"g_0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","iat":100,"nbf":105,"exp":160,"att":{"v":1,"purpose":"fixture.read","schema":"urn:fixture:claims:v1","claims":{"counter":"9007199254740993","nested":{"list":[null,true,7,"18446744073709551615"]}}}})"};

} // namespace

BOOST_AUTO_TEST_SUITE(auth_attestation)

BOOST_AUTO_TEST_CASE(shared_fields_are_physically_named_and_raw_uses_one_model) {
   static_assert(std::same_as<decltype(typed_document{}.iat), std::int64_t>);
   static_assert(std::same_as<decltype(typed_document{}.nbf), std::int64_t>);
   static_assert(std::same_as<decltype(typed_document{}.exp), std::int64_t>);
   auto names = std::vector<std::string>{};
   forge::reflect::for_each_member<typed_document>([&](const char* name, auto) { names.emplace_back(name); });
   const auto expected = std::vector<std::string>{"iss", "aud", "jti", "iat", "nbf", "exp", "att"};
   BOOST_CHECK(names == expected);
   names.clear();
   forge::reflect::for_each_member<att::extension<attestation_fixture::claims>>(
       [&](const char* name, auto) { names.emplace_back(name); });
   BOOST_CHECK(names == (std::vector<std::string>{"v", "purpose", "schema", "claims"}));

   const auto value = document();
   const auto raw = forge::raw::pack(value);
   const auto golden = std::string_view{
       "1668747470733a2f2f6973737565722e6578616d706c651475726e3a7265736f757263653a66697874757265"
       "42675f303132333435363738396162636465663031323334353637383961626364656630313233343536373839"
       "6162636465663031323334353637383961626364656664000000000000006900000000000000a000000000000000"
       "01000000000000000c666978747572652e726561641575726e3a666978747572653a636c61696d733a7631"
       "1039303037313939323534373430393933"};
   BOOST_CHECK(forge::codec::hex::encode(raw) == golden);
   const auto fields = std::tuple{value.iss, value.aud, value.jti, value.iat, value.nbf, value.exp,
                                  std::tuple{value.att.v, value.att.purpose, value.att.schema,
                                             std::tuple{value.att.claims.counter}}};
   BOOST_CHECK(raw == forge::raw::pack(fields));
   BOOST_CHECK(forge::raw::unpack_exact<typed_document>(raw) == value);
}

BOOST_AUTO_TEST_CASE(exact_profile_and_validity_are_checked_without_mutation) {
   const auto value = document();
   BOOST_CHECK_NO_THROW(value.validate(policy(), clock_at(100)));
   BOOST_CHECK_NO_THROW(value.validate(policy(), clock_at(159)));
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(160)), errors::expired);
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(165)), errors::expired);
   BOOST_TEST(value.exp == 160);

   auto expected = policy();
   expected.clock_skew = std::chrono::seconds{0};
   BOOST_CHECK_THROW(value.validate(expected, clock_at(104)), errors::not_yet_valid);
   BOOST_CHECK_NO_THROW(value.validate(expected, clock_at(105)));
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(99)), errors::not_yet_valid);
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(-1)), errors::invalid_profile);
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(std::numeric_limits<std::int64_t>::max())), errors::expired);

   for (auto member : {&att::verification_policy::issuer, &att::verification_policy::audience,
                       &att::verification_policy::purpose, &att::verification_policy::schema}) {
      auto wrong = policy();
      wrong.*member += "x";
      BOOST_CHECK_THROW(value.validate(wrong, clock_at(105)), errors::profile_mismatch);
   }
}

BOOST_AUTO_TEST_CASE(time_and_profile_extremes_fail_closed_without_overflow) {
   for (const auto bad : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                          att::max_numeric_date + 1, std::numeric_limits<std::int64_t>::max()}) {
      for (auto member : {&typed_document::iat, &typed_document::nbf, &typed_document::exp}) {
         auto value = document();
         value.*member = bad;
         BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::invalid_document);
      }
   }
   auto value = document();
   value.nbf = value.exp;
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::invalid_document);
   value = document();
   value.iat = value.nbf + 1;
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::invalid_document);
   value = document();
   ++value.exp;
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::invalid_document);

   for (const auto bad : {std::chrono::seconds::min(), std::chrono::seconds{-1}, std::chrono::seconds{0},
                          std::chrono::seconds::max()}) {
      auto expected = policy();
      expected.max_lifetime = bad;
      BOOST_CHECK_THROW(value.validate(expected, clock_at(105)), errors::invalid_profile);
   }
   for (const auto bad : {std::chrono::seconds::min(), std::chrono::seconds{-1}, std::chrono::seconds::max()}) {
      auto expected = policy();
      expected.clock_skew = bad;
      BOOST_CHECK_THROW(value.validate(expected, clock_at(105)), errors::invalid_profile);
   }
   auto expected = policy();
   expected.version = 2;
   BOOST_CHECK_THROW(document().validate(expected, clock_at(105)), errors::invalid_profile);
   value = document();
   value.att.v = 2;
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::unsupported_version);

   value = document();
   value.iat = att::max_numeric_date - 60;
   value.nbf = value.iat;
   value.exp = att::max_numeric_date;
   BOOST_CHECK_NO_THROW(value.validate(policy(), clock_at(value.iat)));
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(value.exp)), errors::expired);
}

BOOST_AUTO_TEST_CASE(identifier_bounds_and_exact_identity_do_not_normalize) {
   for (const auto& id : {std::string{}, std::string(66, 'a'), std::string{"g_"} + std::string(64, 'A'),
                          std::string{"g_"} + std::string(63, 'a'), std::string{"g_"} + std::string(65, 'a')}) {
      auto value = document();
      value.jti = id;
      BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::invalid_document);
   }
   for (const auto& uri : {std::string{}, std::string{"relative/path"}, std::string{"1urn:bad"},
                           std::string{"urn:bad%2g"}, std::string{"https://issuer.example/ bad"},
                           std::string{"urn:"} + std::string(253, 'a'), std::string{"urn:bad\0tail", 12}}) {
      auto value = document();
      value.iss = uri;
      BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::invalid_document);
   }
   auto value = document();
   value.iss += '/';
   BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::profile_mismatch);
   for (const auto& purpose : {std::string{}, std::string{"Fixture.read"}, std::string{"fixture/read"},
                               std::string(65, 'a')}) {
      value = document();
      value.att.purpose = purpose;
      BOOST_CHECK_THROW(value.validate(policy(), clock_at(105)), errors::invalid_document);
   }
   auto expected = policy();
   expected.issuer = "urn:" + std::string(252, 'a');
   expected.purpose = std::string(64, 'a');
   value = document();
   value.iss = expected.issuer;
   value.att.purpose = expected.purpose;
   BOOST_CHECK_NO_THROW(value.validate(expected, clock_at(105)));
}

BOOST_AUTO_TEST_CASE(native_dynamic_claims_roundtrip_is_nested_and_lossless) {
   const auto decoded = forge::codec::json::read<native_document>(payload, strict_options());
   BOOST_REQUIRE(decoded.ok());
   BOOST_CHECK_NO_THROW(decoded.value.validate(policy(), clock_at(105)));
   BOOST_TEST(decoded.value.att.claims["counter"].as_string() == "9007199254740993");
   const auto& list = decoded.value.att.claims["nested"].get_object()["list"].get_array();
   BOOST_REQUIRE_EQUAL(list.size(), 4);
   BOOST_CHECK(list[0].is_null());
   BOOST_CHECK(list[1].is_bool());
   BOOST_CHECK(list[2].is_int64() || list[2].is_uint64());
   BOOST_TEST(list[3].as_string() == "18446744073709551615");
   const auto encoded = forge::codec::json::write(decoded.value);
   BOOST_REQUIRE(encoded.ok());
   const auto again = forge::codec::json::read<native_document>(encoded.text, strict_options());
   BOOST_REQUIRE(again.ok());
   BOOST_CHECK(again.value == decoded.value);
   BOOST_CHECK(forge::raw::unpack_exact<native_document>(forge::raw::pack(decoded.value)) == decoded.value);
}

BOOST_AUTO_TEST_CASE(native_payload_rejects_unknown_fields_and_non_exact_kinds) {
   for (const auto bad : {"null", "[]", "\"opaque\"", "true", "5"}) {
      auto text = std::string{payload};
      const auto begin = text.find("{\"counter\"");
      text.replace(begin, text.size() - begin - 2, bad);
      BOOST_CHECK(!forge::codec::json::read<native_document>(text, strict_options()).ok());
   }
   for (const auto bad : {"100.0", "1e2", "\"100\"", "true", "null", "-1", "253402300800"}) {
      auto text = std::string{payload};
      const auto begin = text.find("\"iat\":100");
      text.replace(begin, 9, std::string{"\"iat\":"} + bad);
      BOOST_CHECK(!forge::codec::json::read<native_document>(text, strict_options()).ok());
   }
   for (const auto name : {"sub", "client_id", "scope", "sid", "cnf", "extra"}) {
      auto text = std::string{payload};
      text.insert(1, std::string{"\""} + name + "\":null,");
      BOOST_CHECK(!forge::codec::json::read<native_document>(text, strict_options()).ok());
   }
   auto text = std::string{payload};
   text.insert(text.find("\"att\":{") + 7, "\"extra\":null,");
   BOOST_CHECK(!forge::codec::json::read<native_document>(text, strict_options()).ok());
}

BOOST_AUTO_TEST_CASE(j8_decoded_typed_intent_equality_ignores_json_order_but_not_field_changes) {
   // Only intent equality is exercised here, not signature verification or the G2 response workflow.
   const auto intended = document();
   const auto original = forge::codec::json::write(intended);
   BOOST_REQUIRE(original.ok());
   constexpr auto reordered_json = std::string_view{
       R"({"exp":160,"att":{"claims":{"counter":"9007199254740993"},"schema":"urn:fixture:claims:v1","purpose":"fixture.read","v":1},"nbf":105,"jti":"g_0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","iat":100,"aud":"urn:resource:fixture","iss":"https://issuer.example"})"};
   BOOST_CHECK(original.text != reordered_json);
   const auto reordered = forge::codec::json::read<typed_document>(reordered_json, strict_options());
   BOOST_REQUIRE(reordered.ok());
   BOOST_CHECK_NO_THROW(reordered.value.validate(policy(), clock_at(105)));
   BOOST_CHECK(reordered.value == intended);

   auto changed_audience = intended;
   changed_audience.aud = "urn:resource:other";
   auto changed_claims = intended;
   changed_claims.att.claims.counter = "9007199254740994";
   auto changed_expiry = intended;
   --changed_expiry.exp;
   BOOST_CHECK_NO_THROW(changed_claims.validate(policy(), clock_at(105)));
   BOOST_CHECK_NO_THROW(changed_expiry.validate(policy(), clock_at(105)));
   for (const auto& changed : {changed_audience, changed_claims, changed_expiry}) {
      const auto encoded = forge::codec::json::write(changed);
      BOOST_REQUIRE(encoded.ok());
      const auto decoded = forge::codec::json::read<typed_document>(encoded.text, strict_options());
      BOOST_REQUIRE(decoded.ok());
      BOOST_CHECK(decoded.value == changed);
      BOOST_CHECK(decoded.value != intended);
   }
}

BOOST_AUTO_TEST_CASE(product_decimal_claim_rules_export_and_validate_without_double) {
   auto value = document();
   value.att.claims.counter = "18446744073709551615";
   const auto encoded = forge::codec::json::write(value);
   BOOST_REQUIRE(encoded.ok());
   const auto decoded = forge::codec::json::read<typed_document>(encoded.text, strict_options());
   BOOST_REQUIRE(decoded.ok());
   BOOST_TEST(decoded.value.att.claims.counter == value.att.claims.counter);
   const auto artifact = att::document_schema<attestation_fixture::claims>();
   const auto& root = artifact.root;
   BOOST_CHECK(root.kind == forge::schema::constraint_kind::object);
   BOOST_TEST(root.required.size() == 7);
   const auto& counter = root.properties.at("att").properties.at("claims").properties.at("counter");
   BOOST_REQUIRE(counter.decimal_maximum.has_value());
   BOOST_CHECK(*counter.decimal_maximum == std::numeric_limits<std::uint64_t>::max());
   BOOST_CHECK_NO_THROW(forge::schema::json_schema(artifact));
   for (const auto& bad : {std::string{"18446744073709551616"}, std::string{"01"}, std::string{"1.0"}, std::string{"-1"}}) {
      value.att.claims.counter = bad;
      BOOST_CHECK(!forge::codec::json::write(value).ok());
   }
}

BOOST_AUTO_TEST_CASE(signed_value_has_one_wire_authority_and_redacted_diagnostics) {
   const auto value = att::signed_document{"fixture.payload.signature"};
   BOOST_TEST(value.compact() == "fixture.payload.signature");
   BOOST_CHECK(forge::raw::unpack_exact<att::signed_document>(forge::raw::pack(value)) == value);
   const auto diagnostic = forge::variant_schema::encode_diagnostic(value);
   BOOST_TEST(diagnostic.as_string() == "<redacted>");
   const auto nested = forge::variant_schema::encode_diagnostic(attestation_fixture::credential_holder{value});
   BOOST_TEST(nested.get_object()["credential"].as_string() == "<redacted>");
   auto wire = forge::variant{};
   att::to_variant(value, wire);
   BOOST_TEST(wire.as_string() == value.compact());
   auto restored = att::signed_document{};
   att::from_variant(wire, restored);
   BOOST_CHECK(restored == value);
   BOOST_CHECK_THROW(att::from_variant(forge::variant{std::int64_t{1}}, restored), errors::invalid_document);
   BOOST_CHECK_THROW(att::signed_document{std::string{}}, errors::limit_exceeded);
   BOOST_CHECK_THROW(att::signed_document{std::string(att::max_compact_bytes + 1, 'a')}, errors::limit_exceeded);
   BOOST_CHECK_THROW((att::signed_document{std::string{"token\0secret", 12}}), errors::invalid_document);
   BOOST_CHECK_THROW(att::signed_document{"token secret"}, errors::invalid_document);
   BOOST_CHECK_THROW(forge::raw::pack(att::signed_document{}), errors::limit_exceeded);
}

BOOST_AUTO_TEST_SUITE_END()
