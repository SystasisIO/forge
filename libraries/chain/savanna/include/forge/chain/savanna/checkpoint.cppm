module;

#include <boost/describe.hpp>

#include <optional>

export module forge.chain.savanna.checkpoint;

export import forge.chain.savanna.header_state;
export import forge.chain.savanna.validation;

export namespace forge::chain::savanna {

struct checkpoint {
   forge::chain::savanna::block_ref finalized;
   header_state state;
   forge::chain::savanna::validation_state validation;
};

// The finalized anchor stays unchanged; validation must retain every root
// which the anchor's next admissible QC claim may reference.
[[nodiscard]] block_num validation_start(const header_state& state);
[[nodiscard]] checkpoint make_checkpoint(header_state state, validation_state validation);
void validate(const checkpoint& value);

// Both inputs must be complete, valid checkpoints. Different retention prefixes
// are equivalent only when all state and canonical retained history agree.
[[nodiscard]] bool equivalent(const checkpoint& left, const checkpoint& right);

BOOST_DESCRIBE_STRUCT(checkpoint, (), (finalized, state, validation))

} // namespace forge::chain::savanna
