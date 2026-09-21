module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

module forge.chain.api.savanna_finality_verifier;

import forge.chain.api.exceptions;
import forge.raw.raw;

#include "details/savanna_finality_trust_store.hxx"

namespace forge::chain::api::detail {
namespace {

protocol::block_num operational_block_num(const savanna::finality_trust& trust) {
   const auto* checkpoint = std::get_if<savanna::finality_checkpoint_bootstrap>(&trust);
   return checkpoint ? checkpoint->value.finalized.num : protocol::block_num{1U};
}

} // namespace

savanna_finality_trust_store::savanna_finality_trust_store(savanna::finality_trust trust,
                                                           std::vector<savanna::finality_trust> additional_trusts,
                                                           savanna::finality_witness_limits value_limits)
    : limits_{value_limits} {
   configured_roots_.reserve(additional_trusts.size() + 1U);
   add_configured_root(std::move(trust));
   for (auto& additional : additional_trusts) {
      add_configured_root(std::move(additional));
   }
}

savanna::finality_trust savanna_finality_trust_store::preferred_trust() const {
   const auto lock = std::lock_guard{mutex_};
   return *preferred_entry_locked().trust;
}

std::optional<protocol::block_id> savanna_finality_trust_store::preferred_trust_anchor() const {
   const auto lock = std::lock_guard{mutex_};
   return preferred_entry_locked().position.block;
}

std::optional<protocol::block_id>
savanna_finality_trust_store::trust_anchor_at_or_before(protocol::block_num target) const {
   const auto lock = std::lock_guard{mutex_};
   const auto prefer = [&](const trusted_entry* current, const trusted_entry& candidate) {
      if (candidate.position.finalized_block_num > target) {
         return current;
      }
      return !current || candidate.position.finalized_block_num > current->position.finalized_block_num ? &candidate
                                                                                                        : current;
   };

   const auto* selected = static_cast<const trusted_entry*>(nullptr);
   for (const auto& configured : configured_roots_) {
      selected = prefer(selected, configured);
   }
   for (const auto& rolling : rolling_checkpoints_) {
      selected = prefer(selected, rolling);
   }
   return selected ? std::optional{selected->position.block} : std::nullopt;
}

protocol::chain_id savanna_finality_trust_store::trusted_chain() const {
   return chain_;
}

savanna::finality_witness_limits savanna_finality_trust_store::witness_limits() const {
   return limits_;
}

std::shared_ptr<const savanna::header_state>
savanna_finality_trust_store::cached_replay_state(const protocol::state_anchor& anchor,
                                                  const protocol::digest& proof_digest) const {
   const auto lock = std::lock_guard{mutex_};
   const auto found = std::ranges::find_if(
       replay_states_, [&](const auto& entry) { return entry.anchor == anchor && entry.proof_digest == proof_digest; });
   return found == replay_states_.end() ? nullptr : found->state;
}

std::shared_ptr<const savanna::finality_replay>
savanna_finality_trust_store::cached_replay(const protocol::state_anchor& anchor,
                                            const protocol::digest& proof_digest) const {
   const auto lock = std::lock_guard{mutex_};
   const auto found = std::ranges::find_if(
       replay_states_, [&](const auto& entry) { return entry.anchor == anchor && entry.proof_digest == proof_digest; });
   return found == replay_states_.end() ? nullptr : found->replay;
}

savanna_finality_trust_store::snapshot
savanna_finality_trust_store::take_snapshot(const protocol::state_anchor& expected,
                                            const savanna::finality_witness& witness) const {
   const auto lock = std::lock_guard{mutex_};
   if (expected.chain != chain_ || witness.chain != chain_) {
      FORGE_THROW_EXCEPTION(exceptions::wrong_chain, "Savanna finality witness belongs to another chain");
   }

   const auto matches = [&](const trusted_entry& entry) { return entry.position.block == witness.trusted_bootstrap; };
   const auto configured = std::ranges::find_if(configured_roots_, matches);
   if (configured != configured_roots_.end()) {
      return {.source_trust = configured->trust, .preferred = preferred_entry_locked().position};
   }
   const auto rolling = std::ranges::find_if(rolling_checkpoints_, matches);
   if (rolling != rolling_checkpoints_.end()) {
      return {.source_trust = rolling->trust, .preferred = preferred_entry_locked().position};
   }
   FORGE_THROW_EXCEPTION(exceptions::trust_required,
                         "Savanna finality witness does not match a configured or rolling checkpoint");
}

void savanna_finality_trust_store::install_verified(savanna::finality_checkpoint_bootstrap checkpoint,
                                                    savanna::finality_replay replay,
                                                    const protocol::state_anchor& anchor,
                                                    const protocol::digest& proof_digest, savanna::header_state state) {
   auto candidate = make_checkpoint_entry(std::move(checkpoint));
   candidate.canonical_anchors = canonical_anchors(replay, candidate.position, limits_);
   auto retained_replay = std::make_shared<const savanna::finality_replay>(std::move(replay));
   auto replay_entry = replay_state_entry{
       .anchor = anchor,
       .proof_digest = proof_digest,
       .state = std::make_shared<const savanna::header_state>(std::move(state)),
       .replay = retained_replay,
   };
   const auto lock = std::lock_guard{mutex_};
   if (candidate.position.chain != chain_) {
      FORGE_THROW_EXCEPTION(exceptions::wrong_chain, "Savanna finality checkpoint belongs to another chain");
   }

   const auto& current = preferred_entry_locked();
   if (const auto known = find_at_height_locked(candidate.position.finalized_block_num)) {
      if (known->position.block != candidate.position.block) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                               "Savanna finality checkpoint conflicts at a retained finalized height");
      }
      if (known->checkpoint_bytes && candidate.checkpoint_bytes &&
          *known->checkpoint_bytes != *candidate.checkpoint_bytes) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                               "Savanna finality checkpoint conflicts with the trusted canonical checkpoint");
      }
      commit_replay_state_locked(std::move(replay_entry));
      return;
   }

   if (candidate.position.finalized_block_num < current.position.finalized_block_num) {
      const auto* canonical = find_canonical_anchor_at_height_locked(candidate.position.finalized_block_num);
      if (!canonical) {
         FORGE_THROW_EXCEPTION(exceptions::trust_required, "Savanna finality checkpoint is older than preferred trust "
                                                           "and its canonical ancestry is no longer retained");
      }
      if (canonical->block != candidate.position.block) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                               "Savanna finality checkpoint conflicts with retained canonical ancestry");
      }
      commit_replay_state_locked(std::move(replay_entry));
      return;
   }

   if (!replay_contains(*retained_replay, current.position)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "Savanna finality checkpoint does not descend from the current preferred checkpoint");
   }

   for (const auto& configured : configured_roots_) {
      reject_conflicting_height(configured, candidate);
   }
   for (const auto& rolling : rolling_checkpoints_) {
      reject_conflicting_height(rolling, candidate);
   }

   const auto replay_cached = has_replay_state_locked(replay_entry.anchor, replay_entry.proof_digest);
   rolling_checkpoints_.push_back(std::move(candidate));
   auto replay_appended = false;
   try {
      if (!replay_cached) {
         replay_states_.push_back(std::move(replay_entry));
         replay_appended = true;
      }
   } catch (...) {
      rolling_checkpoints_.pop_back();
      throw;
   }

   if (rolling_checkpoints_.size() > rolling_checkpoint_capacity) {
      rolling_checkpoints_.pop_front();
   }
   if (replay_appended && replay_states_.size() > replay_state_capacity) {
      replay_states_.pop_front();
   }
}

