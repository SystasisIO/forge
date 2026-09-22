#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "../../libraries/net/p2p/details/mdns_codec.hxx"

import forge.net.p2p.exceptions;

namespace {

namespace codec = forge::net::p2p::detail::mdns_codec;
using codec_error = forge::net::p2p::exceptions::codec_error;

// Hand-encoded RFC1035 query. No production encoder is used to derive fixtures.
constexpr auto query = std::to_array<std::uint8_t>({
    0xab, 0xcd, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
    4, '_', 'p', '2', 'p', 4, '_', 'u', 'd', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
    0, 12, 0x80, 1});

// All four sections. Pointers reference offsets 12 (s.local), 14 (local),
// 37 (p.s.local), 78 (h.local). TXT has three independent binary attributes.
constexpr auto response = std::to_array<std::uint8_t>({
    0x12, 0x34, 0x84, 0x00, 0, 1, 0, 1, 0, 1, 0, 3,
    1, 's', 5, 'l', 'o', 'c', 'a', 'l', 0, 0, 12, 0x80, 1,
    0xc0, 12, 0, 12, 0x80, 1, 0, 0, 0, 120, 0, 4, 1, 'p', 0xc0, 12,
    0xc0, 37, 0, 16, 0x80, 1, 0, 0, 0, 1, 0, 7, 0, 3, 'a', 0, 'b', 1, 'x',
    0xc0, 37, 0, 33, 0, 1, 0, 0, 0, 120, 0, 10, 0, 1, 0, 2, 0x0f, 0xa1, 1, 'h', 0xc0, 14,
    0xc0, 78, 0, 1, 0x80, 1, 0, 0, 0, 120, 0, 4, 192, 0, 2, 1,
    0xc0, 78, 0, 28, 0, 1, 0, 0, 0, 0, 0, 16,
    0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});

codec::bytes changed(std::size_t offset, std::uint8_t value) {
   auto packet = codec::bytes{response.begin(), response.end()};
   packet.at(offset) = value;
   return packet;
}

// Independent root-name questions linked by backward compression pointers.
codec::bytes pointer_chain() {
   auto packet = codec::bytes{0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0,
                               0, 0, 12, 0, 1,
                               0xc0, 12, 0, 12, 0, 1,
                               0xc0, 17, 0, 12, 0, 1,
                               0xc0, 23, 0, 12, 0, 1};
   return packet;
}

} // namespace

BOOST_AUTO_TEST_SUITE(mdns_codec_tests)

BOOST_AUTO_TEST_CASE(independent_query_golden_preserves_qu_and_caller_service) {
   const auto decoded = codec::decode(query);
   BOOST_CHECK_EQUAL(decoded.head.id, 0xabcd);
   BOOST_CHECK_EQUAL(decoded.head.flags, 0x0100);
   BOOST_REQUIRE_EQUAL(decoded.questions.size(), 1);
   BOOST_CHECK((decoded.questions[0].owner == codec::name{"_p2p", "_udp", "local"}));
   BOOST_CHECK_EQUAL(decoded.questions[0].type, codec::type_ptr);
   BOOST_CHECK_EQUAL(decoded.questions[0].class_code, 0x8001);
   BOOST_CHECK(decoded.answers.empty() && decoded.authorities.empty() && decoded.additionals.empty());
   const auto encoded = codec::encode(decoded);
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(), query.begin(), query.end());

   auto custom = decoded;
   custom.questions[0].owner = {"_custom-network", "_udp", "local"};
   BOOST_CHECK(codec::decode(codec::encode(custom)) == custom);
}

