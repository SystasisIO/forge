#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

import forge.api.wasm.capability;
import forge.api.wasm.validation;
import forge.codec.json;
import forge.raw.raw;
import forge.net.http.method;

BOOST_AUTO_TEST_CASE(body_presence_keeps_its_shared_unsigned_byte_wire_type) {
   static_assert(
       std::is_same_v<forge::raw::enum_wire_type_t<forge::api::wasm::capability::body_presence>, std::uint8_t>);
   static_assert(forge::raw::shared_enum_wire_v<forge::api::wasm::capability::body_presence>);
   BOOST_CHECK(forge::raw::pack(forge::api::wasm::capability::body_presence::absent) == forge::raw::bytes({0}));
   BOOST_CHECK(forge::raw::pack(forge::api::wasm::capability::body_presence::optional) == forge::raw::bytes({1}));
   BOOST_CHECK(forge::raw::pack(forge::api::wasm::capability::body_presence::required) == forge::raw::bytes({2}));
   BOOST_CHECK(forge::raw::pack(forge::api::wasm::capability::body_presence::status_only) == forge::raw::bytes({3}));

   const auto value =
       forge::api::wasm::capability::body_policy{forge::api::wasm::capability::body_presence::required, "x"};
   const auto expected = forge::raw::bytes{0x02, 0x01, 0x78};
   BOOST_CHECK(forge::raw::pack(value) == expected);
   BOOST_CHECK(forge::raw::unpack_exact<forge::api::wasm::capability::body_policy>(expected) == value);
}

BOOST_AUTO_TEST_CASE(nested_capability_has_literal_bytes_independent_of_native_describe) {
   const auto value = forge::api::wasm::capability{
       .binding = "b",
       .request_type = "q",
       .response_type = "r",
       .max_delay_ms = 0x01020304U,
       .http = forge::api::wasm::capability::http_policy{
           .request_body = {forge::api::wasm::capability::body_presence::required, "x"},
           .responses = {{200, {forge::api::wasm::capability::body_presence::required, "y"}},
                         {429, {forge::api::wasm::capability::body_presence::status_only, {}}}}}};
   const auto expected = forge::raw::bytes{0x01, 0x62, 0x01, 0x71, 0x01, 0x72, 0x04, 0x03, 0x02, 0x01, 0x01, 0x02,
                                           0x01, 0x78, 0x02, 0xc8, 0x00, 0x02, 0x01, 0x79, 0xad, 0x01, 0x03, 0x00};
   BOOST_CHECK(forge::raw::pack(value) == expected);
   BOOST_CHECK(forge::raw::unpack_exact<forge::api::wasm::capability>(expected) == value);
   const auto written = forge::codec::json::write(value);
   BOOST_REQUIRE(written.ok());
   const auto parsed = forge::codec::json::read<forge::api::wasm::capability>(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_CHECK(forge::raw::pack(parsed.value) == expected);

   const auto without_http = forge::api::wasm::capability{
       .binding = "b", .request_type = "q", .response_type = "r", .max_delay_ms = 0x01020304U};
   const auto absent_bytes = forge::raw::bytes{0x01, 0x62, 0x01, 0x71, 0x01, 0x72, 0x04, 0x03, 0x02, 0x01, 0x00};
   BOOST_CHECK(forge::raw::pack(without_http) == absent_bytes);
   BOOST_CHECK(forge::raw::unpack_exact<forge::api::wasm::capability>(absent_bytes) == without_http);
}

BOOST_AUTO_TEST_CASE(capability_json_preserves_http_policy_and_raw_layout) {
   const auto source = forge::api::wasm::capability{
       .binding = "provider.submit",
       .request_type = "http_command",
       .response_type = "http_response",
       .max_delay_ms = 1000,
       .http = forge::api::wasm::capability::http_policy{
           .request_body = {forge::api::wasm::capability::body_presence::required, "input"},
           .responses = {{200, {forge::api::wasm::capability::body_presence::required, "result"}},
                         {429, {forge::api::wasm::capability::body_presence::status_only, {}}}}}};
   forge::api::wasm::validate(source);
   const auto bytes = forge::raw::pack(source);
   const auto written = forge::codec::json::write(source);
   BOOST_REQUIRE(written.ok());
   BOOST_CHECK(written.text.find("status_only") != std::string::npos);
   const auto parsed = forge::codec::json::read<forge::api::wasm::capability>(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_CHECK(parsed.value == source);
   BOOST_CHECK(forge::raw::pack(parsed.value) == bytes);

   auto changed = source;
   changed.http->responses.back().body.presence = forge::api::wasm::capability::body_presence::absent;
   BOOST_CHECK(changed != source);
   BOOST_CHECK(forge::raw::unpack_exact<forge::api::wasm::capability>(bytes) == source);
}

BOOST_AUTO_TEST_CASE(http_method_json_uses_names_without_changing_raw_width) {
   const auto written = forge::codec::json::write(forge::net::http::method::post);
   BOOST_REQUIRE(written.ok());
   BOOST_CHECK_EQUAL(written.text, "\"post\"");
   const auto parsed = forge::codec::json::read<forge::net::http::method>(written.text);
   BOOST_REQUIRE(parsed.ok());
   BOOST_CHECK(parsed.value == forge::net::http::method::post);
   BOOST_CHECK(forge::raw::pack(parsed.value) == forge::raw::bytes({6, 0, 0, 0}));
}
