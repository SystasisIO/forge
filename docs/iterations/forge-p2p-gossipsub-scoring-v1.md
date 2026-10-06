# Forge P2P Stage 6 PR11: GossipSub Scoring

## Scope And Baseline

Branch: `forge-p2p-gossipsub-scoring-v1`, from the reviewed PR10 merge in `dev`.
This is preparation, not an implementation or passing acceptance claim.

Follow the production implementation roadmap and the donor-first capability
manifest. PR11 owns `pubsub.gossipsub_v1_0_v1_1`: peer/topic scoring, score decay
and thresholds, gossip promises, mesh repair, opportunistic graft, flood publish
and actual v1.0/v1.1 fallback.

Pinned donors:

- Go pubsub `0ed6f6fdad7eb820486892336bed3081f8fa7f25`: `score.go`,
  `gossipsub.go`, `gossipsub_feat.go`.
- Rust libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
  `protocols/gossipsub/src/peer_score.rs`, `behaviour.rs`, `config.rs`.

## Ownership And Failure Semantics

Reuse node PubSub state, its admitted outbound owner, heartbeat, tracked
lifecycle and RAII resource reservations. Scoring is bounded ephemeral
operational state, not new persistence or a plugin-owned network loop.

Keep public ownership in `forge.net.p2p.pubsub`. Introduce exact private entity
pairs only for coherent ownership, following `create-library`; split aspects
of an existing entity without decorative files or unrelated abstractions.

Invalid messages may affect penalties. Ignore, transient validation failure,
local backpressure and cancellation must not count as malicious peer behavior.
Callbacks and network I/O stay outside state mutexes. Physical connection
direction is distinct from TLS/security initiator roles.

## Acceptance And Delivery

The manifest declares six profile/version contracts: forced v1.0 and v1.1 on
native QUIC, native TCP/Yamux and private TCP/Yamux. Prove all 24 bilateral
Forge/Go/Rust direction/profile/version cases with meaningful negative controls.
Newer donor defaults do not prove forced legacy negotiation. Legacy PRUNE must
not emit unsupported peer-exchange/backoff fields.

Evidence must bind actual validation to score mutation, threshold crossings,
PRUNE/GRAFT, changed mesh membership and delivery through a replacement peer.
Record authenticated transport and negotiated stream ownership. Go quantizes
mesh time while Rust uses fractional time; require equivalent documented
behavior, not artificial equality of floating-point scores.

Add focused regressions for decay, attack penalties, ignored/transient input,
mesh recovery, gossip promises, bounds, cancellation and deterministic shutdown.
Run focused native/package/structure/inventory and live interop acceptance in
one coordinator-owned build with `-j4`, then two independent exact-head reviews.
Do not promote planned evidence or infer production support from unit tests.

PR12 retains IDONTWANT, v1.3 extensions and opt-in Partial Messages. Stage 7
retains official plugin configuration/facets; Stage 8 retains production/hostile
soak proof; Stage 9 retains P2P WebSocket. UPnP remains deferred. No project
version or release change belongs to PR11 preparation.
