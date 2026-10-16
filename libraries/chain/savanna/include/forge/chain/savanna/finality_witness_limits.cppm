module;

#include <boost/describe.hpp>

#include <cstdint>

export module forge.chain.savanna.finality_witness_limits;

export namespace forge::chain::savanna {

inline constexpr auto finality_witness_hard_max_blocks = std::uint32_t{4'096};
inline constexpr auto finality_witness_hard_max_producer_slots = std::uint32_t{65'536};
inline constexpr auto finality_witness_hard_max_bytes = std::uint32_t{8U << 20U};

struct finality_witness_limits {
   // Also bound the retained roots and canonical bytes of each trusted checkpoint.
   std::uint32_t max_blocks = finality_witness_hard_max_blocks;
   std::uint32_t max_producer_slots = finality_witness_hard_max_producer_slots;
   std::uint32_t max_bytes = finality_witness_hard_max_bytes;
};

BOOST_DESCRIBE_STRUCT(finality_witness_limits, (), (max_blocks, max_producer_slots, max_bytes))

} // namespace forge::chain::savanna
