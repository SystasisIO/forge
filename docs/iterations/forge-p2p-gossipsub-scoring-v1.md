# Forge P2P Stage 6 PR11: GossipSub Scoring

## Scope And Baseline

Branch: `forge-p2p-gossipsub-scoring-v1`, from the reviewed PR10 merge in `dev`.
This document describes the implementation and acceptance contract. Run-specific
results remain in the PR and immutable receipts; it makes no production-support
claim.

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

The four native processes first form two one-member meshes joined by a known,
subscribed but off-mesh connection. Before the rejection phase, a real publish
must traverse IHAVE, IWANT and cached MESSAGE on that off-mesh connection.
After a real rejected publish crosses the P4 threshold, the victim must send
PRUNE/GRAFT and deliver a new accepted message through the replacement to the
otherwise unreachable sink. Ignore must neither penalize nor forward.

All actors use a common signed message-ID policy: raw author PeerId bytes
followed by the eight-byte big-endian sequence. This is the Forge/Go default;
Rust selects it through its public `ConfigBuilder::message_id_fn`. Its native
default is textual PeerId plus decimal sequence and is intentionally different.
No donor router is patched, no score or mesh membership is mutated by the
observer, and no manual GRAFT is used to manufacture the result.

Raw actor results only establish observations. Canonical promotion also binds
the clean exact head, immutable donor exports, source/binary hashes, owned PIDs,
launch arguments, joined shutdown and indexed final JSON snapshots. Rust's
public Behaviour exposes scores but not P4 counters: unavailable counters stay
explicitly null, never reconstructed from the score.

Add focused regressions for decay, attack penalties, ignored/transient input,
mesh recovery, gossip promises, bounds, cancellation and deterministic shutdown.
Run focused native/package/structure/inventory and live interop acceptance in
one coordinator-owned build with `-j4`, then two independent exact-head reviews.
Do not promote planned evidence or infer production support from unit tests.

### Review Evidence Boundaries

Native regressions must distinguish a retired physical session's outcome from
the current authenticated session's failures, backoff, score and mesh. The
failure-accounting guard and its bounded mutation share the node lock; cleanup
must still retire the failed owner if local persistence admission throws.

An unfinished native stream-open keeps a strong transport model owner across
session retirement and exception cleanup. Moving the session facade must not
invalidate a suspended Yamux/QUIC receiver. The native reconnect regression
settles independent Identify/Push streams before measuring resources and
accounts separately for the old caller's reservation while G2 connects. It
requires the existing typed cancellation outcome for explicitly canceled G1,
an exact timeout outcome for the current session and exact reservation return.

Joined node stop clears both staged and active gossip promises without
resetting token generations or the router's random state. This is Forge's
explicit deterministic lifecycle contract, not a claim that the pinned donors
expose the same stop/clear API. The allocation rollback regression sweeps all
measured preparation sites for two immutable commands; its observed 165 sites
are not a protocol limit or proof for every possible input.

Remote ephemeral gossip retains its original authenticated physical session
through admission, staging, writes, outcomes and failure attribution. An expired
origin cannot create a request or penalty in a replacement session. Independent
durable GRAFT/PRUNE controls must still drain. The regression uses real G1/G2
Noise/Yamux sessions and a real received IHAVE, then invokes the late dispatch
entry deterministically with the retired owner. This proves the entry contract,
not a naturally reproduced native scheduling race.

Cached MESSAGE replies are bounded individually, rather than aggregating every
matching cache entry into one oversized RPC. The native regression publishes
two real cached messages, requests both in one IWANT and requires two separate
valid wire replies whose combined RPC would exceed the configured payload limit.

Coroutine-factory allocation failure remains a separate runtime coverage gap.
An ordinal allocator failure during admission is not proof of that exact site:
Asio frame recycling and allocator selection may change the allocation path.
Source ordering and general allocation-recovery tests must not be presented as
a site-specific native factory-OOM regression.

The original pinned Rust QUIC transport cannot publicly expose every native
shutdown cause. The maintainer approved separate original-wire and instrumented
shutdown evidence on 2026-10-08. The four Rust QUIC cases retain original raw
errors and independently prove traffic through active indexed snapshots; a
separate accessor-only TEST COPY repeats the full scenario and proves shutdown.
Tokens, owners and causes are never borrowed between executions. Original
shutdown remains `NOT_PROVEN`; combined acceptance is explicitly scoped to
original-wire/instrumented-shutdown. PR11 merged after both scoped gates passed
on the reviewed head; this did not prove original-donor shutdown.

