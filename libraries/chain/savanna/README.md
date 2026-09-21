# Forge Chain Savanna

`forge_chain_savanna` is the Preview operational kernel for Savanna-style
instant finality. It owns policy validation, strong/weak finality transitions,
quorum-certificate verification and construction, bounded validation
commitments, finalizer safety decisions and deterministic fork ranking without
owning a blockchain controller.

The separate dual-target `forge.chain.savanna.values` leaf owns the canonical
finalizer record and the generation-bearing operational finalizer policy. Chain
Protocol reuses the finalizer record. Its separate generation-free
`forge::chain::protocol::finalizer_policy` remains the Spring contract payload;
the two policy types are intentionally not interchangeable.

## Package

```cmake
find_package(Forge CONFIG REQUIRED COMPONENTS chain_savanna)
target_link_libraries(my_target PRIVATE Forge::forge_chain_savanna)
```

The component automatically provides Chain Core, Chain Protocol, Chain Quorum,
Crypto Asymmetric, Crypto BLS, Crypto Digest, Raw and Variant dependencies.

## Modules

- `forge.chain.savanna.types`
- `forge.chain.savanna.policy`
- `forge.chain.savanna.finality_core`
- `forge.chain.savanna.qc`
- `forge.chain.savanna.vote`
- `forge.chain.savanna.vote_accumulator`
- `forge.chain.savanna.validation`
- `forge.chain.savanna.finalizer_safety`
- `forge.chain.savanna.rank`
- `forge.chain.savanna.exceptions`

The same `forge_chain_savanna` target also provides the protocol-bound modules
`policy_state`, `extensions`, `genesis`, `header_state`, `candidate`,
`checkpoint`, `admission` and `finality_witness`; this upper integration is not
a second target or facade. Neutrality is module-level: `exceptions`, `types`,
`policy`, `finality_core`, `qc`, `vote`, `vote_accumulator`, `validation`,
`finalizer_safety` and `rank`, including their implementation files, have no
Chain Protocol import. `policy_state` holds protocol-bearing finalizer proofs
and producer schedule state without duplicating the neutral types and policy
modules. `genesis` owns the value, validation and chain-ID calculation only;
file and JSON loading remain product-owned.

## Operational Types

Block number and slot are explicit `std::uint32_t` values. `block_ref` does not
derive a number from a protocol-specific block ID:

```cpp
import forge.chain.savanna.finality_core;

namespace savanna = forge::chain::savanna;

auto core = savanna::finality_core::genesis(0, 10);

core = core.next(
    savanna::block_ref{
        .num = 0,
        .id = genesis_id,
        .slot = 10,
        .finality_digest = genesis_finality_digest,
        .active_policy_generation = 1,
    },
    savanna::qc_claim{.block = 0, .strong = true});
```

Finalizer policies require a non-empty unique BLS key set without the identity
key, checked total weight and a reachable strict-majority threshold. Admission
also supplies one BLS proof of possession per finalizer. `validate()` returns a
non-serializable `verified_finalizer_policy`, and QC verification accepts only
that verified capability. The raw policy remains the donor-compatible
operational record and is available through `get()`.

Policy generations advance exactly one step. Ordered diffs use ascending
removal and insertion indexes. `apply()` reuses verification for retained keys
and requires proofs aligned with newly inserted finalizers. A downstream
adapter must retain or recover registration proofs when reconstructing an
operational verified policy after restart.

## Compatibility Invariants

`finality_core::pack_for_digest()` preserves the Spring consensus preimage. A
block reference contributes only `id`, `slot` and `finality_digest`; explicit
block number and policy generations are deliberately excluded. Raw layouts and
digest fixtures are covered by donor-backed tests.

QC verification delegates weighted threshold evaluation to
`forge_chain_quorum` and grouped aggregate signature verification to
`forge_crypto_bls`. Same-message key aggregation is available only after
proof-of-possession validation, preventing rogue-key attacks. A finalizer
present in both active and pending policies must cast the same strong/weak vote
in both signatures. A weak QC must contain at least one weak vote, and its
strong-vote subset must remain below threshold, so a strong quorum cannot be
represented non-canonically as weak.

`verify()` returns a non-serializable `verified_quorum_certificate`. In
addition to the checked certificate, this capability records which public keys
are proven strong voters and the finality digest against which their signatures
were verified. Finalizer safety updates accept the capability rather than an
unchecked QC record and require its digest to match the candidate.

## Bounded Validation

`validation_state` is an encapsulated, versioned value. It stores a compact
Merkle frontier before the retained range, the current frontier, retained roots
and the corresponding neutral leaf preimages:

```cpp
auto state = savanna::make_validation(genesis_leaf);
state = savanna::append(std::move(state), next_leaf);

const auto finalized_root = savanna::root_at(state, finalized);
state = savanna::advance_finalized(std::move(state), finalized);
```

