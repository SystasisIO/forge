#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.swarm.protocol.metadata_reader;
import forge.swarm.protocol.metadata_assembler;
import forge.raw.raw;

namespace metadata_test {
namespace core = forge::swarm::core;
namespace protocol = forge::swarm::protocol;

protocol::metadata fixture() {
   const auto content = std::array<std::uint8_t, 1>{'a'};
   auto manifest = core::descriptor{core::descriptor_version::v1,
                                    core::block_size,
                                    {{{"a"}, 1U, core::hash_file(content, core::block_size).root, {}}}};
   return {core::calculate_swarm_id(manifest), std::move(manifest)};
}

std::vector<std::uint8_t> unhex(std::string_view hex) {
   auto bytes = std::vector<std::uint8_t>{};
   for (auto i = std::size_t{}; i < hex.size(); i += 2U) {
      bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string{hex.substr(i, 2U)}, nullptr, 16)));
   }
   return bytes;
}

core::descriptor descriptor_with_size(std::uint32_t target) {
   auto manifest = core::descriptor{};
   if (target < 65536U) {
      for (auto i = 0U; i < 64U; ++i)
         manifest.files.push_back({{std::to_string(1000U + i)}, 0U, {}, {}});
   } else {
      const auto digest = core::hash_file(std::array<std::uint8_t, 1>{'a'}, core::block_size).root;
      const auto layer = std::vector<core::sha256>(64U, digest);
      const auto size = std::uint64_t{64U} * core::block_size;
      const auto root = core::root_from_piece_layer(layer, size, core::block_size);
      for (auto i = 0U; i < core::max_files; ++i) {
         auto path = std::vector<std::string>{std::to_string(1000U + i)};
         for (auto component = 0U; component < 8U; ++component)
            path.emplace_back(200U, 'x');
         manifest.files.push_back({std::move(path), size, root, layer});
      }
   }
   auto current = forge::raw::pack_size(manifest);
   for (auto& file : manifest.files) {
      for (auto i = target < 65536U ? 0U : 1U; i < file.path.size(); ++i) {
         while (file.path[i].size() < core::max_component_size && current < target) {
            const auto delta = file.path[i].size() == 127U ? 2U : 1U;
            if (delta > target - current)
               break;
            file.path[i].push_back('x');
            current += delta;
         }
      }
   }
   BOOST_REQUIRE_EQUAL(current, target);
   BOOST_REQUIRE_EQUAL(forge::raw::pack_size(manifest), target);
   return manifest;
}

} // namespace metadata_test

BOOST_AUTO_TEST_SUITE(swarm_protocol_metadata)

BOOST_AUTO_TEST_CASE(metadata_and_chunk_have_independent_golden_raw_bytes) {
   using namespace metadata_test;
   const auto value = fixture();
   const auto descriptor_bytes = unhex("0100000000400000010101610100000000000000"
                                       "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb00");
   const auto metadata_bytes = unhex("fce83635f30c1c132dc75136561c023f62db154a65bfc68e655668edd64069ab"
                                     "0100000000400000010101610100000000000000"
                                     "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb00");
   BOOST_CHECK(forge::raw::pack(value) == metadata_bytes);
   auto reader = protocol::metadata_reader{value};
   const auto chunk = reader.next();
   BOOST_REQUIRE(chunk);
   BOOST_CHECK(chunk->data == descriptor_bytes);
   BOOST_CHECK(!reader.next());
   const auto one = protocol::metadata_chunk{1U, 0U, {'a'}};
   const auto wire = unhex("01000000000000000161");
   BOOST_CHECK(protocol::encode_metadata_chunk(one) == wire);
   BOOST_CHECK(forge::raw::pack(one) == wire);
   BOOST_CHECK(protocol::decode_metadata_chunk(wire) == one);
}

