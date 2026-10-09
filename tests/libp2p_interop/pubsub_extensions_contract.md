# PR12 Native Extension Evidence

This is the acceptance design, not a result. Reuse the authenticated native
stream receipts, bounded actor control, preparation barrier and source provenance
from `pubsub_contract.md`. Keep its PR11 scoring scenarios unchanged. New scenarios
must prove extension behavior, not reinterpret a v1.1 exchange as v1.2/v1.3.

## Protocol Selection

Go and Forge offer the actual requested native version. For pinned Rust, v1.2
and v1.3 use the native default protocol configuration; its public custom
`protocol_id(..., Version::V1_1)` API does not activate a new router version.
The opposite Forge/Go endpoint limits negotiation. Every observed stream must
prove its actual selection. A Rust/Rust edge cannot prove a forced v1.2 exchange.

## First-RPC Advertisement

Observe the first successful RPC in both directions on the selected v1.3 stream,
including an explicit Partial Messages advertisement when enabled. Verify later
RPCs do not repeat it. Reconnect and require the new stream's own first-RPC state.
Absent support means no support; unknown extensions are not recognized features.
Native regressions cover replacement substreams when the donor public API cannot
request that operation. Adversarial peers may inject malformed/late advertisements
only in separately labelled negative scenarios.

## IDONTWANT

Use actual router emission, not manually injected control frames, for positive
evidence. Connect publisher P to forwarder F and already-informed peer R; connect
F to R and independent sink S. Hold F's application validation before publication.
Publish a payload above the configured threshold and observe R's native IDONTWANT
for that exact message at F before releasing validation. Require F to deliver to
S without writing the suppressed message to R. Bind IDs to the signed full
message, and inspect completed stream I/O rather than logical queue submission.

Rotate the donor between F and R to cover sending and consuming IDONTWANT. The
other processes are Forge; no donor router, score or mesh state is modified.
Application validation holds have bounded lifetime and cancellation. Separate
native tests cover expiry, independent peers, queued-write races, flood bounds,
IWANT responses and the absence of an invalid-delivery penalty for duplicates.

## Partial Messages

Use real v1.3 routers and application hooks. Native mesh pairs A-L and B-R leave
A-B as a connected off-mesh edge. Locally owned group gossip must cross that edge
with no full-message cache entry, trigger a metadata request, and receive actual
parts. Both donor roles are tested. A full-only helper proves normal signed full
messages remain accepted; it does not stand in for partial delivery evidence.

The fixture's application format is independent of router internals: a group ID
is a case-scoped identifier available before the full content is known, not the
hash of a complete message. Metadata describes a bounded set of available/wanted
parts and a revision. Part bytes carry their index and length. Golden fixtures
must agree across C++, Go, Rust and the independent Python decoder. The application
checks contents and reconstructs the expected value; the router must not claim
ordinary full-message validation, signature verification or delivery credit for
a partial callback alone.

The application fixture format is specified by the independent
`partial_fixture.py` and fixed golden tests:

- Group: the 16 bytes of the canonical case token followed by a nonzero big-endian
  uint32 sequence. The default sequence is one.
- Three parts, indices 0-2. An encoded part is version byte `01`, index byte,
  big-endian uint16 length, then 1-256 data bytes. Expected part data is ASCII
  `forge-pr12:<case-token>:part-<index>`.
- Metadata: version byte `01`, nonzero big-endian uint32 revision, a one-byte
  have bitmap and a one-byte want bitmap. Only the three low bits are valid;
  have and want must be disjoint. A newer revision replaces earlier metadata,
  not bitwise-OR accumulation. Same-revision conflicts and older revisions are
  rejected by the fixture application, not the generic router.
- Reconstruction requires three distinct, independently checked parts and
  concatenates them in index order. A callback or claimed completion boolean
  cannot substitute for those actual bytes.

Go uses `WithPartialMessagesExtension`, `OnIncomingRPC`, `OnEmitGossip` and
`PublishPartial`. Its callbacks record bounded observations and enqueue application
work; they do not reenter the router synchronously. Rust uses native
`partial_messages`, `subscribe_partial`, `publish_partial`, its application
Metadata/Partial hooks and `Event::Partial`. Record metadata hooks explicitly:
metadata-only reception need not produce a public Partial event. Do not claim
arbitrary off-mesh sending if a donor's public API requires prior group state.

## Matrix And Completion

Each capability has four donor directions over native QUIC, native TCP/Yamux and
private TCP/Noise/Yamux. Match transport/security/protocol ownership to each actual
stream. Add mixed-version and negative tests without presenting synthetic raw
frames as native donor behavior. Existing original-Rust QUIC shutdown limitations
and the separately identified local source patch remain visible; neither run
borrows another run's success, cause or completion receipt.

The canonical matrix contains 36 original executions: three capabilities, three
transport profiles and four donor directions. Exactly six original Rust QUIC
cases use the explicit split proof below. The other 30 cases retain the strict
terminal-success gate. Six companion executions do not create extra capability
contracts or replace the original records.

- Original traffic must be proven from four indexed, process-owned Prepare
  snapshots. The application work is drained before acknowledgement; errors,
  trace overflow or incomplete traffic before that boundary invalidate the proof.
- The final captures must preserve the acknowledged trace prefix and come from
  the same actually joined processes. Forced termination is not accepted. The
  three Forge actors must satisfy terminal success. Only the original Rust
  actor's bounded post-acknowledgement shutdown error can remain diagnostic;
  its shutdown verdict stays `NOT_PROVEN`.
- The companion changes only the Rust binary to the explicitly documented
  observation-patched build. It uses separate case tokens, process owners and
  artifact paths, independently proves the same capability and must satisfy
  strict completion for all actors.
- Run-local message IDs, bytes and trace sequences are verified independently,
  not normalized into equality or borrowed between runs. The original failure
  remains in the artifact index together with patch and binary provenance.