BOOST_AUTO_TEST_CASE(compressed_all_sections_preserve_flags_ttl_and_individual_txt_values) {
   static_assert(response.size() == 126);
   const auto decoded = codec::decode(response);
   BOOST_CHECK_EQUAL(decoded.head.id, 0x1234);
   BOOST_CHECK_EQUAL(decoded.head.flags, 0x8400);
   BOOST_REQUIRE_EQUAL(decoded.questions.size(), 1);
   BOOST_REQUIRE_EQUAL(decoded.answers.size(), 1);
   BOOST_REQUIRE_EQUAL(decoded.authorities.size(), 1);
   BOOST_REQUIRE_EQUAL(decoded.additionals.size(), 3);
   BOOST_CHECK_EQUAL(decoded.questions[0].class_code, 0x8001);
   const auto& answer = decoded.answers[0];
   BOOST_CHECK((answer.owner == codec::name{"s", "local"}));
   BOOST_CHECK_EQUAL(answer.class_code, 0x8001);
   BOOST_CHECK_EQUAL(answer.ttl, 120);
   BOOST_CHECK((std::get<codec::ptr>(answer.data).target == codec::name{"p", "s", "local"}));
   const auto& text = decoded.authorities[0];
   BOOST_CHECK((text.owner == codec::name{"p", "s", "local"}));
   BOOST_CHECK_EQUAL(text.class_code, 0x8001);
   BOOST_CHECK_EQUAL(text.ttl, 1);
   BOOST_CHECK((std::get<codec::txt>(text.data).attributes ==
                 std::vector<codec::bytes>{{}, {'a', 0, 'b'}, {'x'}}));
   const auto& service = std::get<codec::srv>(decoded.additionals[0].data);
   BOOST_CHECK_EQUAL(service.priority, 1);
   BOOST_CHECK_EQUAL(service.weight, 2);
   BOOST_CHECK_EQUAL(service.port, 4001);
   BOOST_CHECK((service.target == codec::name{"h", "local"}));
   BOOST_CHECK((decoded.additionals[1].owner == codec::name{"h", "local"}));
   BOOST_CHECK_EQUAL(decoded.additionals[1].class_code, 0x8001);
   BOOST_CHECK((std::get<codec::a>(decoded.additionals[1].data).address ==
                 std::array<std::uint8_t, 4>{192, 0, 2, 1}));
   BOOST_CHECK_EQUAL(decoded.additionals[2].ttl, 0); // Goodbye, not silently defaulted.
   BOOST_CHECK((std::get<codec::aaaa>(decoded.additionals[2].data).address ==
                 std::array<std::uint8_t, 16>{0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
   const auto canonical = codec::encode(decoded);
   BOOST_CHECK(codec::decode(canonical) == decoded);
   BOOST_CHECK(codec::encode(codec::decode(canonical)) == canonical);
}

BOOST_AUTO_TEST_CASE(every_truncated_prefix_and_trailing_bytes_are_rejected) {
   for (auto length = std::size_t{}; length < response.size(); ++length) {
      BOOST_CHECK_THROW(static_cast<void>(codec::decode(std::span{response}.first(length))), codec_error);
   }
   auto trailing = codec::bytes{response.begin(), response.end()};
   trailing.push_back(0);
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(trailing)), codec_error);
   for (const auto offset : {36U, 71U, 93U, 109U}) {
      BOOST_CHECK_THROW(static_cast<void>(codec::decode(changed(offset, response[offset] - 1))), codec_error);
      BOOST_CHECK_THROW(static_cast<void>(codec::decode(changed(offset, response[offset] + 1))), codec_error);
   }
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(changed(54, 255))), codec_error);
}

BOOST_AUTO_TEST_CASE(compression_cycles_out_of_bounds_reserved_tags_and_depth_are_rejected) {
   auto packet = changed(12, 0xc0);
   packet[13] = 12; // Self pointer.
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(packet)), codec_error);
   packet[13] = 14;
   packet[14] = 0xc0;
   packet[15] = 12; // Two-pointer cycle.
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(packet)), codec_error);
   packet = changed(25, 0xc0);
   packet[26] = 255;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(packet)), codec_error);
   packet[26] = 0; // DNS header bytes cannot be a name target.
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(packet)), codec_error);
   for (const auto tag : {0x40U, 0x80U}) {
      BOOST_CHECK_THROW(static_cast<void>(codec::decode(changed(12, tag))), codec_error);
   }
   auto bounds = codec::limits{};
   bounds.max_pointer_depth = 3;
   const auto chain = pointer_chain();
   BOOST_CHECK_EQUAL(codec::decode(chain, bounds).questions.size(), 4);
   bounds.max_pointer_depth = 2;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(chain, bounds)), codec_error);
   bounds.max_pointer_depth = 0;
   BOOST_CHECK_NO_THROW(static_cast<void>(codec::decode(query, bounds)));
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(response, bounds)), codec_error);
   bounds.max_pointer_depth = 129;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(query, bounds)), codec_error);
}

