#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <variant>

import forge.chain.savanna.finality_witness;
import forge.chain.savanna.checkpoint;
import forge.chain.savanna.genesis;
import forge.chain.savanna.finality_core;
import forge.chain.savanna.finalizer_safety;
import forge.chain.savanna.policy;
import forge.chain.savanna.policy_state;
import forge.chain.savanna.rank;
import forge.chain.savanna.validation;
import forge.chain.savanna.vote;
import forge.chain.savanna.vote_accumulator;

int main() {
   using namespace forge::chain;
   static_assert(
       std::same_as<savanna::finality_trust,
                    std::variant<savanna::finality_genesis_bootstrap, savanna::finality_checkpoint_bootstrap>>);
   auto id = core::digest{};
   id._hash[0] = 1U;
   const auto seed = std::array<std::uint8_t, 32>{1U};
   const auto key = forge::crypto::bls::private_key{std::span<const std::uint8_t>{seed}};

   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 1U,
       .finalizers =
           {
               {
                   .description = "package",
                   .weight = 1U,
                   .public_key = key.get_public_key(),
               },
           },
   };
   const auto verified = savanna::validate(policy, std::array{key.proof_of_possession()});

   const auto finality = savanna::finality_core::genesis(1U, 1U);
   const auto checkpoint_start = savanna::validation_start(
       savanna::header_state{.id = protocol::calculate_block_id(protocol::block_header{}), .finality = finality});
   const auto block = savanna::block_ref{
       .num = 1U,
       .id = id,
       .slot = 1U,
       .active_policy_generation = 1U,
   };
   const auto rank = savanna::make_rank(finality, block);
   auto validation = savanna::make_validation({
       .num = 1U,
       .slot = 1U,
       .finality_digest = id,
       .commitment = id,
   });
   auto votes = savanna::vote_accumulator{block, verified};
   const auto safety = savanna::make_finalizer_safety(block);
   const auto message = savanna::message_for_vote(id, savanna::vote_kind::strong);
   const auto chain = savanna::calculate_chain_id(savanna::genesis{});
   return checkpoint_start == 1U && verified.get().threshold == 1U && rank.block == 1U &&
                  validation.retained_size() == 1U && !votes.status().quorum_reached() && safety.lock().id == id &&
                  message.size() == 32U && !chain.empty()
              ? 0
              : 1;
}
