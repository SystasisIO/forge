#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

import forge.swarm.core.merkle;
import forge.swarm.core.exceptions;
import forge.raw.raw;

namespace core = forge::swarm::core;

namespace {

constexpr auto a = "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb";
constexpr auto d = "4fe7b59af6de3b665b67788cc2f99892ab827efae3a467342b3bb4e3bc8e5bfe";
constexpr auto t = "c36d0dd6a886e1fce758b6b5c531b703a1f21e8f6453785c390931cf8fa8a76d";
constexpr auto u = "60aae9c7b428f87e0713e88229e18f0adf12cd7b22a0dd8a92bb2485eb7af242";
constexpr auto e = "f73902761f5d940342851b717c1d23bc1f78b1e93d554db2b55b0ef4a045de8f";
constexpr auto v = "a8efe8010d28c9703af50f4293d734b397d1ed6fd7e7029aa6b18b7e7304db45";
constexpr auto z1 = "f5a5fd42d16a20302798ef6ed309979b43003d2320d9f0e8ea9831a92759fb4b";
constexpr auto root = "c19d6b310c5d5c3df97f726886bd7be10d5b969eb02a152867549d301efba617";

std::vector<std::uint8_t> sample() {
   auto bytes = std::vector<std::uint8_t>(4U * core::block_size, 0U);
   bytes.push_back('a');
   return bytes;
}

} // namespace

BOOST_AUTO_TEST_SUITE(swarm_merkle)

BOOST_AUTO_TEST_CASE(roots_match_independent_bep52_vectors) {
   const auto bytes = sample();
   const auto expected = std::array<std::vector<core::sha256>, 4>{
      std::vector<core::sha256>{core::sha256{d}, core::sha256{d}, core::sha256{d}, core::sha256{d}, core::sha256{a}},
      std::vector<core::sha256>{core::sha256{t}, core::sha256{t}, core::sha256{e}},
      std::vector<core::sha256>{core::sha256{u}, core::sha256{v}},
      std::vector<core::sha256>{},
   };
   for (auto i = 0U; i < expected.size(); ++i) {
      const auto piece_size = core::block_size << i;
      const auto hashes = core::hash_file(bytes, piece_size);
      BOOST_TEST(hashes.root.str() == root);
      BOOST_CHECK(hashes.pieces == expected[i]);
      if (!hashes.pieces.empty()) {
         BOOST_TEST(core::root_from_piece_layer(hashes.pieces, bytes.size(), piece_size).str() == root);
      }
   }
}

BOOST_AUTO_TEST_CASE(padding_is_digest_padding_not_zero_bytes_or_last_leaf_duplication) {
   auto bytes = std::vector<std::uint8_t>(core::block_size - 1U, 0U);
   BOOST_TEST(core::hash_file(bytes, core::block_size).root.str() ==
              "3202cc6a8c4ac4b66337a899428e1bc94f2ab7695c2ac7271c91324010f1c452");
   bytes.resize(3U * core::block_size, 0U);
   BOOST_TEST(core::hash_file(bytes, core::block_size).root.str() ==
              "1cb5a249f6e721060aae623d21d3eca23056bda8c19e4e89f9a086151553713d");
   BOOST_CHECK(core::hash_file(bytes, core::block_size).root != core::sha256{u});
}

BOOST_AUTO_TEST_CASE(short_files_use_natural_root_and_empty_files_use_zero_sentinel) {
   const auto bytes = std::array<std::uint8_t, 1>{'a'};
   const auto short_file = core::hash_file(bytes, 8U * core::block_size);
   BOOST_TEST(short_file.root.str() == a);
   BOOST_TEST(short_file.pieces.empty());
   const auto empty = core::hash_file({}, core::block_size);
   BOOST_CHECK(empty.root == core::sha256{});
   BOOST_TEST(empty.pieces.empty());
   BOOST_TEST(core::piece_count(0U, core::block_size) == 0U);
}

