module;

#include <boost/describe.hpp>
#include <compare>
#include <cstdint>
#include <span>
#include <vector>

export module forge.swarm.core.checkpoint;

export import forge.swarm.core.identity;

export namespace forge::swarm::core {

inline constexpr std::uint32_t max_checkpoint_size = 1024U * 1024U;
inline constexpr std::uint32_t max_unfinished_pieces = 64U;

struct piece_id {
   std::uint32_t file = 0;
   std::uint32_t index = 0;
   auto operator<=>(const piece_id&) const = default;
};

struct unfinished_piece {
   piece_id piece;
   std::vector<std::uint8_t> blocks;
   bool operator==(const unfinished_piece&) const = default;
};

struct resume_state {
   std::uint32_t version = 1;
   swarm_id id;
   std::uint64_t generation = 0;
   bool published = false;
   std::vector<std::uint8_t> verified;
   std::vector<unfinished_piece> unfinished;
   bool operator==(const resume_state&) const = default;
};

// Flattened verified-bitmap position in descriptor file order, never a byte offset.
[[nodiscard]] std::uint32_t piece_ordinal(const descriptor&, piece_id);
[[nodiscard]] std::uint32_t piece_length(const descriptor&, piece_id);
[[nodiscard]] bool bitmap_test(std::span<const std::uint8_t>, std::uint32_t index) noexcept;
void validate_checkpoint(const resume_state&, const descriptor&);
[[nodiscard]] std::vector<std::uint8_t> encode_checkpoint(const resume_state&, const descriptor&);
[[nodiscard]] resume_state decode_checkpoint(std::span<const std::uint8_t>, const descriptor&);

BOOST_DESCRIBE_STRUCT(piece_id, (), (file, index))
BOOST_DESCRIBE_STRUCT(unfinished_piece, (), (piece, blocks))
BOOST_DESCRIBE_STRUCT(resume_state, (), (version, id, generation, published, verified, unfinished))

} // namespace forge::swarm::core
