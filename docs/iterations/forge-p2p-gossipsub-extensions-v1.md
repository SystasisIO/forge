# GossipSub Extensions: Stage 6 PR12

## Status And Boundary

Implementation plan for `forge-p2p-gossipsub-extensions-v1`, PR 171.
The inherited PR11 testing follow-up is delivered separately from new runtime
support. No extension is supported merely because its codec or manifest entry
exists. Stage 7 plugin changes are discussed after this PR, not implemented here.

This is the last currently scheduled Stage 6 runtime PR. UPnP remains deferred.
Stage 8 production/hostile/soak proof remains separate.

## Donor Baseline

- libp2p specs: `6b6203ee6f62938ce67efdb33498173f475851c0`, GossipSub v1.2,
  v1.3 and Partial Messages, including `extensions/extensions.proto`.
- Go PubSub: `0ed6f6fdad7eb820486892336bed3081f8fa7f25`.
- Rust libp2p: `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`.
- Preserve pinned source/binary provenance and compare behaviors, not identical
  operational defaults where donors legitimately differ.

## Implementation Order

1. Add bounded wire values/codecs for IDONTWANT, first-RPC capability
   advertisement and Partial Messages. Keep ordinary message/signature encoding
   unchanged. Recognize unknown fields without interpreting them as support.
2. Integrate version selection and per-stream extension generations in existing
   inbound/outbound stream owners. Add no second negotiation or network loop.
3. Add IDONTWANT suppression with per-peer count/byte/TTL bounds, existing
   heartbeat expiry and final outbound admission checks. Do not penalize a peer
   merely for sending a message after IDONTWANT.
4. Add opt-in topic-level partial-message operations. Applications own group
   identity, part encoding, reconstruction and semantic validation. The library
   exposes authenticated sender/topic context and bounded send/receive behavior.
   Both mesh and non-mesh peers must be usable. Partial support must retain
   ordinary full-message reception and mixed-version fallback.
5. Add native, package, adversarial and live donor evidence, then independent
   exact-head review/fix loops. Promote support entries only from passed evidence.

## Ownership And Resource Invariants

- Reuse existing node lifecycle, peer generations, stream write gates,
  outbound-byte reservations, validation admission and heartbeat.
- Remote extension state belongs to its authenticated stream generation; a
  replacement stream cannot inherit first-RPC state accidentally.
- Subscription support and request flags are topic-scoped. Unsubscribe removes
  them; stale generations cannot recreate them.
- Requesting partial data implies support for sending it. Do not send partial
  data to a peer that did not request it. Metadata also requires negotiated
  support; unknown extensions are ignored.
- Callbacks execute outside node/engine locks. Pending operations have bounded
  admission, cancellation and deterministic shutdown ownership. Cancellation
  requests do not imply that arbitrary application awaitables have completed;
  shutdown joins the actual callbacks and requires cooperative application code.
- The network library does not accumulate product reconstruction state or
  introduce arbitrary byte splitting, file storage, a second mesh, or persistence.
- Cohesive private components use exact `details/X.hxx` and `X.cpp` pairs.
  Existing owner aspects may be extended; no file splitting merely by line count.

## Partial Message Application Boundary

Topic registration is immutable for its lifetime. The existing full-message
subscribe/publish path remains available and continues to verify full-message
signatures. Partial messages do not automatically become accepted full messages,
fulfil IWANT promises or improve delivery scores.

- Receive callbacks own their payload, receive the authenticated transport peer
  (not proof of content authorship), and have an explicit cancellation context.
- Explicit partial send checks the actual current stream and remote topic flags
  after every admission/open wait. It reports unsupported, backpressure or closed
  outcomes rather than silently dropping a body or converting it to a full message.
  Successful write is not a delivery or reconstruction acknowledgement.
- A bounded local `(topic, group_id)` advertisement registry supplies gossip even
  when the full-message cache is empty. Applications explicitly register, refresh
  and forget entries; heartbeat expiry bounds them. Remote traffic does not renew
  local ownership indefinitely. Parts and reconstruction state remain outside Forge.
- Existing router selection supplies a bounded off-mesh recipient snapshot to
  the gossip callback. The snapshot grants no lasting send permission; each send
  rechecks the current peer generation and capabilities. Metadata-only responses
  and off-mesh replies are first-class paths.
- At most one gossip callback per registration runs at once; a busy registration
  skips a tick instead of accumulating work. Receive tasks and owned bytes also
  have node-wide bounds. Callbacks may reenter send operations without holding a
  node lock or stream write gate and may execute concurrently for a topic.
- Unsubscribe closes callback admission and requests cancellation. It does not
  promise a callback join; node shutdown owns that join. Local callback failure
  or overload must not automatically become a peer protocol penalty.
- An opaque, non-owning `partial_topic` token binds local operations to the exact
  node ownership identity, topic and subscription generation. Copy/destruction
  does not unsubscribe. The partial subscribe overload returns this token and
  callbacks carry it. Group mutation, peer snapshots, partial sends and scoped
  unsubscribe require it; an old callback cannot mutate a replacement subscription.
  Async operations own their token by value. The existing name-only unsubscribe
  remains an explicit operation on the current subscription for its external owner.
- A callback may await partial send or scoped unsubscribe, but must not await
  shutdown of its own node: shutdown joins that callback. Cancellation before a
  native write is distinct from cancellation after a frame has started; the latter
  cannot promise no remote bytes and must not leave a corrupt stream reusable.

Full-message forwarding is not suppressed merely because a topic supports
partials. A working application partial path must own any such decision, and
peers requesting parts still accept full messages for mixed-network fallback.

## Acceptance

- Hardcoded protobuf fixtures, mixed known/unknown fields, malformed sizes,
  IDONTWANT limits/expiry and unchanged full-message signature bytes.
- First-RPC advertisement, reconnect/generation isolation, absent/unknown
  extensions and version fallback through 1.0/1.1/1.2/1.3.
- Suppression of queued duplicates, independent peers, bounded flood state,
  no false scoring penalty, callback failure and shutdown during blocked I/O.
- Partial capability combinations, eager parts, latest metadata, off-mesh
  replies, full-message fallback, invalid application data and byte limits.
- Live bilateral Forge/Go/Rust exchanges for the nine planned manifest contracts
  (three capabilities by native QUIC/native TCP/private TCP), with four donor
  directions each. Negotiated protocol and actual extension traffic are required.
- Focused P2P/plugin regression/package suites, structure, inventory, fixture
  locks and `git diff --check`; coordinator-only build with `-j4`, no full CI.

## Rust Shutdown Limitation

The [local source patch](../donors/forge-net-p2p-rust-quic-shutdown-v1.md)
is not submitted upstream. Original traffic and explicitly patched shutdown
evidence remain separate. Historical failures and original Rust QUIC shutdown
`NOT_PROVEN` are preserved; no result borrows completion from another run.

## Delivery

After clean exact-head acceptance and review, merge into `dev`. Prepare the next
Stage 7 PR for discussion only. Do not implement plugin configuration or expose
a mutable node accessor in this PR. Use bounded prompts for persistent developers
and independent reviewers, one writer on shared code and one coordinator build.
Archive evidence before deleting this task's inactive compilation directories;
do not remove neighboring worktrees, shared caches or donor repositories.
