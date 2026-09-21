module;

#include <boost/describe.hpp>
#include <forge/exceptions/macros.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

export module forge.chain.savanna.finality_witness;

export import forge.chain.protocol.audit;
export import forge.chain.protocol.block;
export import forge.chain.savanna.admission;
export import forge.chain.savanna.checkpoint;

export namespace forge::chain::savanna {

inline constexpr auto finality_witness_scheme = std::string_view{"spine.savanna.finality"};
inline constexpr auto finality_witness_version = std::uint32_t{1};
inline constexpr auto finality_witness_hard_max_blocks = std::uint32_t{4'096};
inline constexpr auto finality_witness_hard_max_producer_slots = std::uint32_t{65'536};
inline constexpr auto finality_witness_hard_max_bytes = std::uint32_t{8U << 20U};

struct finality_witness_limits {
   std::uint32_t max_blocks = finality_witness_hard_max_blocks;
   std::uint32_t max_producer_slots = finality_witness_hard_max_producer_slots;
   std::uint32_t max_bytes = finality_witness_hard_max_bytes;
};

struct finality_witness_record {
   forge::chain::protocol::signed_block_header header;
   forge::chain::protocol::extensions block_extensions;
   digest action_receipt_root;
};

struct finality_witness {
   forge::chain::protocol::chain_id chain;
   block_id trusted_bootstrap;
   std::vector<finality_witness_record> records;
};

struct finality_genesis_bootstrap {
   genesis configuration;
   state_commitment commitment;
};

struct finality_checkpoint_bootstrap {
   forge::chain::protocol::chain_id chain;
   checkpoint value;
};

using finality_trust = std::variant<finality_genesis_bootstrap, finality_checkpoint_bootstrap>;

struct finality_trust_anchor {
   forge::chain::protocol::chain_id chain;
   block_id block;

   bool operator==(const finality_trust_anchor&) const = default;
};

struct producer_opportunity {
   block_timestamp timestamp;
   forge::chain::protocol::account_name expected_producer;
   std::optional<block_id> produced_block;
   std::optional<block_num> produced_block_num;

   bool operator==(const producer_opportunity&) const = default;
};

struct finality_replay {
   std::vector<forge::chain::protocol::state_anchor> anchors;
   std::vector<producer_opportunity> producer_opportunities;
   block_num finalized_block_num = 0;
   block_num validated_block_num = 0;
};

struct finality_trust_advance {
   finality_checkpoint_bootstrap checkpoint;
   finality_replay replay;
};

[[nodiscard]] finality_witness make_finality_witness(forge::chain::protocol::chain_id chain, block_id trusted_bootstrap,
                                                     std::span<const finality_witness_record> records,
                                                     finality_witness_limits limits = {});

[[nodiscard]] forge::chain::protocol::proof_blob encode_finality_witness(const finality_witness& witness,
                                                                         finality_witness_limits limits = {});

[[nodiscard]] finality_witness decode_finality_witness(const forge::chain::protocol::proof_blob& proof,
                                                       finality_witness_limits limits = {});

[[nodiscard]] finality_replay replay_finality_witness(const finality_trust& trust, const finality_witness& witness,
                                                      finality_witness_limits limits = {});

[[nodiscard]] header_state replay_finality_witness_state(const finality_trust& trust, const finality_witness& witness,
                                                         const forge::chain::protocol::state_anchor& expected,
                                                         finality_witness_limits limits = {});

[[nodiscard]] finality_checkpoint_bootstrap
advance_finality_trust(const finality_trust& trust, const finality_witness& witness,
                       const forge::chain::protocol::state_anchor& finalized, finality_witness_limits limits = {});

[[nodiscard]] finality_trust_advance
advance_finality_trust_with_replay(const finality_trust& trust, const finality_witness& witness,
                                   const forge::chain::protocol::state_anchor& finalized,
                                   finality_witness_limits limits = {});

[[nodiscard]] finality_checkpoint_bootstrap
advance_finality_trust(const finality_trust& trust, const forge::chain::protocol::proof_blob& proof,
                       const forge::chain::protocol::state_anchor& finalized, finality_witness_limits limits = {});

[[nodiscard]] finality_trust_anchor trust_anchor(const finality_trust& trust);

void verify_finality_witness(const finality_trust& trust, const forge::chain::protocol::proof_blob& proof,
                             const forge::chain::protocol::state_anchor& expected, finality_witness_limits limits = {});

void verify_finality_ancestry_witness(const finality_trust& trust, const forge::chain::protocol::proof_blob& proof,
                                      const forge::chain::protocol::state_anchor& finalized,
                                      std::span<const forge::chain::protocol::state_anchor> intermediate,
                                      finality_witness_limits limits = {});

BOOST_DESCRIBE_STRUCT(finality_witness_limits, (), (max_blocks, max_producer_slots, max_bytes))
BOOST_DESCRIBE_STRUCT(finality_witness_record, (), (header, block_extensions, action_receipt_root))
BOOST_DESCRIBE_STRUCT(finality_witness, (), (chain, trusted_bootstrap, records))
BOOST_DESCRIBE_STRUCT(finality_genesis_bootstrap, (), (configuration, commitment))
BOOST_DESCRIBE_STRUCT(finality_checkpoint_bootstrap, (), (chain, value))
BOOST_DESCRIBE_STRUCT(finality_trust_anchor, (), (chain, block))
BOOST_DESCRIBE_STRUCT(producer_opportunity, (), (timestamp, expected_producer, produced_block, produced_block_num))
BOOST_DESCRIBE_STRUCT(finality_replay, (), (anchors, producer_opportunities, finalized_block_num, validated_block_num))
BOOST_DESCRIBE_STRUCT(finality_trust_advance, (), (checkpoint, replay))

} // namespace forge::chain::savanna

export namespace forge::chain::savanna::exceptions {

enum class finality_witness_code : std::uint16_t {
   invalid_witness = 1,
   limit_exceeded = 2,
   wrong_chain = 3,
   untrusted_bootstrap = 4,
   anchor_mismatch = 5,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(finality_witness_code, "forge.chain.savanna.finality_witness")

using invalid_finality_witness =
    forge::exceptions::coded_exception<finality_witness_code, finality_witness_code::invalid_witness>;
using finality_witness_limit_exceeded =
    forge::exceptions::coded_exception<finality_witness_code, finality_witness_code::limit_exceeded>;
using finality_witness_wrong_chain =
    forge::exceptions::coded_exception<finality_witness_code, finality_witness_code::wrong_chain>;
using untrusted_finality_bootstrap =
    forge::exceptions::coded_exception<finality_witness_code, finality_witness_code::untrusted_bootstrap>;
using finality_anchor_mismatch =
    forge::exceptions::coded_exception<finality_witness_code, finality_witness_code::anchor_mismatch>;

} // namespace forge::chain::savanna::exceptions
