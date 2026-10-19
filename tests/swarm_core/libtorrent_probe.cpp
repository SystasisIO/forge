#include <libtorrent/aux_/merkle.hpp>
#include <libtorrent/hasher.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

import forge.swarm.core.merkle;

namespace core = forge::swarm::core;
namespace donor = libtorrent;

unsigned compare_libtorrent_picker();

namespace {

bool same(const core::sha256& actual, const donor::sha256_hash& expected) {
   return std::memcmp(actual.data(), expected.data(), 32U) == 0;
}

void compare(std::span<const std::uint8_t> bytes, std::uint32_t piece_size) {
   const auto actual = core::hash_file(bytes, piece_size);
   if (bytes.empty()) {
      if (actual.root != core::sha256{} || !actual.pieces.empty()) {
         throw std::runtime_error{"empty-file sentinel differs"};
      }
      return;
   }
   const auto count = static_cast<int>(bytes.size() / core::block_size + (bytes.size() % core::block_size != 0U));
   const auto leaves = donor::merkle_num_leafs(count);
   auto tree = std::vector<donor::sha256_hash>(donor::merkle_num_nodes(leaves));
   const auto first = donor::merkle_first_leaf(leaves);
   for (auto i = 0; i < count; ++i) {
      const auto offset = static_cast<std::size_t>(i) * core::block_size;
      const auto length = std::min<std::size_t>(core::block_size, bytes.size() - offset);
      tree[first + i] = donor::hasher256{reinterpret_cast<const char*>(bytes.data() + offset),
                                       static_cast<int>(length)}.final();
   }
   donor::merkle_fill_tree(tree, leaves);
   if (!same(actual.root, tree[0])) {
      throw std::runtime_error{"file root differs from pinned libtorrent"};
   }
   const auto pieces = core::piece_count(bytes.size(), piece_size);
   const auto piece_level = bytes.size() > piece_size ?
      donor::merkle_num_layers(leaves) - static_cast<int>(std::countr_zero(piece_size / core::block_size)) : 0;
   if (actual.pieces.size() != (bytes.size() > piece_size ? pieces : 0U)) {
      throw std::runtime_error{"piece layer shape differs"};
   }
   for (auto i = 0U; i < pieces; ++i) {
      auto node = donor::merkle_to_flat_index(piece_level, static_cast<int>(i));
      if (bytes.size() > piece_size && !same(actual.pieces[i], tree[node])) {
         throw std::runtime_error{"piece layer differs from pinned libtorrent"};
      }
      const auto proof = core::make_piece_proof(actual.pieces, bytes.size(), piece_size, i);
      auto depth = std::size_t{};
      while (node != 0) {
         if (depth >= proof.siblings.size() || !same(proof.siblings[depth], tree[donor::merkle_get_sibling(node)])) {
            throw std::runtime_error{"proof differs from pinned libtorrent tree"};
         }
         node = donor::merkle_get_parent(node);
         ++depth;
      }
      if (depth != proof.siblings.size()) {
         throw std::runtime_error{"proof contains extra siblings"};
      }
      const auto offset = static_cast<std::size_t>(i) * piece_size;
      const auto data = bytes.subspan(offset, std::min<std::size_t>(piece_size, bytes.size() - offset));
      if (!core::verify_piece(data, bytes.size(), piece_size, i, proof, actual.root)) {
         throw std::runtime_error{"donor-matching piece was rejected"};
      }
   }
}

} // namespace

int main() {
   try {
      auto cases = 0U;
      for (auto size : {0U, 1U, core::block_size - 1U, core::block_size, core::block_size + 1U,
                       2U * core::block_size - 1U, 2U * core::block_size, 2U * core::block_size + 1U,
                       3U * core::block_size, 4U * core::block_size + 1U, 9U * core::block_size + 15U}) {
         auto bytes = std::vector<std::uint8_t>(size);
         for (auto i = std::size_t{}; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>((i * 31U + 17U) & 0xffU);
         }
         for (auto scale : {1U, 2U, 4U, 8U, 16U}) {
            compare(bytes, core::block_size * scale);
            ++cases;
         }
      }
      std::cout << "PASS: " << cases << " file/piece geometries against unchanged libtorrent b4e9e2471e7a\n";
      const auto picker_cases = compare_libtorrent_picker();
      std::cout << "PASS: " << picker_cases
                << " picker decisions against unchanged libtorrent b4e9e2471e7a\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
   }
}