BOOST_AUTO_TEST_CASE(name_label_limits_root_and_binary_labels_roundtrip) {
   auto value = codec::message{};
   value.questions.push_back({.owner = {}});
   value.questions.push_back({.owner = {std::string{"a.b"}, std::string{"x\0y", 3}}});
   value.questions.push_back({.owner = {std::string(63, 'a'), std::string(63, 'b'),
                                       std::string(63, 'c'), std::string(61, 'd')}});
   BOOST_CHECK(codec::decode(codec::encode(value)) == value);
   value.questions.back().owner.back().push_back('d'); // mDNS wire length 256, including root.
   BOOST_CHECK(codec::decode(codec::encode(value)) == value);
   value.questions.back().owner.back().push_back('d'); // mDNS wire length 257, including root.
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(value)), codec_error);
   value.questions.back().owner = {std::string(64, 'a')};
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(value)), codec_error);
   value.questions.back().owner = {""};
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(value)), codec_error);

   // Hand-built 254-byte first name plus a compressed 256-byte second name.
   auto packet = codec::bytes{0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0};
   for (const auto size : {63U, 63U, 63U, 60U}) {
      packet.push_back(static_cast<std::uint8_t>(size));
      packet.insert(packet.end(), size, 'a');
   }
   packet.insert(packet.end(), {0, 0, 12, 0, 1, 1, 'x', 0xc0, 12, 0, 12, 0, 1});
   const auto decoded_compressed = codec::decode(packet);
   BOOST_REQUIRE_EQUAL(decoded_compressed.questions.size(), 2U);
   BOOST_CHECK_EQUAL(decoded_compressed.questions[1].owner.size(), 5U);
   BOOST_CHECK_EQUAL(decoded_compressed.questions[1].owner.front(), "x");

   // A 255-byte first name plus a label makes the compressed name 257 bytes.
   packet = {0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0};
   for (const auto size : {63U, 63U, 63U, 61U}) {
      packet.push_back(static_cast<std::uint8_t>(size));
      packet.insert(packet.end(), size, 'a');
   }
   packet.insert(packet.end(), {0, 0, 12, 0, 1, 1, 'x', 0xc0, 12, 0, 12, 0, 1});
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(packet)), codec_error);

   // One uncompressed name of mDNS wire length 256 is valid.
   packet = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
   for (const auto size : {63U, 63U, 63U, 62U}) {
      packet.push_back(static_cast<std::uint8_t>(size));
      packet.insert(packet.end(), size, 'a');
   }
   packet.insert(packet.end(), {0, 0, 12, 0, 1});
   const auto decoded_uncompressed = codec::decode(packet);
   BOOST_REQUIRE_EQUAL(decoded_uncompressed.questions.size(), 1U);
   BOOST_CHECK_EQUAL(decoded_uncompressed.questions.front().owner.back().size(), 62U);

   // One uncompressed name of mDNS wire length 257 is invalid.
   packet = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
   for (const auto size : {63U, 63U, 63U, 63U}) {
      packet.push_back(static_cast<std::uint8_t>(size));
      packet.insert(packet.end(), size, 'a');
   }
   packet.insert(packet.end(), {0, 0, 12, 0, 1});
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(packet)), codec_error);
}

