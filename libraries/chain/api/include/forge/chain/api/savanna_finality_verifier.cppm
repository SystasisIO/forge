module;

#include <memory>
#include <optional>
#include <span>
#include <vector>

export module forge.chain.api.savanna_finality_verifier;

export import forge.chain.api.finality;
export import forge.chain.savanna.finality_witness;

export namespace forge::chain::api {

class savanna_finality_verifier final : public finality_verifier {
 public:
   savanna_finality_verifier(savanna::finality_trust trust, savanna::finality_witness_limits limits = {});
   savanna_finality_verifier(savanna::finality_trust trust, std::vector<savanna::finality_trust> additional_trusts,
                             savanna::finality_witness_limits limits = {});
   ~savanna_finality_verifier();

   savanna_finality_verifier(const savanna_finality_verifier&) = delete;
   savanna_finality_verifier& operator=(const savanna_finality_verifier&) = delete;
   savanna_finality_verifier(savanna_finality_verifier&&) noexcept;
   savanna_finality_verifier& operator=(savanna_finality_verifier&&) noexcept;

   [[nodiscard]] std::optional<protocol::chain_id> trusted_chain() const override;
   [[nodiscard]] savanna::finality_trust preferred_trust() const;
   [[nodiscard]] std::optional<protocol::block_id> preferred_trust_anchor() const override;
   [[nodiscard]] std::optional<protocol::block_id> trust_anchor_at_or_before(protocol::block_num target) const override;
   [[nodiscard]] savanna::header_state replay_state(const protocol::state_anchor& expected,
                                                    const protocol::proof_blob& proof) const;
   [[nodiscard]] savanna::finality_replay verified_replay(const protocol::state_anchor& expected,
                                                          const protocol::proof_blob& proof) const;

   void verify(const protocol::state_anchor& anchor, const protocol::proof_blob& proof) override;
   void verify_ancestry(const protocol::state_anchor& finalized, std::span<const protocol::state_anchor> intermediate,
                        const protocol::proof_blob& proof) override;

 private:
   struct impl;
   [[nodiscard]] static std::unique_ptr<impl> make_impl(savanna::finality_trust trust,
                                                        std::vector<savanna::finality_trust> additional_trusts,
                                                        savanna::finality_witness_limits limits);

   std::unique_ptr<impl> impl_;
};

[[nodiscard]] std::shared_ptr<savanna_finality_verifier>
make_savanna_finality_verifier(savanna::finality_trust trust, savanna::finality_witness_limits limits = {});

[[nodiscard]] std::shared_ptr<savanna_finality_verifier>
make_savanna_finality_verifier_with_trusts(savanna::finality_trust trust,
                                           std::vector<savanna::finality_trust> additional_trusts,
                                           savanna::finality_witness_limits limits = {});

} // namespace forge::chain::api
