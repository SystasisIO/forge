#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

import forge.api.wasm.codec;
import forge.raw.raw;

namespace {

namespace protocol = forge::api::wasm;
namespace api = forge::api::core;
using forge::raw::bytes;

template <typename T> void golden(const T& value, const bytes& expected) {
   BOOST_CHECK(forge::raw::pack(value) == expected);
   BOOST_CHECK(forge::raw::unpack_exact<T>(expected) == value);
   BOOST_CHECK(protocol::decode<T>(expected) == value);
}

api::error_payload error_value() {
   return {.error = "E", .message = "msg", .retryable = true,
           .status_code = api::status::conflict,
           .identity = {.category = "cat", .code = 0x01020304U},
           .details_codec = api::codec_id{.value = "raw"},
           .details = api::bytes{0x00U, 0x80U, 0xffU}};
}

bytes error_bytes() {
   return {0x01, 0x45, 0x03, 0x6d, 0x73, 0x67, 0x01,
           0x99, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
           0x03, 0x63, 0x61, 0x74, 0x04, 0x03, 0x02, 0x01,
           0x01, 0x03, 0x72, 0x61, 0x77, 0x01, 0x03, 0x00, 0x80, 0xff};
}

} // namespace

BOOST_AUTO_TEST_SUITE(wasm_protocol_wire)

BOOST_AUTO_TEST_CASE(canonical_types_and_variant_order_are_not_copied) {
   static_assert(std::is_same_v<decltype(protocol::payload::data), forge::raw::bytes>);
   static_assert(std::is_same_v<decltype(protocol::http_command::method), forge::net::http::method>);
   static_assert(std::is_same_v<std::variant_alternative_t<2, decltype(protocol::step::result)>,
                                api::error_payload>);
   static_assert(std::is_same_v<std::variant_alternative_t<1, decltype(protocol::event::value)>,
                                api::error_payload>);
   static_assert(std::is_same_v<decltype(protocol::limits::wire), forge::raw::unpack_limits>);
   BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(payload_continuation_and_effect_have_literal_field_order) {
   golden(protocol::payload{"t", {0x00, 0x80, 0xff}},
          {0x01, 0x74, 0x03, 0x00, 0x80, 0xff});
   golden(protocol::continuation{0x01020304U, {"s", {0x42}}},
          {0x04, 0x03, 0x02, 0x01, 0x01, 0x73, 0x01, 0x42});
   golden(protocol::effect{"b", {"q", {0x7f}}, "r", 0x01020304U},
          {0x01, 0x62, 0x01, 0x71, 0x01, 0x7f, 0x01, 0x72, 0x04, 0x03, 0x02, 0x01});
}

BOOST_AUTO_TEST_CASE(every_step_alternative_has_frozen_bytes) {
   const auto pending = protocol::awaiting{
      {"b", {"q", {0x7f}}, "r", 0x01020304U}, {1, {"s", {0x42}}}};
   const auto waiting_bytes = bytes{
      0x01, 0x62, 0x01, 0x71, 0x01, 0x7f, 0x01, 0x72, 0x04, 0x03, 0x02, 0x01,
      0x01, 0x00, 0x00, 0x00, 0x01, 0x73, 0x01, 0x42};
   golden(pending, waiting_bytes);
   auto step_bytes = bytes{0x00};
   step_bytes.insert(step_bytes.end(), waiting_bytes.begin(), waiting_bytes.end());
   golden(protocol::step{pending}, step_bytes);

   golden(protocol::completed{{"o", {0xfe}}}, {0x01, 0x6f, 0x01, 0xfe});
   golden(protocol::step{protocol::completed{{"o", {0xfe}}}}, {0x01, 0x01, 0x6f, 0x01, 0xfe});

   step_bytes = {0x02};
   const auto problem = error_bytes();
   step_bytes.insert(step_bytes.end(), problem.begin(), problem.end());
   golden(protocol::step{error_value()}, step_bytes);
   BOOST_CHECK(forge::raw::pack(error_value()) == problem);
}

BOOST_AUTO_TEST_CASE(every_event_alternative_has_frozen_bytes) {
   golden(protocol::event{protocol::payload{"t", {0x00, 0x80, 0xff}}},
          {0x00, 0x01, 0x74, 0x03, 0x00, 0x80, 0xff});
   auto expected = bytes{0x01};
   const auto problem = error_bytes();
   expected.insert(expected.end(), problem.begin(), problem.end());
   golden(protocol::event{error_value()}, expected);
}

BOOST_AUTO_TEST_CASE(context_is_an_optional_view_and_ordered_typed_facts) {
   golden(protocol::subject_view{"i", "u"}, {0x01, 0x69, 0x01, 0x75});
   golden(protocol::context{}, {0x00, 0x00});
   golden(protocol::context{protocol::subject_view{"i", "u"}, {{"f", {0x01}}}},
          {0x01, 0x01, 0x69, 0x01, 0x75, 0x01, 0x01, 0x66, 0x01, 0x01});
}

BOOST_AUTO_TEST_CASE(http_uses_existing_method_width_and_explicit_optional_body) {
   golden(protocol::http_command{forge::net::http::method::get, "/x", std::nullopt},
          {0x02, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x78, 0x00});
   golden(protocol::http_command{forge::net::http::method::post, "/x", protocol::payload{"n", {0x00}}},
          {0x06, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x78, 0x01, 0x01, 0x6e, 0x01, 0x00});
   golden(protocol::http_response{204, std::nullopt}, {0xcc, 0x00, 0x00});
   golden(protocol::http_response{200, protocol::payload{"n", {0x00}}},
          {0xc8, 0x00, 0x01, 0x01, 0x6e, 0x01, 0x00});
}

BOOST_AUTO_TEST_CASE(payload_codec_delegates_to_existing_raw_without_a_second_serializer) {
   const auto value = protocol::encode("number", std::uint32_t{0x01020304U});
   BOOST_CHECK(value == (protocol::payload{"number", {0x04, 0x03, 0x02, 0x01}}));
   BOOST_CHECK_EQUAL(protocol::decode<std::uint32_t>(value, "number"), 0x01020304U);
}

BOOST_AUTO_TEST_SUITE_END()
