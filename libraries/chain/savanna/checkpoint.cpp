module;

#include <forge/exceptions/macros.hpp>

#include <utility>
#include <vector>

module forge.chain.savanna.checkpoint;

import forge.raw.raw;

namespace forge::chain::savanna {

block_num validation_start(const header_state& state) {
   validate(state.finality);
   const auto current = state.num();
   if (state.finality.current_block_num() != current) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality_state,
                            "Savanna checkpoint finality head does not match its header");
   }
   const auto claim = state.finality.latest_qc_claim().block;
   // Genesis claims use an empty action root. The next possible non-genesis
   // claim still needs its root, even if it is older than the checkpoint.
   return state.finality.is_genesis_block_num(claim) && claim < current ? claim + 1U : claim;
}

void validate(const checkpoint& value) {
   if (value.finalized.empty() || value.state.id != value.finalized.id || value.state.num() != value.finalized.num ||
       forge::chain::protocol::calculate_block_id(value.state.header) != value.state.id ||
       value.state.block != value.finalized || value.state.make_block_ref() != value.finalized) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_header, "Savanna checkpoint identity is inconsistent");
   }
   const auto first = validation_start(value.state);
   validate(value.validation);
   if (value.validation.empty() || value.validation.first_block_num() > first ||
       value.validation.current_block_num() != value.finalized.num) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_validation_state,
                            "Savanna checkpoint is missing required validation history; recovery is required");
   }
   static_cast<void>(validate(value.state.active_finalizers));
   for (const auto& [block, policy] : value.state.proposed_finalizers) {
      static_cast<void>(block);
      static_cast<void>(validate(policy));
   }
   if (value.state.pending_finalizers) {
      static_cast<void>(validate(value.state.pending_finalizers->second));
   }
   if (value.state.latest_qc_finalizers) {
      static_cast<void>(validate(*value.state.latest_qc_finalizers));
   }
   static_cast<void>(decode_header_extensions(value.state.header.header_extensions));
}

checkpoint make_checkpoint(header_state state, validation_state validation) {
   const auto first = validation_start(state);
   auto value = checkpoint{
       .finalized = state.make_block_ref(),
       .state = std::move(state),
       .validation = std::move(validation),
   };
   validate(value);
   value.validation = advance_finalized(std::move(value.validation), first);
   return value;
}

bool equivalent(const checkpoint& left, const checkpoint& right) {
   validate(left);
   validate(right);
   if (left.finalized != right.finalized || forge::raw::pack(left.state) != forge::raw::pack(right.state)) {
      return false;
   }
   const auto first = validation_start(left.state);
   return forge::raw::pack(advance_finalized(left.validation, first)) ==
          forge::raw::pack(advance_finalized(right.validation, first));
}

} // namespace forge::chain::savanna
