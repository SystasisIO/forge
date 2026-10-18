#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import forge.swarm.core.descriptor;
import forge.swarm.core.identity;
import forge.swarm.core.exceptions;
import forge.raw.raw;

namespace core = forge::swarm::core;

namespace {

std::vector<std::uint8_t> unhex(std::string_view hex) {
   auto bytes = std::vector<std::uint8_t>{};
   for (auto i = std::size_t{}; i < hex.size(); i += 2U) {
      bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(i, 2U)}, nullptr, 16)));
   }
   return bytes;
}

core::descriptor one_file() {
   const auto data = std::array<std::uint8_t, 1>{'a'};
   return {core::descriptor_version::v1, core::block_size, {{{"a"}, 1U, core::hash_file(data, core::block_size).root, {}}}};
}

constexpr auto a_wire = "0100000000400000010101610100000000000000"
   "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb00";
constexpr auto empty_wire = "0100000000400000010101650000000000000000"
   "000000000000000000000000000000000000000000000000000000000000000000";

} // namespace

BOOST_AUTO_TEST_SUITE(swarm_descriptor)

BOOST_AUTO_TEST_CASE(canonical_raw_and_identity_have_frozen_independent_bytes) {
   const auto descriptor = one_file();
   const auto wire = unhex(a_wire);
   BOOST_CHECK(core::encode_descriptor(descriptor) == wire);
   BOOST_CHECK(forge::raw::pack(descriptor) == wire);
   BOOST_CHECK(core::decode_descriptor(wire) == descriptor);
   BOOST_TEST(core::calculate_swarm_id(descriptor).value.str() ==
              "fce83635f30c1c132dc75136561c023f62db154a65bfc68e655668edd64069ab");
   BOOST_CHECK(core::calculate_swarm_id(wire) == core::calculate_swarm_id(descriptor));
   BOOST_TEST(core::validate_descriptor(descriptor).total_size == 1U);
}

BOOST_AUTO_TEST_CASE(empty_file_has_one_canonical_encoding) {
   const auto wire = unhex(empty_wire);
   const auto value = core::decode_descriptor(wire);
   BOOST_REQUIRE_EQUAL(value.files.size(), 1U);
   BOOST_TEST(value.files[0].size == 0U);
   BOOST_CHECK(value.files[0].root == core::sha256{});
   BOOST_TEST(core::calculate_swarm_id(value).value.str() ==
              "966a1c7e9685805707bec566ca527377980763084a3b1d343d34ed501ccbc1af");
   auto wrong = value;
   wrong.files[0].root = core::sha256::hash(std::string{"nonzero"});
   BOOST_CHECK_THROW((void)core::validate_descriptor(wrong), core::exceptions::invalid_descriptor);
}

BOOST_AUTO_TEST_CASE(decode_is_exact_and_rejects_nonminimal_counts_and_huge_prefixes) {
   auto bytes = unhex(a_wire);
   bytes.push_back(0U);
   BOOST_CHECK_THROW((void)core::decode_descriptor(bytes), core::exceptions::invalid_encoding);
   bytes = unhex(a_wire);
   bytes[8] = 0x81U;
   bytes.insert(bytes.begin() + 9U, 0U);
   BOOST_CHECK_THROW((void)core::decode_descriptor(bytes), core::exceptions::invalid_encoding);
   bytes = unhex(a_wire);
   bytes.resize(8U);
   bytes.insert(bytes.end(), {0xffU, 0xffU, 0xffU, 0xffU, 0x0fU});
   BOOST_CHECK_THROW((void)core::decode_descriptor(bytes), core::exceptions::invalid_encoding);
   bytes = unhex(a_wire);
   bytes[9] = 0xffU;
   bytes.insert(bytes.begin() + 10U, {0xffU, 0xffU, 0xffU, 0x0fU});
   BOOST_CHECK_THROW((void)core::decode_descriptor(bytes), core::exceptions::invalid_encoding);
}

BOOST_AUTO_TEST_CASE(every_truncated_prefix_is_rejected) {
   const auto wire = unhex(a_wire);
   for (auto size = std::size_t{}; size < wire.size(); ++size) {
      BOOST_CHECK_THROW((void)core::decode_descriptor(std::span{wire}.first(size)), core::exceptions::invalid_encoding);
   }
}

BOOST_AUTO_TEST_CASE(unsafe_components_are_rejected_without_rewriting) {
   for (const auto& component : std::vector<std::string>{"", ".", "..", "/tmp", "a/b", "a\\b", std::string{"a\0b", 3U}, "\xc0\xaf"}) {
      auto value = one_file();
      value.files[0].path = {component};
      BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_path);
   }
}

BOOST_AUTO_TEST_CASE(order_is_componentwise_and_prefix_conflicts_are_rejected) {
   auto value = one_file();
   auto first = value.files[0];
   first.path = {"a", "x"};
   auto second = value.files[0];
   second.path = {"a-"};
   value.files = {first, second};
   BOOST_TEST(core::validate_descriptor(value).total_size == 2U);
   std::swap(value.files[0], value.files[1]);
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_path);
   value.files = {first, first};
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_path);
   first.path = {"a"};
   second.path = {"a", "x"};
   value.files = {first, second};
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_path);
}

BOOST_AUTO_TEST_CASE(identities_bind_paths_lengths_and_piece_geometry) {
   const auto original = one_file();
   auto changed = original;
   changed.files[0].path = {"b"};
   BOOST_CHECK(core::calculate_swarm_id(changed) != core::calculate_swarm_id(original));
   changed = original;
   changed.piece_size *= 2U;
   BOOST_CHECK(core::calculate_swarm_id(changed) != core::calculate_swarm_id(original));
}

