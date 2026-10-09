# GossipSub Extensions

## Pinned References

- libp2p specs `6b6203ee6f62938ce67efdb33498173f475851c0`:
  `pubsub/gossipsub/gossipsub-v1.2.md`, `gossipsub-v1.3.md`,
  `partial-messages.md`, and `extensions/extensions.proto`.
- Go PubSub `0ed6f6fdad7eb820486892336bed3081f8fa7f25`:
  `gossipsub.go`, `extensions.go`, `gossipsub_feat.go`,
  `partialmessages/partialmsgs.go` and their focused tests.
- Rust libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
  `protocols/gossipsub/src/behaviour.rs`, `config.rs`,
  `extensions/partial_messages.rs` and the PubSub protobuf definitions.

Donor checkouts are read-only references. Live fixtures use the pinned source
exports and actual negotiated streams. Codec tests alone do not establish
runtime support or donor interoperability.

The pinned Rust `ConfigBuilder::protocol_id` custom-version enum only contains
v1.0/v1.1. Assigning a v1.2/v1.3 string through that setter would falsely label
v1.1 router behavior. New scenarios must use the donor's native default protocol
set, restrict the counterpart's genuine supported versions, and assert the
actually negotiated protocol. Do not patch the router or rewrite protocol names
in observations. Existing forced v1.0/v1.1 fixtures keep their valid custom-ID
configuration.

## Accepted And Rejected Patterns

IDONTWANT carries opaque message IDs, not text. It is optional on both sides,
with bounded per-peer state and heartbeat expiry. A sender is not penalized
merely for delivering a message after IDONTWANT. Suppression must be checked
after outbound waits, before native write; filtering peers before enqueue alone
does not satisfy the queued-duplicate case. Size thresholds and expiry defaults
are operational policy and need not match both donors numerically.

Version 1.3 advertises extension characteristics in the first RPC of each new
stream. Absence means no advertised extension support; unknown fields grant no
capability. A new stream does not inherit the old stream's advertisement state.
The runtime, not the protobuf codec, enforces first-RPC placement and topic flags.
The codec preserves proto2 presence, false/empty values and singular-message
merge semantics, including aggregate bounds across repeated occurrences.

Partial Messages remain explicitly enabled application cooperation. Adopt the
Go-style callbacks and bounded group advertisements, not Rust-specific generic
reconstruction traits. The application defines group identity, part encoding,
metadata interpretation, semantic verification and reconstruction. Forge owns
authenticated transport attribution, routing eligibility and resource bounds.
Transport authentication is not proof of authorship for partial content.

The application must be able to reply to non-mesh peers and send eager parts.
Requests for partial data imply support for sending it. Metadata requires topic
support; bodies additionally require a remote request. The library must not
silently discard a supplied body to pretend that an unsupported send succeeded.
Full-message signatures and reception remain unchanged for mixed networks.

The existing heartbeat selects eligible off-mesh peers for group gossip even
when no full messages are cached. Local group advertisements have explicit
refresh/forget and TTL bounds; no network-triggered unbounded reconstruction
cache is introduced. Callbacks run outside node locks and stream gates and
remain owned until their actual completion after cooperative cancellation.

## Evidence Status

The PR12 implementation and exact-head acceptance are in progress. Native codec
fixtures and the independent Python wire inspector are a first layer, not a
live compatibility verdict. Runtime, mixed-version and adversarial exchanges
must pass before the donor capability manifest is promoted.

The inherited [Rust QUIC shutdown limitation](forge-net-p2p-rust-quic-shutdown-v1.md)
is unchanged: original traffic and separately patched local shutdown evidence
are distinct. No GossipSub donor behavior is patched to make a test pass, and
no upstream patch is submitted as part of this work.
