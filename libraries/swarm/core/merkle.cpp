module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

module forge.swarm.core.merkle;

namespace forge::swarm::core {
namespace {

bool valid_piece_size(std::uint32_t size) {
   return size >= block_size && size <= max_piece_size && std::has_single_bit(size);
}

bool valid_geometry(std::uint64_t size, std::uint32_t piece_size) {
   return valid_piece_size(piece_size) && size <= max_file_size &&
          size / piece_size + (size % piece_size != 0U) <= max_piece_count;
}

std::uint32_t tree_height(std::uint32_t count) {
   return count <= 1U ? 0U : std::bit_width(count - 1U);
}

sha256 hash_pair(const sha256& left, const sha256& right) {
   auto encoder = sha256::encoder{};
   encoder.write(left.to_uint8_span());
   encoder.write(right.to_uint8_span());
   return encoder.result();
}

std::array<sha256, 29> empty_hashes(std::uint32_t height) {
   auto result = std::array<sha256, 29>{};
   for (auto rank = 1U; rank <= height; ++rank) {
      result[rank] = hash_pair(result[rank - 1U], result[rank - 1U]);
   }
   return result;
}

sha256 subtree_root(std::span<const sha256> leaves, std::uint32_t start, std::uint32_t height,
                    std::uint32_t base_height, const std::array<sha256, 29>& zeros) {
   if (start >= leaves.size()) {
      return zeros[base_height + height];
   }
   if (height == 0U) {
      return leaves[start];
   }
   const auto half = std::uint32_t{1} << (height - 1U);
   return hash_pair(subtree_root(leaves, start, height - 1U, base_height, zeros),
                    subtree_root(leaves, start + half, height - 1U, base_height, zeros));
}

void append_leaf(std::array<sha256, 11>& frontier, std::uint32_t count, sha256 hash) {
   auto height = 0U;
   while ((count & (std::uint32_t{1} << height)) != 0U) {
      hash = hash_pair(frontier[height], hash);
      ++height;
   }
   frontier[height] = hash;
}

sha256 padded_frontier_root(std::array<sha256, 11> frontier, std::uint32_t count,
                           std::uint32_t height) {
   const auto zeros = empty_hashes(height);
   const auto target = std::uint32_t{1} << height;
   // Append whole empty subtrees, not zero bytes and not a zero digest at an upper layer.
   while (count < target) {
      auto rank = static_cast<std::uint32_t>(std::countr_zero(count));
      const auto added = std::uint32_t{1} << rank;
      auto hash = zeros[rank];
      while ((count & (std::uint32_t{1} << rank)) != 0U) {
         hash = hash_pair(frontier[rank], hash);
         ++rank;
      }
      frontier[rank] = hash;
      count += added;
   }
   return frontier[height];
}

std::uint32_t expected_piece_bytes(std::uint64_t size, std::uint32_t piece_size, std::uint32_t index) {
   // Called only after bounded geometry and index checks; the product cannot overflow uint64.
   const auto offset = std::uint64_t{index} * piece_size;
   return static_cast<std::uint32_t>(std::min<std::uint64_t>(piece_size, size - offset));
}

void validate_layer(std::span<const sha256> layer, std::uint64_t size, std::uint32_t piece_size) {
   const auto count = piece_count(size, piece_size);
   const auto expected = size > piece_size ? count : 0U;
   if (layer.size() != expected) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_geometry, "piece layer has the wrong hash count");
   }
}

} // namespace

void validate_piece_size(std::uint32_t piece_size) {
   if (!valid_piece_size(piece_size)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_geometry, "piece size must be a power of two from 16 KiB to 16 MiB");
   }
}

std::uint32_t piece_count(std::uint64_t size, std::uint32_t piece_size) {
   validate_piece_size(piece_size);
   if (size > max_file_size) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "file size exceeds the Swarm v1 limit");
   }
   const auto count = size / piece_size + (size % piece_size != 0U);
   if (count > max_piece_count) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "file piece count exceeds the Swarm v1 limit");
   }
   return static_cast<std::uint32_t>(count);
}

sha256 root_from_piece_layer(std::span<const sha256> layer, std::uint64_t size, std::uint32_t piece_size) {
   validate_layer(layer, size, piece_size);
   if (size == 0U) {
      return {};
   }
   if (size <= piece_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_geometry, "a short file root cannot be reconstructed from an empty layer");
   }
   const auto height = tree_height(static_cast<std::uint32_t>(layer.size()));
   const auto base_height = tree_height(piece_size / block_size);
   const auto zeros = empty_hashes(base_height + height);
   return subtree_root(layer, 0U, height, base_height, zeros);
}

sha256 hash_piece(std::span<const std::uint8_t> bytes, std::uint64_t size,
                  std::uint32_t piece_size, std::uint32_t index) {
   const auto count = piece_count(size, piece_size);
   if (index >= count || bytes.size() != expected_piece_bytes(size, piece_size, index)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_content, "piece index or byte length does not match file geometry");
   }
   auto frontier = std::array<sha256, 11>{};
   auto blocks = 0U;
   for (auto offset = std::size_t{0}; offset < bytes.size();) {
      const auto length = std::min<std::size_t>(block_size, bytes.size() - offset);
      append_leaf(frontier, blocks, sha256::hash(bytes.subspan(offset, length)));
      ++blocks;
      offset += length;
   }
   const auto height = tree_height(size <= piece_size ? blocks : piece_size / block_size);
   return padded_frontier_root(frontier, blocks, height);
}