BOOST_AUTO_TEST_CASE(equal_merkle_roots_with_different_file_lengths_have_different_ids) {
   const auto d = unhex("4fe7b59af6de3b665b67788cc2f99892ab827efae3a467342b3bb4e3bc8e5bfe");
   auto compact = d;
   compact.insert(compact.end(), d.begin(), d.end());
   const auto large = std::vector<std::uint8_t>(2U * core::block_size, 0U);
   const auto first = core::hash_file(compact, 2U * core::block_size);
   const auto second = core::hash_file(large, 2U * core::block_size);
   BOOST_CHECK(first.root == second.root);
   auto left = one_file();
   left.piece_size = 2U * core::block_size;
   left.files[0].size = compact.size();
   left.files[0].root = first.root;
   auto right = left;
   right.files[0].size = large.size();
   BOOST_CHECK(core::calculate_swarm_id(left) != core::calculate_swarm_id(right));
}

BOOST_AUTO_TEST_CASE(utf8_and_case_sensitive_logical_paths_preserve_exact_bytes) {
   auto value = one_file();
   const auto original = value.files[0];
   auto upper = original;
   upper.path = {"A"};
   auto lower = original;
   lower.path = {"a"};
   auto cyrillic = original;
   cyrillic.path = {"\xd0\xb4\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x8b\xd0\xb5.bin"};
   value.files = {upper, lower, cyrillic};
   const auto restored = core::decode_descriptor(core::encode_descriptor(value));
   BOOST_CHECK(restored == value);
   BOOST_TEST(restored.files.back().path.front() == cyrillic.path.front());
   auto nfc = one_file();
   nfc.files[0].path = {"\xc3\xa9"};
   auto nfd = nfc;
   nfd.files[0].path = {"e\xcc\x81"};
   BOOST_CHECK(core::calculate_swarm_id(nfc) != core::calculate_swarm_id(nfd));
   std::swap(value.files[0], value.files[2]);
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_path);
}

BOOST_AUTO_TEST_CASE(layer_mismatch_and_short_file_extra_layer_are_rejected) {
   auto value = one_file();
   value.files[0].pieces = {value.files[0].root};
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_descriptor);
   const auto bytes = std::vector<std::uint8_t>(core::block_size + 1U, 42U);
   const auto hashes = core::hash_file(bytes, core::block_size);
   value.files[0].size = bytes.size();
   value.files[0].root = hashes.root;
   value.files[0].pieces = hashes.pieces;
   BOOST_CHECK(core::decode_descriptor(core::encode_descriptor(value)) == value);
   value.files[0].pieces.back() = core::sha256{};
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_descriptor);
}

BOOST_AUTO_TEST_CASE(descriptor_bounds_reject_unsupported_or_excessive_inputs) {
   auto value = one_file();
   value.version = static_cast<core::descriptor_version>(2U);
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::unsupported_version);
   value = one_file();
   value.files.clear();
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::invalid_descriptor);
   value = one_file();
   value.files[0].size = std::numeric_limits<std::uint64_t>::max();
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::limit_exceeded);
   value = one_file();
   value.files[0].path = {std::string(core::max_component_size + 1U, 'a')};
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(all_variable_length_fields_are_checked_before_decode_allocations) {
   const auto wire = unhex(a_wire);
   // The frozen one-file fixture has count fields at these exact offsets.
   for (auto offset : {8U, 9U, 10U, 52U}) {
      auto nonminimal = wire;
      nonminimal[offset] |= 0x80U;
      nonminimal.insert(nonminimal.begin() + offset + 1U, 0U);
      BOOST_CHECK_THROW((void)core::decode_descriptor(nonminimal), core::exceptions::invalid_encoding);
      auto huge = wire;
      huge[offset] = 0xffU;
      huge.insert(huge.begin() + offset + 1U, {0xffU, 0xffU, 0xffU, 0x0fU});
      BOOST_CHECK_THROW((void)core::decode_descriptor(huge), core::exceptions::invalid_encoding);
   }
   const auto oversized = std::vector<std::uint8_t>(core::max_metadata_size + 1U, 0U);
   BOOST_CHECK_THROW((void)core::decode_descriptor(oversized), core::exceptions::limit_exceeded);
   auto value = one_file();
   value.files.resize(core::max_files + 1U, value.files.front());
   BOOST_CHECK_THROW((void)core::validate_descriptor(value), core::exceptions::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(complete_wire_preserves_typed_path_validation_errors) {
   auto value = one_file();
   value.files[0].path = {std::string(core::max_component_size + 1U, 'a')};
   BOOST_CHECK_THROW((void)core::decode_descriptor(forge::raw::pack(value)), core::exceptions::limit_exceeded);
   value.files[0].path.assign(core::max_path_depth + 1U, "a");
   BOOST_CHECK_THROW((void)core::decode_descriptor(forge::raw::pack(value)), core::exceptions::limit_exceeded);
   value.files[0].path.clear();
   BOOST_CHECK_THROW((void)core::decode_descriptor(forge::raw::pack(value)), core::exceptions::invalid_path);
   value.files[0].path = {""};
   BOOST_CHECK_THROW((void)core::decode_descriptor(forge::raw::pack(value)), core::exceptions::invalid_path);
}

BOOST_AUTO_TEST_SUITE_END()
