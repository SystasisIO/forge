#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>

import forge.expression.artifact_codec;
import forge.expression.exceptions;
import forge.raw.codec;

namespace {

namespace expression = forge::expression;

// This is an envelope fixture, not a semantically valid CEL program. Semantic
// admission is exercised by the real worker tests, separately from Raw layout.
expression::artifact envelope() {
   expression::artifact value;
   value.expression = {0x08, 0x01};
   value.digest = expression::artifact_digest(value);
   return value;
}

} // namespace

BOOST_AUTO_TEST_SUITE(expression_artifact)

BOOST_AUTO_TEST_CASE(portable_raw_layout_and_domain_separated_digest) {
   auto value = envelope();
   BOOST_CHECK_EQUAL(value.digest.str(), "cdcf9412b9e4312f194f2de3420cac3b1abe1ef3a1d8b967249d2d90623b92b3");
   value.digest = {};
   // Independently specified little-endian fixed-width fields. The final
   // 32 zero bytes are the digest; enum type occupies exactly one byte.
   const std::array<std::uint8_t, 81> expected{
       0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
       0x00, 0x04, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
       0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x02, 0x08, 0x01};
   const auto bytes = forge::raw::pack(value);
   BOOST_CHECK_EQUAL_COLLECTIONS(bytes.begin(), bytes.end(), expected.begin(), expected.end());
   BOOST_CHECK(forge::raw::unpack_exact<expression::artifact>(bytes) == value);
}

BOOST_AUTO_TEST_CASE(envelope_roundtrip_and_tampering) {
   const auto value = envelope();
   auto bytes = expression::encode_artifact(value);
   BOOST_CHECK(expression::decode_artifact(bytes) == value);
   bytes.push_back(0);
   BOOST_CHECK_THROW(expression::decode_artifact(bytes), expression::exceptions::invalid_program);
   bytes = expression::encode_artifact(value);
   bytes.back() ^= 1;
   BOOST_CHECK_THROW(expression::decode_artifact(bytes), expression::exceptions::invalid_program);
   bytes.resize(12);
   BOOST_CHECK_THROW(expression::decode_artifact(bytes), expression::exceptions::invalid_program);
}

BOOST_AUTO_TEST_CASE(version_budget_and_declarations_are_admitted_before_use) {
   auto value = envelope();
   ++value.semantics;
   BOOST_CHECK_THROW(expression::artifact_digest(value), expression::exceptions::invalid_profile);
   value = envelope();
   value.budget.nodes = 0;
   BOOST_CHECK_THROW(expression::artifact_digest(value), expression::exceptions::invalid_profile);
   value = envelope();
   value.variables = {{"z"}, {"a"}};
   BOOST_CHECK_THROW(expression::artifact_digest(value), expression::exceptions::invalid_program);
   value.variables = {{"a"}, {"a"}};
   BOOST_CHECK_THROW(expression::artifact_digest(value), expression::exceptions::invalid_program);
   value.variables = {{"a", static_cast<expression::type>(255)}};
   BOOST_CHECK_THROW(expression::artifact_digest(value), expression::exceptions::invalid_program);
   value = envelope();
   value.result = expression::type::string;
   BOOST_CHECK_THROW(expression::artifact_digest(value), expression::exceptions::invalid_program);
}

BOOST_AUTO_TEST_CASE(maximum_declarations_and_expression_have_bounded_roundtrip) {
   auto value = envelope();
   for (unsigned index = 0; index < 64; ++index) {
      value.variables.push_back({std::string{"v"} + (index < 10 ? "0" : "") + std::to_string(index)});
   }
   value.expression.resize(expression::max_expression_bytes, 1);
   value.digest = expression::artifact_digest(value);
   BOOST_CHECK(expression::decode_artifact(expression::encode_artifact(value)) == value);
   value.expression.push_back(1);
   BOOST_CHECK_THROW(expression::artifact_digest(value), expression::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_SUITE_END()
