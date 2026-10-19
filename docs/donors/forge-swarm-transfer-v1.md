# Swarm Transfer Mechanisms

PR4 extends the existing `forge_swarm_core` target with deterministic transfer
decisions. The [implementation roadmap](../iterations/forge-content-swarm-v1.md)
separates these decisions from the later network runtime and product policy.

## Reviewed Donors

- [BEP 3](https://www.bittorrent.org/beps/bep_0003.html), revision
  `0e08ddf84d8d3bf101cdf897fc312f2774588c9e`: bounded pipelining, interest/choke
  transitions, optimistic unchoke, endgame duplicate cancellation and responses
  crossing cancellation.
- libtorrent commit `b4e9e2471e7abce2a58585a7d7fbc4e8d1edc47e`:
  `include/libtorrent/piece_picker.hpp`, `src/piece_picker.cpp`,
  `src/choker.cpp` and `test/test_piece_picker.cpp`.
- Existing Forge descriptor geometry, piece identifiers, bitmap/checkpoint
  validation and typed exceptions. The Core target remains independent of
  Asio execution, DB, API, network and application libraries.

## Accepted Mechanisms

- Availability counts change with peer advertisements and disconnection.
  Verified local availability is distinct from requested or received blocks.
- Rare pieces improve distribution diversity. Sequential and explicit-priority
  selection are deliberate alternatives; equal candidates need reproducible
  diversity rather than one globally preferred first piece.
- Limit unfinished pieces and their bytes. Under pressure, finish existing
  pieces before opening more work, as libtorrent's partial-piece picker does.
- Keep request ownership per peer and bounded globally. Endgame duplicates use
  the same capacity accounting as normal requests; cancellation of redundant
  work is explicit and late responses cannot revive retired work.
- Integrity verification is a separate transition. Receiving a block does not
  justify advertising its piece, and failure of an aggregate check does not
  identify a guilty peer when several peers supplied its blocks.
- Limit serving slots, retain useful selections between rounds and periodically
  try other interested peers. Product permission is required independently of
  any choke/interest state or throughput observation.

## Composition Boundaries

Time and selection entropy are explicit inputs. The caller serializes access;
these state machines create no locks, timers, tasks, sockets or private runtime.
Their future runtime owner applies decisions through Forge Asio and API.

The chosen mechanisms do not imply BitTorrent/IPFS wire support. Forge has its
own descriptor identity and later peer API. Libtorrent's complete session,
disk worker, transport, economic assumptions and all optional picker modes are
not transplanted into Core.

## Deliberate Policy Differences

Forge priorities are strict: a higher explicit piece priority wins before
rarity, partial-piece preference and endgame copy count. Libtorrent also offers
priority-weighted rarity. The comparison fixture uses unambiguous cases where
these policies agree, not a claim that their entire ordering is identical.

Partial-piece pressure is based on Forge's explicit unfinished-piece and byte
budgets. The donor has different pressure thresholds; its `prioritize_partials`
mode is the oracle for finishing an already open piece, not for those thresholds.
Request IDs and verification generations are local ownership tokens, not
BitTorrent wire fields. Endgame is bounded by outstanding blocks, duplicate
requests and copies per block; the runtime must execute returned cancellations.

The choker consumes cumulative samples at caller-selected rounds. Its optimistic
slot rotates with explicit time; seed rotation uses served-byte quota and age.
These are scheduling decisions, not permission to serve and not durable peer
reputation. No mandatory reciprocal-upload admission rule is introduced.

Current encrypted Storlane provider fragments retain their product API.
Future adapters may describe ciphertext as independent immutable Swarm objects;
decryption is not intrinsically required for transferring or hashing bytes.
That product integration and payment accounting are outside this PR.

## Evidence

`test_forge_swarm_libtorrent` compiles the pinned, unchanged donor picker and
compares unique rarity, filtering, sequential/high-priority selection, partial
preference and an admitted/canceled pipeline. The fixture uses a single null
donor peer and therefore does not claim donor multi-peer endgame or choker
equivalence. Forge simulations separately cover those transitions, ownership,
integrity attribution and bounded long sequences. The existing 55 Merkle
geometry comparisons remain in the same executable.

`test_forge_swarm_allocation` injects allocator failure at each allocation in
fresh/partial/occupied/endgame request admission, choke selection and index
updates, without production test hooks. Exact capacities, nonzero sample deltas,
existing request ownership and retry verify that a failed batch leaves no
invisible requests or consumed choke round. The installed
`swarm_core` consumer imports and executes both mechanisms while rejecting
upper-layer dependency leakage.

The maximum-descriptor probe exercises 262144 pieces with stable inventory,
repeated identical bitmaps and single-bit updates. It reports measurements
without a machine-dependent timing assertion. Setup is outside the measured
loop and both initial indexes are warmed explicitly; Forge's `pick` includes
admission, the donor's does not. Forge accepts a
complete bitmap while the donor sparse update takes one piece index. These are
not equivalent total-work timings and do not establish overall performance
superiority. Source review verifies maintained ordering and sparse updates;
the probe catches repeated full sorting in the common request path.
Another maximum-geometry fixture has 262144 pieces with 1024 blocks each,
independent request/byte limits, and zero/one-byte remaining credit. It checks
bounded ownership and reports the cost of exhausted-budget picks without
materializing the four-TiB payload. Short-tail selection has separate unit tests.

The PR acceptance record retains exact revisions, commands, raw results and
independent reviews. Runtime/network readiness is not inferred from these tests.
