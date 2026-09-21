#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

import forge.multiformats.multihash;
import forge.multiformats.types;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.reachability;

namespace forge::net::p2p {
namespace {

[[nodiscard]] peer_id test_peer(std::uint8_t value) {
   const auto payload = forge::multiformats::bytes{value};
   return peer_id::from_bytes(forge::multiformats::multihash::identity(payload).encode());
}

[[nodiscard]] endpoint test_endpoint() {
   return parse_endpoint("/ip4/127.0.0.1/tcp/4001");
}

void check_bytes(const std::vector<std::uint8_t>& actual, const std::vector<std::uint8_t>& expected) {
   BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
}

void append_raw_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
   while (value >= 0x80U) {
      out.push_back(static_cast<std::uint8_t>((value & 0x7fU) | 0x80U));
      value >>= 7U;
   }
   out.push_back(static_cast<std::uint8_t>(value));
}

void append_raw_varint_field(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t value) {
   append_raw_varint(out, static_cast<std::uint64_t>(field) << 3U);
   append_raw_varint(out, value);
}

void append_raw_bytes_field(std::vector<std::uint8_t>& out, std::uint32_t field,
                            const std::vector<std::uint8_t>& value) {
   append_raw_varint(out, (static_cast<std::uint64_t>(field) << 3U) | 2U);
   append_raw_varint(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}

[[nodiscard]] std::vector<std::uint8_t> raw_frame(const std::vector<std::uint8_t>& payload) {
   auto out = std::vector<std::uint8_t>{};
   append_raw_varint(out, payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_reachability_codec_tests)

BOOST_AUTO_TEST_CASE(autonat_v1_dial_uses_the_required_dial_wrapper) {
   // Message { type: DIAL, dial: Dial { peer: PeerInfo { id, addrs } } }
   const auto golden = std::vector<std::uint8_t>{
       0x15,
       0x08, 0x00,
       0x12, 0x11,
       0x0a, 0x0f,
       0x0a, 0x03, 0x00, 0x01, 0x01,
       0x12, 0x08, 0x04, 0x7f, 0x00, 0x00, 0x01, 0x06, 0x0f, 0xa1,
   };
   const auto value = reachability::message{
       .kind = reachability::message::message_kind::dial,
       .peer = reachability::peer_info{.peer = test_peer(1), .endpoints = {test_endpoint()}},
   };

   check_bytes(reachability::codec::encode_v1(value), golden);

   const auto decoded = reachability::codec::decode_v1(golden);
   BOOST_TEST(static_cast<int>(decoded.kind) == static_cast<int>(reachability::message::message_kind::dial));
   BOOST_REQUIRE(decoded.peer.has_value());
   BOOST_TEST(decoded.peer->peer.to_string() == test_peer(1).to_string());
   BOOST_REQUIRE_EQUAL(decoded.peer->endpoints.size(), 1U);
   BOOST_TEST(decoded.peer->endpoints.front().to_string() == test_endpoint().to_string());
}

BOOST_AUTO_TEST_CASE(autonat_v1_omitted_response_status_uses_proto2_ok_default) {
   // Message { type: DIAL_RESPONSE, dialResponse: DialResponse {} }
   const auto golden = std::vector<std::uint8_t>{0x04, 0x08, 0x01, 0x1a, 0x00};

   const auto decoded = reachability::codec::decode_v1(golden);
   BOOST_REQUIRE(decoded.response.has_value());
   BOOST_TEST(static_cast<int>(decoded.response->status) == static_cast<int>(reachability::dial_status::ok));

}

BOOST_AUTO_TEST_CASE(autonat_v1_success_preserves_proto2_status_presence_for_rust) {
   // Pinned Rust v1 DialResponse::into_proto uses Some(OK), not an absent status.
   const auto golden = std::vector<std::uint8_t>{
       0x10, 0x08, 0x01, 0x1a, 0x0c, 0x08, 0x00,
       0x1a, 0x08, 0x04, 0x7f, 0x00, 0x00, 0x01, 0x06, 0x0f, 0xa1,
   };
   const auto value = reachability::message{
       .kind = reachability::message::message_kind::dial_response,
       .response = reachability::dial_response{.status = reachability::dial_status::ok,
                                             .endpoint = test_endpoint()},
   };
   check_bytes(reachability::codec::encode_v1(value), golden);
}

BOOST_AUTO_TEST_CASE(autonat_v2_omitted_default_scalars_match_proto3_frames) {
   // Message { dialResponse: DialResponse {} }; proto3 defaults to INTERNAL_ERROR, 0, UNUSED.
   const auto dial_response = std::vector<std::uint8_t>{0x02, 0x12, 0x00};
   const auto decoded_response = reachability::codec::decode_v2(dial_response);
   BOOST_REQUIRE(decoded_response.dial_response.has_value());
   BOOST_TEST(static_cast<int>(decoded_response.dial_response->status) ==
              static_cast<int>(reachability::v2::response_status::internal_error));
   BOOST_TEST(decoded_response.dial_response->index == 0U);
   BOOST_TEST(static_cast<int>(decoded_response.dial_response->dial_status) ==
              static_cast<int>(reachability::v2::dial_status::unused));
   check_bytes(reachability::codec::encode_v2(reachability::v2::message{
                   .type = reachability::v2::message::kind::dial_response,
                   .dial_response = reachability::v2::dial_response{},
               }),
               dial_response);

   // DialBackResponse {} uses proto3's OK default.
   const auto dial_back_response = std::vector<std::uint8_t>{0x00};
   const auto decoded_dial_back = reachability::codec::decode_v2_dial_back_response(dial_back_response);
   BOOST_TEST(static_cast<int>(decoded_dial_back.status) == static_cast<int>(reachability::v2::dial_back_status::ok));
   check_bytes(reachability::codec::encode_v2_dial_back_response(reachability::v2::dial_back_response{}),
               dial_back_response);
   const auto explicit_frame = std::vector<std::uint8_t>{0x02, 0x08, 0x00};
   const auto explicit_zero = reachability::codec::decode_v2_dial_back_response(explicit_frame);
   BOOST_TEST(static_cast<int>(explicit_zero.status) == static_cast<int>(reachability::v2::dial_back_status::ok));
}

BOOST_AUTO_TEST_CASE(autonat_codec_applies_endpoint_and_framed_message_bounds_in_both_directions) {
   const auto v1_dial = reachability::message{
       .kind = reachability::message::message_kind::dial,
       .peer = reachability::peer_info{.peer = test_peer(2), .endpoints = {test_endpoint(), test_endpoint()}},
   };
   auto endpoint_limit = reachability::options{};
   endpoint_limit.max_endpoints = 1;

   BOOST_CHECK_THROW(reachability::codec::encode_v1(v1_dial, endpoint_limit), exceptions::invalid_options);
   BOOST_CHECK_THROW(reachability::codec::decode_v1(reachability::codec::encode_v1(v1_dial), endpoint_limit),
                     exceptions::codec_error);

   auto v1_message_limit = reachability::options{};
   v1_message_limit.max_message_size = 2;
   BOOST_CHECK_THROW(reachability::codec::encode_v1(v1_dial, v1_message_limit), exceptions::invalid_options);
   BOOST_CHECK_THROW(reachability::codec::decode_v1(reachability::codec::encode_v1(v1_dial), v1_message_limit),
                     exceptions::codec_error);

   const auto v2_data = reachability::v2::message{
       .type = reachability::v2::message::kind::dial_data_response,
       .dial_data_response = reachability::v2::dial_data_response{.data = {0x42}},
   };
   auto message_limit = reachability::options{};
   message_limit.max_message_size = 2;

   BOOST_CHECK_THROW(reachability::codec::encode_v2(v2_data, message_limit), exceptions::invalid_options);
   BOOST_CHECK_THROW(reachability::codec::decode_v2(reachability::codec::encode_v2(v2_data), message_limit),
                     exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(autonat_codec_rejects_out_of_range_raw_enum_values) {
   auto v1_response = std::vector<std::uint8_t>{};
   append_raw_varint_field(v1_response, 1, 1);
   auto v1_status = std::vector<std::uint8_t>{};
   append_raw_varint_field(v1_status, 1, 65'536);
   const auto observed_address = std::vector<std::uint8_t>{0x04, 0x7f, 0x00, 0x00, 0x01, 0x06, 0x0f, 0xa1};
   append_raw_bytes_field(v1_status, 3, observed_address);
   append_raw_bytes_field(v1_response, 3, v1_status);
   BOOST_CHECK_THROW(reachability::codec::decode_v1(raw_frame(v1_response)), exceptions::codec_error);

   auto v2_response_status = std::vector<std::uint8_t>{};
   append_raw_varint_field(v2_response_status, 1, 65'736);
   auto v2_response = std::vector<std::uint8_t>{};
   append_raw_bytes_field(v2_response, 2, v2_response_status);
   BOOST_CHECK_THROW(reachability::codec::decode_v2(raw_frame(v2_response)), exceptions::codec_error);

   auto v2_dial_status = std::vector<std::uint8_t>{};
   append_raw_varint_field(v2_dial_status, 3, 65'736);
   auto v2_dial = std::vector<std::uint8_t>{};
   append_raw_bytes_field(v2_dial, 2, v2_dial_status);
   BOOST_CHECK_THROW(reachability::codec::decode_v2(raw_frame(v2_dial)), exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(autonat_codec_rejects_out_of_range_raw_indices) {
   constexpr auto out_of_range_index = std::uint64_t{1} << 32U;

   auto v2_response_index = std::vector<std::uint8_t>{};
   append_raw_varint_field(v2_response_index, 2, out_of_range_index);
   auto v2_response = std::vector<std::uint8_t>{};
   append_raw_bytes_field(v2_response, 2, v2_response_index);
   BOOST_CHECK_THROW(reachability::codec::decode_v2(raw_frame(v2_response)), exceptions::codec_error);

   auto v2_data_request = std::vector<std::uint8_t>{};
   append_raw_varint_field(v2_data_request, 1, out_of_range_index);
   append_raw_varint_field(v2_data_request, 2, 1);
   auto v2_data = std::vector<std::uint8_t>{};
   append_raw_bytes_field(v2_data, 3, v2_data_request);
   BOOST_CHECK_THROW(reachability::codec::decode_v2(raw_frame(v2_data)), exceptions::codec_error);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