savanna_trusted_position
savanna_finality_trust_store::checkpoint_position(const savanna::finality_checkpoint_bootstrap& checkpoint) {
   const auto trust = savanna::finality_trust{checkpoint};
   const auto anchor = savanna::trust_anchor(trust);
   return {
       .chain = anchor.chain,
       .block = anchor.block,
       .finalized_block_num = operational_block_num(trust),
   };
}

bool savanna_finality_trust_store::replay_contains(const savanna::finality_replay& replay,
                                                   const savanna_trusted_position& position) {
   return std::ranges::any_of(replay.anchors, [&](const auto& anchor) {
      return anchor.chain == position.chain && anchor.block == position.block &&
             anchor.block_num == position.finalized_block_num;
   });
}

savanna_finality_trust_store::trusted_entry
savanna_finality_trust_store::make_configured_entry(savanna::finality_trust trust) {
   auto checkpoint_bytes = std::optional<protocol::bytes>{};
   if (const auto* checkpoint = std::get_if<savanna::finality_checkpoint_bootstrap>(&trust)) {
      checkpoint_bytes = forge::raw::pack(checkpoint->value);
   }
   const auto anchor = savanna::trust_anchor(trust);
   return {
       .position =
           {
               .chain = anchor.chain,
               .block = anchor.block,
               .finalized_block_num = operational_block_num(trust),
           },
       .checkpoint_bytes = std::move(checkpoint_bytes),
       .trust = std::make_shared<const savanna::finality_trust>(std::move(trust)),
   };
}

