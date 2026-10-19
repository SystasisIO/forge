#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import forge.swarm.protocol.peer_message;
import forge.raw.raw;

namespace message_test {
namespace core = forge::swarm::core;
namespace protocol = forge::swarm::protocol;

std::vector<std::uint8_t> unhex(std::string_view hex) {
   auto result = std::vector<std::uint8_t>{};
   for (auto i = std::size_t{}; i < hex.size(); i += 2U) {
      result.push_back(static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(i, 2U)}, nullptr, 16)));
   }
   return result;
}

core::descriptor manifest(std::uint32_t pieces, std::uint32_t piece_size = core::block_size, std::uint32_t trim = 0U) {
   const auto bytes = std::vector<std::uint8_t>(pieces * piece_size - trim, 42U);
   const auto hashes = core::hash_file(bytes, piece_size);
   return {core::descriptor_version::v1, piece_size, {{{"a"}, bytes.size(), hashes.root, hashes.pieces}}};
}

std::vector<std::pair<protocol::peer_message, std::string>> golden() {
   const auto zero = std::string(64U, '0');
   return {
       {{protocol::handshake{1U, {}, "r", 0x01020304U}}, "000100" + zero + "017204030201"},
       {{protocol::inventory{{1U, 128U, 0U}}}, "0103018000"},
       {{protocol::have{{2U, 3U}}}, "020200000003000000"},
       {{protocol::interest{true}}, "0301"},
       {{protocol::choke{false}}, "0400"},
       {{protocol::request{1U, {2U, 3U}, core::block_size, 7U}}, "05010000000000000002000000030000000040000007000000"},
       {{protocol::block{1U, {2U, 3U}, core::block_size, {'a', 'b'}}},
        "060100000000000000020000000300000000400000026162"},
       {{protocol::cancel{1U}}, "070100000000000000"},
       {{protocol::reject{1U, protocol::rejection::canceled}}, "08010000000000000004"},
       {{protocol::hash_request{1U, {2U, 3U}}}, "0901000000000000000200000003000000"},
       {{protocol::hash_response{1U, {2U, 3U}, {}, {}}}, "0a01000000000000000200000003000000" + zero + "00"},
   };
}

} // namespace message_test

BOOST_AUTO_TEST_SUITE(swarm_protocol_message)

BOOST_AUTO_TEST_CASE(frozen_variant_order_and_all_field_layouts_match_hardcoded_raw) {
   using namespace message_test;
   const auto fixtures = golden();
   BOOST_REQUIRE_EQUAL(fixtures.size(), 11U);
   for (auto index = std::size_t{}; index < fixtures.size(); ++index) {
      const auto& [message, hex] = fixtures[index];
      const auto wire = unhex(hex);
      BOOST_TEST(message.body.index() == index);
      BOOST_CHECK(forge::raw::pack(message) == wire);
      BOOST_CHECK(protocol::encode_message(message) == wire);
      BOOST_CHECK(protocol::decode_message(wire) == message);
      BOOST_CHECK(protocol::decode_message(forge::raw::pack(message)) == message);
   }
   for (auto code = 1U; code <= 4U; ++code) {
      const auto message = protocol::peer_message{protocol::reject{1U, static_cast<protocol::rejection>(code)}};
      const auto wire = protocol::encode_message(message);
      BOOST_TEST(wire.size() == 10U);
      BOOST_TEST(wire.back() == code);
   }
}

BOOST_AUTO_TEST_CASE(all_truncated_frames_trailing_unknown_alternatives_and_nonminimal_counts_are_rejected) {
   using namespace message_test;
   for (const auto& [message, hex] : golden()) {
      const auto bytes = unhex(hex);
      for (auto size = std::size_t{}; size < bytes.size(); ++size) {
         BOOST_CHECK_THROW((void)protocol::decode_message(std::span{bytes}.first(size)),
                           protocol::exceptions::invalid_encoding);
      }
      auto trailing = bytes;
      trailing.push_back(0U);
      BOOST_CHECK_THROW((void)protocol::decode_message(trailing), protocol::exceptions::invalid_encoding);
      auto nonminimal = bytes;
      nonminimal[0] |= 0x80U;
      nonminimal.insert(nonminimal.begin() + 1U, 0U);
      BOOST_CHECK_THROW((void)protocol::decode_message(nonminimal), protocol::exceptions::invalid_encoding);
   }
   for (const auto hex : {"0b", "ff", "ffffffff0f", "8080808010", "0302", "0402", "018000", "01ffffffff0f"}) {
      BOOST_CHECK_THROW((void)protocol::decode_message(unhex(hex)), protocol::exceptions::invalid_encoding);
   }
   const auto too_big = std::vector<std::uint8_t>(protocol::max_wire_message_size + 1U);
   BOOST_CHECK_THROW((void)protocol::decode_message(too_big), protocol::exceptions::invalid_encoding);
}

