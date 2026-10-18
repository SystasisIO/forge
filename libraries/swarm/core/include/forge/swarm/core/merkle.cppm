module;

#include <boost/describe.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

export module forge.swarm.core.merkle;

export import forge.crypto.digest.sha256;
export import forge.swarm.core.exceptions;

export namespace forge::swarm::core {

using forge::crypto::digest::sha256;

inline constexpr std::uint32_t block_size = 16U * 1024U;
inline constexpr std::uint32_t max_piece_size = 16U * 1024U * 1024U;
inline constexpr std::uint64_t max_file_size = std::uint64_t{1} << 40U;
inline constexpr std::uint32_t max_piece_count = 262144U;

struct file_hashes {
   sha256 root;
   std::vector<sha256> pieces;

   bool operator==(const file_hashes&) const = default;
};

struct piece_proof {
   // Bottom-up siblings; orientation is derived exclusively from the file-local index.
   std::vector<sha256> siblings;

   bool operator==(const piece_proof&) const = default;
};

void validate_piece_size(std::uint32_t piece_size);
[[nodiscard]] std::uint32_t piece_count(std::uint64_t size, std::uint32_t piece_size);

// The layer exists only for size > piece_size. Short files use their natural block-tree root.
[[nodiscard]] sha256 root_from_piece_layer(std::span<const sha256> layer, std::uint64_t size,
                                           std::uint32_t piece_size);
[[nodiscard]] sha256 hash_piece(std::span<const std::uint8_t> bytes, std::uint64_t size,
                               std::uint32_t piece_size, std::uint32_t index);
[[nodiscard]] piece_proof make_piece_proof(std::span<const sha256> layer, std::uint64_t size,
                                          std::uint32_t piece_size, std::uint32_t index);
[[nodiscard]] bool verify_piece_hash(const sha256& hash, std::uint64_t size, std::uint32_t piece_size,
                                    std::uint32_t index, const piece_proof& proof, const sha256& root);
[[nodiscard]] bool verify_piece(std::span<const std::uint8_t> bytes, std::uint64_t size,
                               std::uint32_t piece_size, std::uint32_t index,
                               const piece_proof& proof, const sha256& root);

// Streaming value: accepts consecutive complete 16 KiB blocks, then the exact final short block.
// Retains hashes only, never file bytes. A failed append leaves the value unchanged.
class file_hasher {
 public:
   explicit file_hasher(std::uint64_t size, std::uint32_t piece_size);
   void append_block(std::span<const std::uint8_t> bytes);
   [[nodiscard]] file_hashes finish();

 private:
   std::array<sha256, 11> _frontier{};
   std::vector<sha256> _pieces;
   std::uint64_t _size = 0;
   std::uint64_t _received = 0;
   std::uint32_t _piece_size = 0;
   std::uint32_t _piece_blocks = 0;
   bool _finished = false;
};

[[nodiscard]] file_hashes hash_file(std::span<const std::uint8_t> bytes, std::uint32_t piece_size);

BOOST_DESCRIBE_STRUCT(file_hashes, (), (root, pieces))
BOOST_DESCRIBE_STRUCT(piece_proof, (), (siblings))

} // namespace forge::swarm::core