savanna_finality_trust_store::trusted_entry
savanna_finality_trust_store::make_checkpoint_entry(savanna::finality_checkpoint_bootstrap checkpoint) {
   auto checkpoint_bytes = forge::raw::pack(checkpoint.value);
   auto trust = savanna::finality_trust{std::move(checkpoint)};
   const auto anchor = savanna::trust_anchor(trust);
   return {
       .position =
           {
               .chain = anchor.chain,
               .block = anchor.block,
               .finalized_block_num = operational_block_num(trust),
           },
       .checkpoint_bytes = std::move(checkpoint_bytes),
       .trust = std::make_shared<const savanna::finality_trust>(std::move(trust)),
   };
}

std::vector<protocol::state_anchor>
savanna_finality_trust_store::canonical_anchors(const savanna::finality_replay& replay,
                                                const savanna_trusted_position& checkpoint,
                                                savanna::finality_witness_limits limits) {
   auto result = std::vector<protocol::state_anchor>{};
   result.reserve(std::min(replay.anchors.size(), static_cast<std::size_t>(limits.max_blocks)));
   auto previous = std::optional<protocol::block_num>{};
   auto contains_checkpoint = false;
   for (const auto& anchor : replay.anchors) {
      if (anchor.chain != checkpoint.chain || (previous && anchor.block_num <= *previous)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality, "Savanna finality replay has invalid canonical ancestry");
      }
      previous = anchor.block_num;
      if (anchor.block_num > checkpoint.finalized_block_num) {
         continue;
      }
      if (result.size() == limits.max_blocks) {
         FORGE_THROW_EXCEPTION(exceptions::resource_exhausted,
                               "Savanna finality checkpoint canonical ancestry exceeds the witness block limit");
      }
      contains_checkpoint = contains_checkpoint ||
                            (anchor.block == checkpoint.block && anchor.block_num == checkpoint.finalized_block_num);
      result.push_back(anchor);
   }
   if (!contains_checkpoint) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "Savanna finality replay does not contain its finalized checkpoint");
   }
   return result;
}

