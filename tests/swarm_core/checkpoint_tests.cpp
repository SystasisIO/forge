#include <boost/test/unit_test.hpp>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import forge.swarm.core.checkpoint;

namespace checkpoint_test {
namespace core = forge::swarm::core;

core::descriptor manifest() {
   const auto bytes = std::array<std::uint8_t, 1>{'a'};
   return {core::descriptor_version::v1, core::block_size, {{{"a"}, 1U, core::hash_file(bytes, core::block_size).root, {}}}};
}
std::vector<std::uint8_t> unhex(std::string_view value) {
   auto bytes = std::vector<std::uint8_t>{};
   for (auto i = std::size_t{}; i < value.size(); i += 2U) {
      bytes.push_back(static_cast<std::uint8_t>(std::stoul(std::string{value.substr(i, 2U)}, nullptr, 16)));
   }
   return bytes;
}
}

BOOST_AUTO_TEST_SUITE(swarm_checkpoint)

BOOST_AUTO_TEST_CASE(checksummed_raw_layout_has_independent_frozen_bytes) {
   namespace core = forge::swarm::core;
   const auto manifest = checkpoint_test::manifest();
   const auto state = core::resume_state{1U, core::calculate_swarm_id(manifest), 7U, false, {1U}, {}};
   const auto bytes = checkpoint_test::unhex("01000000fce83635f30c1c132dc75136561c023f62db154a65bfc68e655668edd64069ab"
      "070000000000000000010100dfc6f0320bc29607a4c9d0d2d443193699d7e52d649aec89b34638830ccef3d5");
   BOOST_CHECK(core::encode_checkpoint(state, manifest) == bytes);
   BOOST_CHECK(core::decode_checkpoint(bytes, manifest) == state);
   for (auto i = std::size_t{}; i < bytes.size(); ++i) {
      auto corrupt = bytes;
      corrupt[i] ^= 0x80U;
      BOOST_CHECK_THROW((void)core::decode_checkpoint(corrupt, manifest), core::exceptions::invalid_checkpoint);
      BOOST_CHECK_THROW((void)core::decode_checkpoint(std::span{bytes}.first(i), manifest), core::exceptions::invalid_checkpoint);
   }
}

BOOST_AUTO_TEST_CASE(checkpoint_geometry_and_allocation_limits_are_enforced) {
   namespace core = forge::swarm::core;
   const auto manifest = checkpoint_test::manifest();
   auto state = core::resume_state{1U, core::calculate_swarm_id(manifest), 1U, false, {0U}, {}};
   const auto canonical = core::encode_checkpoint(state, manifest);
   auto huge = canonical;
   huge[45] = 0xffU;
   huge.insert(huge.begin() + 46U, {0xffU, 0xffU, 0xffU, 0x0fU});
   BOOST_CHECK_THROW((void)core::decode_checkpoint(huge, manifest), core::exceptions::invalid_checkpoint);
   auto nonminimal = canonical;
   nonminimal[45] = 0x81U;
   nonminimal.insert(nonminimal.begin() + 46U, 0U);
   BOOST_CHECK_THROW((void)core::decode_checkpoint(nonminimal, manifest), core::exceptions::invalid_checkpoint);
   state.verified[0] = 2U;
   BOOST_CHECK_THROW(core::validate_checkpoint(state, manifest), core::exceptions::invalid_checkpoint);
   state.verified[0] = 0U;
   state.published = true;
   BOOST_CHECK_THROW(core::validate_checkpoint(state, manifest), core::exceptions::invalid_checkpoint);
   state.published = false;
   state.generation = std::numeric_limits<std::uint64_t>::max();
   BOOST_CHECK_THROW(core::validate_checkpoint(state, manifest), core::exceptions::invalid_checkpoint);
   const auto oversized = std::vector<std::uint8_t>(core::max_checkpoint_size + 1U);
   BOOST_CHECK_THROW((void)core::decode_checkpoint(oversized, manifest), core::exceptions::invalid_checkpoint);
}

BOOST_AUTO_TEST_CASE(unfinished_indices_order_and_trailing_bits_are_validated) {
   namespace core = forge::swarm::core;
   const auto manifest = checkpoint_test::manifest();
   auto state = core::resume_state{1U, core::calculate_swarm_id(manifest), 1U, false, {0U}, {{{0, 0}, {1U}}}};
   BOOST_CHECK(core::decode_checkpoint(core::encode_checkpoint(state, manifest), manifest) == state);
   state.unfinished.push_back(state.unfinished.front());
   BOOST_CHECK_THROW(core::validate_checkpoint(state, manifest), core::exceptions::invalid_checkpoint);
   state.unfinished.resize(1);
   state.unfinished.front().blocks[0] = 2U;
   BOOST_CHECK_THROW(core::validate_checkpoint(state, manifest), core::exceptions::invalid_checkpoint);
   state.unfinished.front().blocks[0] = 1U;
   state.verified[0] = 1U;
   BOOST_CHECK_THROW(core::validate_checkpoint(state, manifest), core::exceptions::invalid_checkpoint);
}

BOOST_AUTO_TEST_SUITE_END()