BOOST_AUTO_TEST_CASE(last_piece_proofs_match_known_padding_at_each_height) {
   const auto bytes = sample();
   const auto hashes = core::hash_file(bytes, 2U * core::block_size);
   const auto proof = core::make_piece_proof(hashes.pieces, bytes.size(), 2U * core::block_size, 2U);
   BOOST_REQUIRE_EQUAL(proof.siblings.size(), 2U);
   BOOST_TEST(proof.siblings[0].str() == z1);
   BOOST_TEST(proof.siblings[1].str() == u);
   const auto raw_proof = forge::raw::pack(proof);
   BOOST_REQUIRE_EQUAL(raw_proof.size(), 65U);
   BOOST_TEST(raw_proof.front() == 2U);
   BOOST_CHECK(std::equal(raw_proof.begin() + 1U, raw_proof.begin() + 33U, proof.siblings[0].to_uint8_span().begin()));
   BOOST_CHECK(std::equal(raw_proof.begin() + 33U, raw_proof.end(), proof.siblings[1].to_uint8_span().begin()));
   const auto last = std::span{bytes}.last(1U);
   BOOST_CHECK(core::verify_piece(last, bytes.size(), 2U * core::block_size, 2U, proof, hashes.root));
   auto wrong = proof;
   wrong.siblings[0] = core::sha256{};
   BOOST_CHECK(!core::verify_piece(last, bytes.size(), 2U * core::block_size, 2U, wrong, hashes.root));
   wrong = proof;
   std::swap(wrong.siblings[0], wrong.siblings[1]);
   BOOST_CHECK(!core::verify_piece(last, bytes.size(), 2U * core::block_size, 2U, wrong, hashes.root));
   wrong = proof;
   wrong.siblings.push_back(core::sha256{});
   BOOST_CHECK(!core::verify_piece(last, bytes.size(), 2U * core::block_size, 2U, wrong, hashes.root));
}

BOOST_AUTO_TEST_CASE(self_consistent_forged_root_cannot_hide_noncanonical_empty_subtrees) {
   const auto pair = [](const core::sha256& left, const core::sha256& right) {
      auto encoder = core::sha256::encoder{};
      encoder.write(left.to_uint8_span());
      encoder.write(right.to_uint8_span());
      return encoder.result();
   };
   const auto bytes = sample();
   const auto hashes = core::hash_file(bytes, 2U * core::block_size);
   auto proof = core::make_piece_proof(hashes.pieces, bytes.size(), 2U * core::block_size, 2U);
   proof.siblings[0] = core::sha256{};
   const auto forged_root = pair(proof.siblings[1], pair(core::sha256{e}, proof.siblings[0]));
   BOOST_CHECK(!core::verify_piece(std::span{bytes}.last(1U), bytes.size(), 2U * core::block_size,
                                   2U, proof, forged_root));
}

BOOST_AUTO_TEST_CASE(every_piece_verifies_and_tampering_fails) {
   const auto bytes = sample();
   for (auto piece_size : {core::block_size, 2U * core::block_size, 4U * core::block_size, 8U * core::block_size}) {
      const auto hashes = core::hash_file(bytes, piece_size);
      const auto count = core::piece_count(bytes.size(), piece_size);
      for (auto i = 0U; i < count; ++i) {
         const auto offset = static_cast<std::size_t>(i) * piece_size;
         const auto data = std::span{bytes}.subspan(offset, std::min<std::size_t>(piece_size, bytes.size() - offset));
         const auto proof = core::make_piece_proof(hashes.pieces, bytes.size(), piece_size, i);
         BOOST_CHECK(core::verify_piece(data, bytes.size(), piece_size, i, proof, hashes.root));
         auto tampered = std::vector<std::uint8_t>{data.begin(), data.end()};
         tampered[0] ^= 1U;
         BOOST_CHECK(!core::verify_piece(tampered, bytes.size(), piece_size, i, proof, hashes.root));
      }
   }
}