void savanna_finality_trust_store::add_configured_root(savanna::finality_trust trust) {
   auto entry = make_configured_entry(std::move(trust));
   if (configured_roots_.empty()) {
      chain_ = entry.position.chain;
   } else if (entry.position.chain != chain_) {
      FORGE_THROW_EXCEPTION(exceptions::wrong_chain,
                            "Savanna finality verifier requires configured roots from one chain");
   }

   for (const auto& configured : configured_roots_) {
      if (configured.position.block == entry.position.block) {
         if (forge::raw::pack(*configured.trust) == forge::raw::pack(*entry.trust)) {
            return;
         }
         FORGE_THROW_EXCEPTION(exceptions::invalid_request,
                               "Savanna finality verifier contains a duplicate configured root");
      }
      if (configured.position.finalized_block_num == entry.position.finalized_block_num) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_request,
                               "Savanna finality verifier contains distinct configured roots at one finalized height");
      }
   }
   configured_roots_.push_back(std::move(entry));
}

const savanna_finality_trust_store::trusted_entry& savanna_finality_trust_store::preferred_entry_locked() const {
   const auto prefer = [](const trusted_entry* current, const trusted_entry& candidate) {
      return !current || candidate.position.finalized_block_num > current->position.finalized_block_num ? &candidate
                                                                                                        : current;
   };

   const auto* preferred = static_cast<const trusted_entry*>(nullptr);
   for (const auto& configured : configured_roots_) {
      preferred = prefer(preferred, configured);
   }
   for (const auto& rolling : rolling_checkpoints_) {
      preferred = prefer(preferred, rolling);
   }
   return *preferred;
}

const savanna_finality_trust_store::trusted_entry*
savanna_finality_trust_store::find_at_height_locked(protocol::block_num height) const {
   const auto matches = [&](const trusted_entry& entry) { return entry.position.finalized_block_num == height; };
   const auto configured = std::ranges::find_if(configured_roots_, matches);
   if (configured != configured_roots_.end()) {
      return &*configured;
   }
   const auto rolling = std::ranges::find_if(rolling_checkpoints_, matches);
   return rolling == rolling_checkpoints_.end() ? nullptr : &*rolling;
}

const protocol::state_anchor*
savanna_finality_trust_store::find_canonical_anchor_at_height_locked(protocol::block_num height) const {
   const auto* selected = static_cast<const protocol::state_anchor*>(nullptr);
   for (const auto& checkpoint : rolling_checkpoints_) {
      for (const auto& anchor : checkpoint.canonical_anchors) {
         if (anchor.block_num != height) {
            continue;
         }
         if (anchor.chain != chain_) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                                  "Savanna finality checkpoint retains canonical ancestry for another chain");
         }
         if (selected && *selected != anchor) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                                  "Savanna finality checkpoints retain conflicting canonical ancestry");
         }
         selected = &anchor;
      }
   }
   return selected;
}

void savanna_finality_trust_store::reject_conflicting_height(const trusted_entry& existing,
                                                             const trusted_entry& candidate) {
   if (existing.position.finalized_block_num != candidate.position.finalized_block_num) {
      return;
   }
   if (existing.position.block != candidate.position.block ||
       (existing.checkpoint_bytes && candidate.checkpoint_bytes &&
        *existing.checkpoint_bytes != *candidate.checkpoint_bytes)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality,
                            "Savanna finality checkpoint conflicts with a retained checkpoint");
   }
}

bool savanna_finality_trust_store::has_replay_state_locked(const protocol::state_anchor& anchor,
                                                           const protocol::digest& proof_digest) const {
   return std::ranges::any_of(
       replay_states_, [&](const auto& entry) { return entry.anchor == anchor && entry.proof_digest == proof_digest; });
}

void savanna_finality_trust_store::commit_replay_state_locked(replay_state_entry staged) {
   const auto found = std::ranges::find_if(replay_states_, [&](const auto& existing) {
      return existing.anchor == staged.anchor && existing.proof_digest == staged.proof_digest;
   });
   if (found != replay_states_.end()) {
      return;
   }
   replay_states_.push_back(std::move(staged));
   if (replay_states_.size() > replay_state_capacity) {
      replay_states_.pop_front();
   }
}

} // namespace forge::chain::api::detail