BOOST_AUTO_TEST_CASE(stateless_validation_rejects_zero_ids_unknown_codes_versions_and_resource_excess) {
   using namespace message_test;
   auto messages = std::vector<protocol::peer_message>{
       {protocol::request{0U, {}, 0U, core::block_size}},
       {protocol::block{0U, {}, 0U, {1U}}},
       {protocol::cancel{0U}},
       {protocol::reject{0U}},
       {protocol::hash_request{0U}},
       {protocol::hash_response{0U}},
       {protocol::have{{core::max_files, 0U}}},
       {protocol::have{{0U, core::max_piece_count}}},
       {protocol::request{1U, {}, 1U, core::block_size}},
       {protocol::request{1U, {}, 0U, 0U}},
       {protocol::request{1U, {}, 0U, core::block_size + 1U}},
       {protocol::request{1U, {}, core::max_piece_size, 1U}},
       {protocol::request{1U, {}, 0xffffc000U, 0xffffffffU}},
       {protocol::block{1U, {}, 0U, {}}},
       {protocol::block{1U, {}, 0U, std::vector<std::uint8_t>(core::block_size + 1U)}},
       {protocol::inventory{std::vector<std::uint8_t>(protocol::max_bitmap_size + 1U)}},
       {protocol::reject{1U, static_cast<protocol::rejection>(0U)}},
       {protocol::reject{1U, static_cast<protocol::rejection>(255U)}},
       {protocol::hash_response{1U, {}, {}, {std::vector<core::sha256>(19U)}}},
       {protocol::handshake{1U, {}, std::string(129U, 'x'), 0U}},
   };
   for (const auto& message : messages) {
      BOOST_CHECK_THROW(protocol::validate(message), protocol::exceptions::invalid_message);
      BOOST_CHECK_THROW((void)protocol::encode_message(message), protocol::exceptions::invalid_message);
   }
   const auto version = protocol::peer_message{protocol::handshake{2U, {}, {}, 0U}};
   BOOST_CHECK_THROW(protocol::validate(version), protocol::exceptions::unsupported_version);
   BOOST_CHECK_THROW((void)protocol::decode_message(forge::raw::pack(version)),
                     protocol::exceptions::unsupported_version);
   const auto unknown = protocol::peer_message{protocol::reject{1U, static_cast<protocol::rejection>(5U)}};
   BOOST_CHECK_THROW((void)protocol::decode_message(forge::raw::pack(unknown)), protocol::exceptions::invalid_message);
}

BOOST_AUTO_TEST_CASE(manifest_ranges_require_exact_blocks_and_exact_tail_not_peer_verification_claims) {
   using namespace message_test;
   const auto descriptor = manifest(1U, 2U * core::block_size, core::block_size - 1U);
   const auto full = protocol::peer_message{protocol::request{1U, {0U, 0U}, 0U, core::block_size}};
   const auto tail = protocol::peer_message{protocol::request{2U, {0U, 0U}, core::block_size, 1U}};
   BOOST_CHECK_NO_THROW(protocol::validate(full, descriptor));
   BOOST_CHECK_NO_THROW(protocol::validate(tail, descriptor));
   const auto unverified =
       protocol::peer_message{protocol::block{1U, {}, 0U, std::vector<std::uint8_t>(core::block_size, 255U)}};
   BOOST_CHECK_NO_THROW(protocol::validate(unverified, descriptor));
   BOOST_CHECK_NO_THROW(
       protocol::validate(protocol::peer_message{protocol::block{2U, {}, core::block_size, {9U}}}, descriptor));
   for (const auto& item :
        std::vector<protocol::peer_message>{{protocol::request{1U, {}, 0U, core::block_size - 1U}},
                                            {protocol::request{1U, {}, core::block_size, core::block_size}},
                                            {protocol::request{1U, {}, 2U * core::block_size, 1U}},
                                            {protocol::request{1U, {1U, 0U}, 0U, 1U}},
                                            {protocol::request{1U, {0U, 1U}, 0U, 1U}},
                                            {protocol::block{1U, {}, core::block_size, {1U, 2U}}},
                                            {protocol::have{{0U, 1U}}},
                                            {protocol::hash_request{1U, {1U, 0U}}}}) {
      BOOST_CHECK_THROW(protocol::validate(item, descriptor), protocol::exceptions::invalid_message);
   }
   const auto id = core::calculate_swarm_id(descriptor);
   BOOST_CHECK_NO_THROW(
       protocol::validate(protocol::peer_message{protocol::handshake{1U, id, {}, 0xffffffffU}}, descriptor));
   BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{protocol::handshake{}}, descriptor),
                     protocol::exceptions::invalid_message);
}