BOOST_AUTO_TEST_CASE(reader_and_assembler_cross_exact_16k_boundaries_without_partial_publication) {
   using namespace metadata_test;
   for (const auto size : {core::block_size - 1U, core::block_size, core::block_size + 1U}) {
      auto manifest = descriptor_with_size(size);
      const auto value = protocol::metadata{core::calculate_swarm_id(manifest), std::move(manifest)};
      auto reader = protocol::metadata_reader{value};
      auto assembler = protocol::metadata_assembler{value.swarm};
      BOOST_CHECK(!assembler.complete());
      BOOST_CHECK_THROW((void)std::move(assembler).finish(), protocol::exceptions::invalid_metadata);
      auto offset = 0U;
      while (const auto chunk = reader.next()) {
         BOOST_TEST(chunk->total == size);
         BOOST_TEST(chunk->offset == offset);
         BOOST_TEST(chunk->data.size() == std::min(core::block_size, size - offset));
         const auto wire = protocol::encode_metadata_chunk(*chunk);
         assembler.append(protocol::decode_metadata_chunk(wire));
         offset += static_cast<std::uint32_t>(chunk->data.size());
         BOOST_TEST(assembler.received_size() == offset);
         if (offset < size)
            BOOST_CHECK_THROW((void)std::move(assembler).finish(), protocol::exceptions::invalid_metadata);
      }
      BOOST_CHECK(assembler.complete());
      BOOST_CHECK(std::move(assembler).finish() == value);
      BOOST_CHECK_THROW((void)std::move(assembler).finish(), protocol::exceptions::invalid_metadata);
   }
}

BOOST_AUTO_TEST_CASE(canonical_metadata_at_16mib_cap_roundtrips_and_cap_plus_one_is_rejected) {
   using namespace metadata_test;
   auto manifest = descriptor_with_size(core::max_metadata_size);
   auto value = protocol::metadata{core::calculate_swarm_id(manifest), std::move(manifest)};
   auto reader = protocol::metadata_reader{value};
   auto assembler = protocol::metadata_assembler{value.swarm};
   auto chunks = 0U;
   while (const auto chunk = reader.next()) {
      assembler.append(*chunk);
      ++chunks;
   }
   BOOST_TEST(chunks == core::max_metadata_size / protocol::metadata_chunk_size);
   BOOST_TEST(assembler.received_size() == core::max_metadata_size);
   BOOST_CHECK(assembler.complete());
   BOOST_CHECK(std::move(assembler).finish() == value);
   value.manifest.files.back().path.back().push_back('x');
   BOOST_TEST(forge::raw::pack_size(value.manifest) == core::max_metadata_size + 1U);
   BOOST_CHECK_THROW((protocol::metadata_reader{value}), protocol::exceptions::invalid_metadata);
   const auto too_large = protocol::metadata_chunk{core::max_metadata_size + 1U, 0U,
                                                   std::vector<std::uint8_t>(protocol::metadata_chunk_size)};
   BOOST_CHECK_THROW(protocol::validate(too_large), protocol::exceptions::invalid_metadata);
}

BOOST_AUTO_TEST_CASE(chunk_semantics_reject_overflow_unaligned_short_empty_and_out_of_order_values) {
   using namespace metadata_test;
   const auto valid =
       protocol::metadata_chunk{core::block_size + 1U, 0U, std::vector<std::uint8_t>(core::block_size, 7U)};
   for (auto mutation = 0U; mutation < 7U; ++mutation) {
      auto bad = valid;
      if (mutation == 0U)
         bad.total = 0U;
      if (mutation == 1U)
         bad.total = core::max_metadata_size + 1U;
      if (mutation == 2U)
         bad.offset = 1U;
      if (mutation == 3U)
         bad.offset = bad.total;
      if (mutation == 4U)
         bad.offset = 0xffffffffU;
      if (mutation == 5U)
         bad.data.pop_back();
      if (mutation == 6U)
         bad.data.clear();
      BOOST_CHECK_THROW(protocol::validate(bad), protocol::exceptions::invalid_metadata);
   }
   auto assembler = protocol::metadata_assembler{fixture().swarm};
   const auto tail = protocol::metadata_chunk{core::block_size + 1U, core::block_size, {7U}};
   BOOST_CHECK_THROW(assembler.append(tail), protocol::exceptions::invalid_metadata);
   BOOST_TEST(assembler.received_size() == 0U);
   assembler.append(valid);
   BOOST_CHECK_THROW(assembler.append(valid), protocol::exceptions::invalid_metadata);
   auto changed_total = tail;
   changed_total.total = 2U * core::block_size;
   changed_total.data.resize(core::block_size);
   BOOST_CHECK_THROW(assembler.append(changed_total), protocol::exceptions::invalid_metadata);
   BOOST_TEST(assembler.received_size() == core::block_size);
   assembler.append(tail);
   BOOST_CHECK(assembler.complete());
   BOOST_CHECK_THROW(assembler.append(tail), protocol::exceptions::invalid_metadata);
   BOOST_CHECK_THROW((void)std::move(assembler).finish(), protocol::exceptions::invalid_metadata);
}

