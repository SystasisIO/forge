module;

#include <boost/describe.hpp>

#include <cstdint>
#include <optional>
#include <vector>

export module forge.chain.savanna.extensions;

export import forge.chain.savanna.policy_state;
export import forge.chain.savanna.qc;

export namespace forge::chain::savanna {

inline constexpr std::uint16_t protocol_feature_extension_id = 0;
inline constexpr std::uint16_t additional_signatures_extension_id = 2;
inline constexpr std::uint16_t finality_extension_id = 2;
inline constexpr std::uint16_t quorum_certificate_extension_id = 3;
inline constexpr std::uint16_t finalizer_proof_extension_id = 4;
inline constexpr std::uint16_t state_commitment_extension_id = 5;
inline constexpr std::uint32_t state_commitment_version = 4;
inline constexpr std::uint32_t proper_savanna_schedule_version = std::uint32_t{1} << 31U;

struct protocol_feature_extension {
   std::vector<digest> features;
};

struct finality_extension {
   forge::chain::savanna::qc_claim claim;
   std::optional<forge::chain::savanna::finalizer_policy_diff> finalizers;
   std::optional<proposer_policy_diff> proposers;
};

struct finalizer_proof_extension {
   std::uint32_t generation = 0;
   std::vector<forge::crypto::bls::signature> inserted_proofs;
};

struct state_commitment {
   std::uint32_t version = state_commitment_version;
   digest state_root;
   std::uint64_t state_size = 0;
   digest change_root;
   std::uint64_t change_count = 0;
};

struct additional_signatures_extension {
   std::vector<forge::chain::protocol::signature> signatures;
};

struct quorum_certificate_extension {
   forge::chain::savanna::quorum_certificate certificate;
};

struct header_extensions {
   std::vector<digest> protocol_features;
   finality_extension finality;
   std::optional<finalizer_proof_extension> finalizer_proofs;
   state_commitment commitment;
};

struct block_extensions {
   std::vector<forge::chain::protocol::signature> additional_signatures;
   std::optional<forge::chain::savanna::quorum_certificate> certificate;
};

[[nodiscard]] header_extensions decode_header_extensions(const forge::chain::protocol::extensions& extensions);
[[nodiscard]] block_extensions decode_block_extensions(const forge::chain::protocol::extensions& extensions);

BOOST_DESCRIBE_STRUCT(protocol_feature_extension, (), (features))
BOOST_DESCRIBE_STRUCT(finality_extension, (), (claim, finalizers, proposers))
BOOST_DESCRIBE_STRUCT(finalizer_proof_extension, (), (generation, inserted_proofs))
BOOST_DESCRIBE_STRUCT(state_commitment, (), (version, state_root, state_size, change_root, change_count))
BOOST_DESCRIBE_STRUCT(additional_signatures_extension, (), (signatures))
BOOST_DESCRIBE_STRUCT(quorum_certificate_extension, (), (certificate))

} // namespace forge::chain::savanna