BOOST_AUTO_TEST_CASE(packet_section_record_and_txt_budgets_are_enforced) {
   const auto value = codec::decode(response);
   for (const auto member : {&codec::limits::max_questions, &codec::limits::max_records,
                             &codec::limits::max_records_per_section, &codec::limits::max_rdata_size,
                             &codec::limits::max_txt_attributes, &codec::limits::max_txt_attributes_per_record,
                             &codec::limits::max_txt_bytes, &codec::limits::max_txt_bytes_per_record}) {
      auto bounds = codec::limits{};
      bounds.*member = 0;
      BOOST_CHECK_THROW(static_cast<void>(codec::decode(response, bounds)), codec_error);
      BOOST_CHECK_THROW(static_cast<void>(codec::encode(value, bounds)), codec_error);
   }
   auto bounds = codec::limits{};
   bounds.max_records = 4; // 1 answer + 1 authority + 3 additionals.
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(response, bounds)), codec_error);
   bounds = {};
   bounds.max_records_per_section = 2;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(response, bounds)), codec_error);
   bounds = {};
   bounds.max_packet_size = response.size() - 1;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(response, bounds)), codec_error);
   const auto canonical = codec::encode(value);
   bounds.max_packet_size = canonical.size();
   BOOST_CHECK(codec::encode(value, bounds) == canonical);
   --bounds.max_packet_size;
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(value, bounds)), codec_error);

   for (const auto count_offset : {4U, 6U, 8U, 10U}) {
      BOOST_CHECK_THROW(static_cast<void>(codec::decode(changed(count_offset, 255))), codec_error);
   }
   // Even with caller-raised limits, advertised counts must fit the actual wire.
   bounds = {};
   bounds.max_questions = 65535;
   const auto tiny = codec::bytes{0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0};
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(tiny, bounds)), codec_error);
}

BOOST_AUTO_TEST_CASE(txt_limits_preserve_empty_maximum_and_multiple_record_attributes) {
   auto value = codec::message{};
   value.answers.push_back({.type = codec::type_txt, .data = codec::txt{.attributes = {codec::bytes(255, 'x'), {}}}});
   value.additionals.push_back({.type = codec::type_txt, .data = codec::txt{.attributes = {{'y'}}}});
   const auto wire = codec::encode(value);
   BOOST_CHECK(codec::decode(wire) == value);
   auto bounds = codec::limits{};
   bounds.max_txt_attributes = 2;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(wire, bounds)), codec_error);
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(value, bounds)), codec_error);
   bounds = {};
   bounds.max_txt_bytes = 255;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(wire, bounds)), codec_error);
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(value, bounds)), codec_error);
   std::get<codec::txt>(value.answers[0].data).attributes[0].push_back('x');
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(value)), codec_error);
}

BOOST_AUTO_TEST_CASE(unknown_rdata_is_skipped_without_interpreting_opaque_pointers) {
   auto packet = changed(44, 99); // Unknown authority RR, with seven opaque bytes.
   packet[53] = 0xc0;
   packet[54] = 0xff;
   const auto decoded = codec::decode(packet);
   BOOST_REQUIRE_EQUAL(decoded.authorities.size(), 1);
   BOOST_CHECK_EQUAL(decoded.authorities[0].type, 99);
   BOOST_CHECK_EQUAL(decoded.authorities[0].class_code, 0x8001);
   BOOST_CHECK_EQUAL(std::get<codec::unknown>(decoded.authorities[0].data).wire_size, 7);
   BOOST_REQUIRE_EQUAL(decoded.additionals.size(), 3);
   BOOST_CHECK_EQUAL(std::get<codec::srv>(decoded.additionals[0].data).port, 4001);
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(decoded)), codec_error);
   packet[52] = 255;
   BOOST_CHECK_THROW(static_cast<void>(codec::decode(packet)), codec_error);

   auto wrong_type = codec::decode(response);
   wrong_type.answers[0].type = codec::type_a;
   BOOST_CHECK_THROW(static_cast<void>(codec::encode(wrong_type)), codec_error);
}

BOOST_AUTO_TEST_SUITE_END()