BOOST_AUTO_TEST_CASE(streaming_hasher_matches_all_boundary_geometries) {
   for (auto size : {0U, 1U, core::block_size - 1U, core::block_size, core::block_size + 1U,
                    2U * core::block_size - 1U, 2U * core::block_size, 2U * core::block_size + 1U}) {
      const auto bytes = std::vector<std::uint8_t>(size, 0x5a);
      auto streaming = core::file_hasher{bytes.size(), 2U * core::block_size};
      for (auto offset = std::size_t{}; offset < bytes.size(); offset += core::block_size) {
         streaming.append_block(std::span{bytes}.subspan(offset, std::min<std::size_t>(core::block_size, bytes.size() - offset)));
      }
      BOOST_CHECK(streaming.finish() == core::hash_file(bytes, 2U * core::block_size));
   }
}

BOOST_AUTO_TEST_CASE(streaming_rejects_incomplete_and_bad_blocks_without_mutation) {
   auto hasher = core::file_hasher{core::block_size + 1U, core::block_size};
   const auto short_block = std::array<std::uint8_t, 1>{'a'};
   BOOST_CHECK_THROW(hasher.append_block(short_block), core::exceptions::invalid_content);
   BOOST_CHECK_THROW((void)hasher.finish(), core::exceptions::invalid_content);
   const auto block = std::vector<std::uint8_t>(core::block_size, 0U);
   hasher.append_block(block);
   hasher.append_block(short_block);
   auto all = block;
   all.push_back('a');
   BOOST_CHECK(hasher.finish() == core::hash_file(all, core::block_size));
   BOOST_CHECK_THROW(hasher.append_block(short_block), core::exceptions::invalid_content);
}

BOOST_AUTO_TEST_CASE(maximum_piece_height_streams_without_a_file_sized_buffer) {
   const auto block = std::vector<std::uint8_t>(core::block_size, 0U);
   const auto tail = std::array<std::uint8_t, 1>{'a'};
   auto hasher = core::file_hasher{core::max_piece_size + 1U, core::max_piece_size};
   for (auto i = 0U; i < core::max_piece_size / core::block_size; ++i) {
      hasher.append_block(block);
   }
   hasher.append_block(tail);
   const auto hashes = hasher.finish();
   BOOST_REQUIRE_EQUAL(hashes.pieces.size(), 2U);
   BOOST_TEST(hashes.pieces[0].str() == "c8d2a90e63fef3657908231f1006c64a5c2af966460d3e7af193f3369c442c77");
   BOOST_TEST(hashes.pieces[1].str() == "349b0194e9704fe376e0876348449f9970de9fe0cd16e28d8563c2c7f6ce2ecf");
   BOOST_TEST(hashes.root.str() == "09e587c9d38b0ec18a61c5027f0538db534290ed3bb6b2f75b682c22a4147f21");
   const auto proof = core::make_piece_proof(hashes.pieces, core::max_piece_size + 1U, core::max_piece_size, 1U);
   BOOST_CHECK(core::verify_piece(tail, core::max_piece_size + 1U, core::max_piece_size, 1U, proof, hashes.root));
}

BOOST_AUTO_TEST_CASE(geometry_and_resource_limits_fail_before_large_work) {
   BOOST_CHECK_THROW(core::validate_piece_size(0U), core::exceptions::invalid_geometry);
   BOOST_CHECK_THROW(core::validate_piece_size(core::block_size / 2U), core::exceptions::invalid_geometry);
   BOOST_CHECK_THROW(core::validate_piece_size(core::block_size + 1U), core::exceptions::invalid_geometry);
   BOOST_CHECK_THROW(core::validate_piece_size(core::max_piece_size * 2U), core::exceptions::invalid_geometry);
   BOOST_CHECK_THROW((void)core::piece_count(std::numeric_limits<std::uint64_t>::max(), core::block_size),
                     core::exceptions::limit_exceeded);
   BOOST_CHECK_THROW((void)core::root_from_piece_layer({}, core::block_size + 1U, core::block_size),
                     core::exceptions::invalid_geometry);
   BOOST_CHECK(!core::verify_piece({}, std::numeric_limits<std::uint64_t>::max(), 0U,
                                  std::numeric_limits<std::uint32_t>::max(), {}, core::sha256{}));
}

BOOST_AUTO_TEST_SUITE_END()