`advance_finalized()` retains the finalized block itself and removes older
roots and preimages. The prefix frontier keeps the complete Merkle history in
`O(log N)` space. A caller chooses and enforces its reversible window by
advancing the state whenever finality moves. Looking up a pruned root throws
`validation_root_unavailable`.

## Finality Trust Advance

`advance_finality_trust()` derives a checkpoint bootstrap solely from a
caller-trusted genesis or checkpoint bootstrap, a decoded witness (or its Raw
proof) and a finalized `state_anchor`. It replays headers, producer signatures,
QC, policy transitions and validation state before it advances validation to
the finalized target. The returned checkpoint is local replay output; witness
bytes never supply a checkpoint as trust material. Use
`advance_finality_trust_with_replay()` when the verified replay anchors are
also needed for an ancestry-aware trust promotion.

`finality_replay::producer_opportunities` records every expected timestamp
slot between replayed headers. Each value is selected from the replayed proposer
policy at that slot; skipped slots carry no `produced_block`, while the produced
slot carries its block identity and number. This makes missed slots observable
without treating a produced block as a schedule opportunity. Policy transitions
use the same canonical selection as admission. Materialization is bounded by
`finality_witness_limits::max_producer_slots`. The Savanna schedule-version
sentinel (`1 << 31`) remains a header-format marker; it is never compared with
a producer-policy schedule version.

`append()` is `O(log N)` and does not replay the retained range. Full replay is
performed by explicit `validate()` and by Raw decoding. The Raw v1 layout is:

1. version;
2. prefix incremental-Merkle frontier;
3. current incremental-Merkle frontier;
4. first retained block number;
5. retained roots;
6. retained leaf preimages.

The old Forge 8.13.0 Preview layout is intentionally not decoded. Unknown
versions and inconsistent frontiers fail with
`forge::raw::exceptions::codec_error`.

## Vote Accumulation

`vote_accumulator` is a move-only, thread-safe, ephemeral collector for one
candidate:

```cpp
savanna::vote_accumulator votes{
    candidate,
    active_verified_policy,
    pending_verified_policy};

const auto result = votes.add({
    .block = candidate.id,
    .finalizer = finalizer_key,
    .kind = savanna::vote_kind::strong,
    .signature = signature,
});

if (auto qc = votes.best()) {
   publish(*qc);
}
```

Construction verifies that the supplied active and optional pending policy
generations exactly match the candidate. A candidate that names a pending
generation requires that policy, while a candidate without one rejects an
extra pending policy.

It implements the donor states `unrestricted`, `restricted`,
`weak_achieved`, `weak_final` and `strong`. A finalizer shared by active and
pending policies is updated atomically. BLS verification is done outside the
state mutex and duplicate/conflict state is checked again before mutation.
`observe()` validates an external QC before accepting it; `best()` chooses
between locally aggregated and received candidates with strong-over-weak
ordering. Active and pending policy signatures remain paired as one complete
certificate throughout storage and selection. Optional vote sets use the
canonical donor encoding: an all-weak QC omits its empty strong-vote bitset.

The accumulator is deliberately not serializable. Losing pending votes on
restart affects progress only; it cannot authorize an invalid QC. Networking,
unknown-block queues and connection policy belong to the node runtime.

## Finalizer Safety

`finalizer_safety_state` carries the donor `last_vote`, `lock` and
`other_branch_latest_time` mechanics using explicit block slots:

```cpp
auto safety = savanna::make_finalizer_safety(initial_lock);
const auto plan = savanna::plan_vote(safety, candidate_core, candidate);

if (plan.decision != savanna::vote_decision::abstain) {
   co_await persist_safety(plan.next);
   const auto signature = sign(
       savanna::message_for_vote(candidate.finality_digest,
                                 plan.decision == savanna::vote_decision::strong
                                     ? savanna::vote_kind::strong
                                     : savanna::vote_kind::weak));
   publish(signature);
}
```

`plan_vote()` checks slot monotonicity, higher-QC liveness and ancestry safety,
then returns the next versioned Raw state. The downstream controller **must
durably persist `vote_plan.next` before signing or publishing the vote**.
Forge intentionally owns neither the DB/file implementation nor key custody.

`advance_from_qc()` updates safety state only from a verified strong QC that
proves the local finalizer cast a strong vote. After restart, the product
restores the serialized safety state and reconstructs verified policies from
the required BLS proofs of possession.

Chain code does not include or call the BLS vendor.

## Boundaries

This library does not own protocol blocks, producer schedules, genesis
configuration, header extensions, signed-block admission, controller state,
execution, persistence implementation, P2P synchronization, node lifecycle,
key custody or product policy. Those layers adapt their own protocol records
into the operational types here.