BOOST_AUTO_TEST_CASE(reader_and_assembly_reject_identity_mismatch_and_noncanonical_descriptor_bytes) {
   using namespace metadata_test;
   const auto original = fixture();
   auto value = original;
   value.swarm = {};
   BOOST_CHECK_THROW((protocol::metadata_reader{value}), protocol::exceptions::invalid_metadata);
   BOOST_CHECK_THROW(protocol::validate(value), protocol::exceptions::invalid_metadata);
   auto wrong = protocol::metadata_assembler{value.swarm};
   const auto bytes = core::encode_descriptor(original.manifest);
   wrong.append({static_cast<std::uint32_t>(bytes.size()), 0U, bytes});
   BOOST_CHECK_THROW((void)std::move(wrong).finish(), protocol::exceptions::invalid_metadata);
   auto noncanonical = bytes;
   noncanonical[8] = 0x81U;
   noncanonical.insert(noncanonical.begin() + 9U, 0U);
   auto assembler = protocol::metadata_assembler{{core::sha256::hash(std::span<const std::uint8_t>{noncanonical})}};
   assembler.append({static_cast<std::uint32_t>(noncanonical.size()), 0U, noncanonical});
   BOOST_CHECK_THROW((void)std::move(assembler).finish(), protocol::exceptions::invalid_metadata);
}

BOOST_AUTO_TEST_CASE(bounded_chunk_decoder_rejects_forged_prefixes_trailing_bytes_and_tight_parent_limits) {
   using namespace metadata_test;
   const auto valid = unhex("01000000000000000161");
   for (auto size = std::size_t{}; size < valid.size(); ++size) {
      BOOST_CHECK_THROW((void)protocol::decode_metadata_chunk(std::span{valid}.first(size)),
                        protocol::exceptions::invalid_encoding);
   }
   auto trailing = valid;
   trailing.push_back(0U);
   BOOST_CHECK_THROW((void)protocol::decode_metadata_chunk(trailing), protocol::exceptions::invalid_encoding);
   auto nonminimal = valid;
   nonminimal[8] = 0x81U;
   nonminimal.insert(nonminimal.begin() + 9U, 0U);
   BOOST_CHECK_THROW((void)protocol::decode_metadata_chunk(nonminimal), protocol::exceptions::invalid_encoding);
   auto huge = unhex("0100000000000000ffffffff0f");
   BOOST_CHECK_THROW((void)protocol::decode_metadata_chunk(huge), protocol::exceptions::invalid_encoding);
   huge = unhex("01000000000000008080808010");
   BOOST_CHECK_THROW((void)protocol::decode_metadata_chunk(huge), protocol::exceptions::invalid_encoding);
   auto limits = forge::raw::unpack_limits{};
   limits.max_bytes = 0U;
   BOOST_CHECK_THROW((void)protocol::decode_metadata_chunk(valid, limits), protocol::exceptions::invalid_encoding);
   limits.max_bytes = 1U;
   BOOST_CHECK(protocol::decode_metadata_chunk(valid, limits) == (protocol::metadata_chunk{1U, 0U, {'a'}}));
   const auto maximum = protocol::metadata_chunk{core::block_size, 0U, std::vector<std::uint8_t>(core::block_size)};
   const auto maximum_wire = protocol::encode_metadata_chunk(maximum);
   BOOST_TEST(maximum_wire.size() == protocol::max_metadata_chunk_wire_size);
   BOOST_CHECK(protocol::decode_metadata_chunk(maximum_wire) == maximum);
   auto over = maximum_wire;
   over.push_back(0U);
   BOOST_CHECK_THROW((void)protocol::decode_metadata_chunk(over), protocol::exceptions::invalid_encoding);
}

BOOST_AUTO_TEST_CASE(realms_are_bounded_printable_ascii_without_identity_or_authorization_claims) {
   using namespace metadata_test;
   const auto id = fixture().swarm;
   for (const auto& realm : {std::string{}, std::string{"realm"}, std::string(128U, '~')}) {
      BOOST_CHECK_NO_THROW(protocol::validate(protocol::metadata_request{id, realm}));
      BOOST_CHECK_NO_THROW(protocol::validate(protocol::exchange_request{id, realm}));
   }
   for (const auto& realm :
        {std::string(129U, 'x'), std::string{"\n"}, std::string{"\x7f"}, std::string{"\x80"}, std::string(1U, '\0')}) {
      BOOST_CHECK_THROW(protocol::validate(protocol::metadata_request{id, realm}),
                        protocol::exceptions::invalid_message);
      BOOST_CHECK_THROW(protocol::validate(protocol::exchange_request{id, realm}),
                        protocol::exceptions::invalid_message);
   }
}

BOOST_AUTO_TEST_SUITE_END()
