#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace forge::chain::api::detail {

struct savanna_trusted_position {
   protocol::chain_id chain;
   protocol::block_id block;
   protocol::block_num finalized_block_num = 0;
};

class savanna_finality_trust_store {
 public:
   struct snapshot {
      std::shared_ptr<const savanna::finality_trust> source_trust;
      savanna_trusted_position preferred;
   };

   savanna_finality_trust_store(savanna::finality_trust trust, std::vector<savanna::finality_trust> additional_trusts,
                                savanna::finality_witness_limits limits);

   [[nodiscard]] savanna::finality_trust preferred_trust() const;
   [[nodiscard]] std::optional<protocol::block_id> preferred_trust_anchor() const;
   [[nodiscard]] std::optional<protocol::block_id> trust_anchor_at_or_before(protocol::block_num target) const;
   [[nodiscard]] protocol::chain_id trusted_chain() const;
   [[nodiscard]] savanna::finality_witness_limits witness_limits() const;
   [[nodiscard]] std::shared_ptr<const savanna::header_state>
   cached_replay_state(const protocol::state_anchor& anchor, const protocol::digest& proof_digest) const;
   [[nodiscard]] std::shared_ptr<const savanna::finality_replay>
   cached_replay(const protocol::state_anchor& anchor, const protocol::digest& proof_digest) const;
   [[nodiscard]] snapshot take_snapshot(const protocol::state_anchor& expected,
                                        const savanna::finality_witness& witness) const;

   void install_verified(savanna::finality_checkpoint_bootstrap checkpoint, savanna::finality_replay replay,
                         const protocol::state_anchor& anchor, const protocol::digest& proof_digest,
                         savanna::header_state state);

   [[nodiscard]] static savanna_trusted_position
   checkpoint_position(const savanna::finality_checkpoint_bootstrap& checkpoint);
   [[nodiscard]] static bool replay_contains(const savanna::finality_replay& replay,
                                             const savanna_trusted_position& position);

 private:
   struct trusted_entry {
      savanna_trusted_position position;
      std::optional<protocol::bytes> checkpoint_bytes;
      std::shared_ptr<const savanna::finality_trust> trust;
      std::vector<protocol::state_anchor> canonical_anchors;
   };

   struct replay_state_entry {
      protocol::state_anchor anchor;
      protocol::digest proof_digest;
      std::shared_ptr<const savanna::header_state> state;
      std::shared_ptr<const savanna::finality_replay> replay;
   };

   [[nodiscard]] static trusted_entry make_configured_entry(savanna::finality_trust trust);
   [[nodiscard]] static trusted_entry make_checkpoint_entry(savanna::finality_checkpoint_bootstrap checkpoint);
   [[nodiscard]] static std::vector<protocol::state_anchor>
   canonical_anchors(const savanna::finality_replay& replay, const savanna_trusted_position& checkpoint,
                     savanna::finality_witness_limits limits);
   void add_configured_root(savanna::finality_trust trust);
   [[nodiscard]] const trusted_entry& preferred_entry_locked() const;
   [[nodiscard]] const trusted_entry* find_at_height_locked(protocol::block_num height) const;
   [[nodiscard]] const protocol::state_anchor* find_canonical_anchor_at_height_locked(protocol::block_num height) const;
   static void reject_conflicting_height(const trusted_entry& existing, const trusted_entry& candidate);
   [[nodiscard]] bool has_replay_state_locked(const protocol::state_anchor& anchor,
                                              const protocol::digest& proof_digest) const;
   void commit_replay_state_locked(replay_state_entry staged);

   static constexpr auto rolling_checkpoint_capacity = std::size_t{16};
   static constexpr auto replay_state_capacity = std::size_t{16};

   savanna::finality_witness_limits limits_;
   protocol::chain_id chain_;
   mutable std::mutex mutex_;
   std::vector<trusted_entry> configured_roots_;
   std::deque<trusted_entry> rolling_checkpoints_;
   std::deque<replay_state_entry> replay_states_;
};

} // namespace forge::chain::api::detail