The post-merge [original Rust QUIC diagnosis](../donors/forge-net-p2p-rust-quic-shutdown-v1.md)
isolates the opaque-error limitation with native and two-host reproductions.
It prepares an upstream error-source proposal without modifying the canonical
donor or relaxing the original shutdown classification.

### Shutdown Evidence Repairs

After all four Prepare acknowledgements, the fixture keeps every host alive
until all Go PubSub owners acknowledge an actual quiesce. PubSub cancellation is
separate from the live controller. Open/write and application callback admission
close; retained native I/O/framing/terminal operations, rejected-stream disposal,
subscriber work and admitted callbacks must drain before the PID/identity-bound
ACK. Native-error observation remains enabled. Python rejects missing/foreign
ACKs and later native PubSub writes or selected owners. No public join of Go's
internal router is inferred.

Retention distinguishes closed admission from invalid owners atomically. A
previously admitted native open or handler may finish after closing is published
but before context cancellation; a valid stream then returns fixture cancellation
and is fully reset before its active reservation is released. Empty IDs, wrong
protocols/authentication, duplicate owners, capacity overflow and failed native
Reset remain fatal even with an already canceled context. The deterministic
regression holds the actual rejected Reset across this closing-before-cancel
boundary, checks its receipt and prevents premature drain completion. It does not
prove the live QUIC scenario or alter native Close0 acceptance.

Terminal operations remain globally accounted through RETURN, receipt capture
and framing finalization even after their stream leaves the retained registry.
The regression holds a repeated Close/Reset on that released wrapper and requires
ACK to remain unavailable until the call completes; native failures stay sticky.
Unit failure paths release their gates and bounded-join their actual owners,
with fail-closed cleanup if an owner does not finish.

Only then does the fixture publish all donor stop commands before awaiting any
donor. Forge remains alive until every donor has exited successfully, drained
native owners and supplied an identity-bound shutdown receipt. Failure cleanup
still stops all processes, but cannot create acceptance evidence.

Go QUIC validation distinguishes a failed repeated Close from its independently
finalized successful Reset. An empty, late-born unselected stream may retain a
sealed AppClosed0 error only with its actual live parent baseline and successful
full disposal after each failed native return. Neither path invents a stream
baseline, rewrites raw errors nor exports RPC, framing or scoring authority.
Source-only review and replay of saved receipts do not replace a fresh canonical
run; the original Rust observation limitation remains explicitly documented.

Whole-owner Go teardown cancels the root context before its subscriber child and
does not perform live subscription/topic removal, which could open another
stream during simultaneous peer shutdown. The focused test binds this ordering
to the actual defer and requires native host/worker/resource disposal. Any
unjoined test owner exhausts a bounded failure budget and fails the process,
rather than returning with an orphan background task. Native I/O failures are
not reclassified; quiesce does not serialize the donor's concurrent Close/Reset
or relax the returned-Reset-before-Close rule for repeated Close.

A separate concurrent Reset/Close observation retains a direct local Yamux
code-zero Close error with unknown cause and `native_close_succeeded=false`.
At Close BEGIN it pins the exact already-running full Reset on that authenticated
owner. Finalization requires that same Prepared Reset to return successfully,
publish its indexed receipt and fully dispose the owner, with all observed
operations joined, both RPC decoders clean and no prior failure or overflow.
Reset may return before or after Close RETURN; its receipt proves disposal,
never the cause of the Close error. A future or superseded Reset cannot supply
this evidence. Wrapped, nonzero, remote or pre-Prepare errors remain fatal.
The finalizer seals the native operation order while claiming joined state.
The independent checker considers all native BEGIN counters, including receipts
published after finalization, to reject superseded attempts and operations that
have not returned by that seal. Both the new observation and its finalizer must
precede the actual quiesce acknowledgement.

The donor baseline is `go-yamux/v5@v5.0.1`: `ResetWithError` changes the stream
state before sending/reset cleanup completes; `CloseWrite` returns the stored
write error from `halfReset`. Go PubSub's sender defers Close while peer-dead
cleanup independently resets the stream. Controlled regressions cover both
native-return orders and adversarial receipts. Fresh canonical evidence is
required for live acceptance; prior failed runs remain failed.

PR12 retains IDONTWANT, v1.3 extensions and opt-in Partial Messages. Stage 7
retains official plugin configuration/facets; Stage 8 retains production/hostile
soak proof; Stage 9 retains P2P WebSocket. UPnP remains deferred. No project
version or release change belongs to PR11 preparation.