BOOST_AUTO_TEST_CASE(inventory_geometry_covers_empty_seven_eight_nine_and_maximum_bitmaps) {
   using namespace message_test;
   for (const auto count : {0U, 7U, 8U, 9U}) {
      const auto descriptor =
          count == 0U ? core::descriptor{core::descriptor_version::v1, core::block_size, {{{"a"}, 0U, {}, {}}}}
                      : manifest(count);
      auto pieces = std::vector<std::uint8_t>((count + 7U) / 8U);
      for (auto i = 0U; i < count; ++i)
         pieces[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
      BOOST_CHECK_NO_THROW(protocol::validate(protocol::peer_message{protocol::inventory{pieces}}, descriptor));
      if (count % 8U != 0U) {
         pieces.back() |= 0x80U;
         BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{protocol::inventory{pieces}}, descriptor),
                           protocol::exceptions::invalid_message);
      }
      pieces.push_back(0U);
      BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{protocol::inventory{pieces}}, descriptor),
                        protocol::exceptions::invalid_message);
   }
   const auto maximum =
       protocol::peer_message{protocol::inventory{std::vector<std::uint8_t>(protocol::max_bitmap_size, 255U)}};
   const auto wire = protocol::encode_message(maximum);
   BOOST_TEST(wire.size() < protocol::max_wire_message_size);
   BOOST_CHECK(protocol::decode_message(wire) == maximum);
   const auto forged = unhex("01818002"); // claims 32769 byte entries, but none are present
   BOOST_CHECK_THROW((void)protocol::decode_message(forged), protocol::exceptions::invalid_encoding);
}

BOOST_AUTO_TEST_CASE(hash_proof_binds_to_expected_file_piece_root_and_short_file_geometry) {
   using namespace message_test;
   const auto descriptor = manifest(3U);
   const auto& file = descriptor.files[0];
   auto response = protocol::hash_response{
       1U, {0U, 2U}, file.pieces[2U], core::make_piece_proof(file.pieces, file.size, descriptor.piece_size, 2U)};
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::peer_message{response}, descriptor));
   auto wrong = response;
   wrong.hash = {};
   BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{wrong}, descriptor),
                     protocol::exceptions::invalid_message);
   wrong = response;
   wrong.piece.index = 0U;
   BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{wrong}, descriptor),
                     protocol::exceptions::invalid_message);
   wrong = response;
   wrong.proof.siblings.pop_back();
   BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{wrong}, descriptor),
                     protocol::exceptions::invalid_message);
   auto another = descriptor;
   another.files.push_back(file);
   another.files[1].path = {"b"};
   const auto other_hashes = core::hash_file(std::vector<std::uint8_t>(3U * core::block_size, 43U), core::block_size);
   another.files[1].root = other_hashes.root;
   another.files[1].pieces = other_hashes.pieces;
   // Geometry/proof validation uses the expected file root, not a remote claim.
   wrong = response;
   wrong.piece.file = 1U;
   BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{wrong}, another), protocol::exceptions::invalid_message);
   const auto short_file = manifest(1U, core::block_size, core::block_size - 1U);
   auto short_response = protocol::hash_response{1U, {}, short_file.files[0].root, {}};
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::peer_message{short_response}, short_file));
   short_response.proof.siblings.push_back({});
   BOOST_CHECK_THROW(protocol::validate(protocol::peer_message{short_response}, short_file),
                     protocol::exceptions::invalid_message);
}

BOOST_AUTO_TEST_CASE(decode_limits_tighten_parent_bytes_and_all_proof_container_budgets_before_allocation) {
   using namespace message_test;
   const auto block = protocol::peer_message{protocol::block{1U, {}, 0U, std::vector<std::uint8_t>(core::block_size)}};
   const auto wire = protocol::encode_message(block);
   auto limits = forge::raw::unpack_limits{};
   limits.max_bytes = core::block_size - 1U;
   BOOST_CHECK_THROW((void)protocol::decode_message(wire, limits), protocol::exceptions::invalid_encoding);
   limits.max_bytes = core::block_size;
   BOOST_CHECK(protocol::decode_message(wire, limits) == block);
   const auto proof = protocol::peer_message{protocol::hash_response{1U, {}, {}, {std::vector<core::sha256>(18U)}}};
   const auto proof_wire = protocol::encode_message(proof);
   BOOST_CHECK(protocol::decode_message(proof_wire) == proof);
   for (auto field = 0U; field < 3U; ++field) {
      limits = {};
      if (field == 0U)
         limits.max_container_elements = 17U;
      if (field == 1U)
         limits.max_total_container_elements = 17U;
      if (field == 2U)
         limits.first_container_elements = 17U;
      BOOST_CHECK_THROW((void)protocol::decode_message(proof_wire, limits), protocol::exceptions::invalid_encoding);
   }
   auto malicious = proof_wire;
   malicious[49U] = 19U;
   BOOST_CHECK_THROW((void)protocol::decode_message(malicious), protocol::exceptions::invalid_encoding);
   malicious[49U] = 255U;
   BOOST_CHECK_THROW((void)protocol::decode_message(malicious), protocol::exceptions::invalid_encoding);
}

BOOST_AUTO_TEST_SUITE_END()