piece_proof make_piece_proof(std::span<const sha256> layer, std::uint64_t size,
                             std::uint32_t piece_size, std::uint32_t index) {
   validate_layer(layer, size, piece_size);
   const auto count = piece_count(size, piece_size);
   if (index >= count) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_geometry, "piece proof index is outside the file");
   }
   auto result = piece_proof{};
   const auto height = tree_height(count);
   const auto base_height = tree_height(piece_size / block_size);
   const auto zeros = empty_hashes(base_height + height);
   result.siblings.reserve(height);
   for (auto rank = 0U; rank < height; ++rank) {
      const auto sibling_start = ((index >> rank) ^ 1U) << rank;
      result.siblings.push_back(subtree_root(layer, sibling_start, rank, base_height, zeros));
   }
   return result;
}

bool verify_piece_hash(const sha256& hash, std::uint64_t size, std::uint32_t piece_size,
                       std::uint32_t index, const piece_proof& proof, const sha256& root) {
   if (!valid_geometry(size, piece_size) || size == 0U) {
      return false;
   }
   const auto count = static_cast<std::uint32_t>(size / piece_size + (size % piece_size != 0U));
   const auto height = tree_height(count);
   if (index >= count || proof.siblings.size() != height) {
      return false;
   }
   const auto base_height = tree_height(piece_size / block_size);
   const auto zeros = empty_hashes(base_height + height);
   auto current = hash;
   for (auto rank = 0U; rank < height; ++rank) {
      const auto& sibling = proof.siblings[rank];
      const auto sibling_start = ((index >> rank) ^ 1U) << rank;
      if (sibling_start >= count && sibling != zeros[base_height + rank]) {
         return false;
      }
      current = ((index >> rank) & 1U) != 0U ? hash_pair(sibling, current) : hash_pair(current, sibling);
   }
   return current == root;
}

bool verify_piece(std::span<const std::uint8_t> bytes, std::uint64_t size,
                  std::uint32_t piece_size, std::uint32_t index,
                  const piece_proof& proof, const sha256& root) {
   if (!valid_geometry(size, piece_size) || size == 0U) {
      return false;
   }
   const auto count = static_cast<std::uint32_t>(size / piece_size + (size % piece_size != 0U));
   if (index >= count || proof.siblings.size() != tree_height(count) ||
       bytes.size() != expected_piece_bytes(size, piece_size, index)) {
      return false;
   }
   return verify_piece_hash(hash_piece(bytes, size, piece_size, index), size, piece_size, index, proof, root);
}

file_hasher::file_hasher(std::uint64_t size, std::uint32_t piece_size)
    : _size(size), _piece_size(piece_size) {
   _pieces.reserve(piece_count(size, piece_size));
}

void file_hasher::append_block(std::span<const std::uint8_t> bytes) {
   if (_finished || _received == _size ||
       bytes.size() != std::min<std::uint64_t>(block_size, _size - _received)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_content, "file hasher requires the next complete block or final short block");
   }
   auto frontier = _frontier;
   append_leaf(frontier, _piece_blocks, sha256::hash(bytes));
   const auto blocks = _piece_blocks + 1U;
   if (blocks == _piece_size / block_size) {
      _pieces.push_back(frontier[tree_height(blocks)]);
      _piece_blocks = 0U;
   } else {
      _piece_blocks = blocks;
   }
   _frontier = frontier;
   _received += bytes.size();
}

file_hashes file_hasher::finish() {
   if (_finished || _received != _size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_content, "file hasher is incomplete or already finalized");
   }
   if (_size == 0U) {
      _finished = true;
      return {};
   }
   if (_size <= _piece_size) {
      const auto root = _piece_blocks == 0U ? _pieces.front()
          : padded_frontier_root(_frontier, _piece_blocks, tree_height(_piece_blocks));
      _pieces.clear();
      _finished = true;
      return {.root = root, .pieces = {}};
   }
   if (_piece_blocks != 0U) {
      _pieces.push_back(padded_frontier_root(_frontier, _piece_blocks, tree_height(_piece_size / block_size)));
   }
   auto root = sha256{};
   try {
      root = root_from_piece_layer(_pieces, _size, _piece_size);
   } catch (...) {
      if (_piece_blocks != 0U) {
         _pieces.pop_back();
      }
      throw;
   }
   _finished = true;
   return {.root = root, .pieces = std::move(_pieces)};
}

file_hashes hash_file(std::span<const std::uint8_t> bytes, std::uint32_t piece_size) {
   auto hasher = file_hasher{bytes.size(), piece_size};
   for (auto offset = std::size_t{0}; offset < bytes.size();) {
      const auto length = std::min<std::size_t>(block_size, bytes.size() - offset);
      hasher.append_block(bytes.subspan(offset, length));
      offset += length;
   }
   return hasher.finish();
}

} // namespace forge::swarm::core
