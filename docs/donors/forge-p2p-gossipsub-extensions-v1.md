# GossipSub Extensions Donor Traceability

## Scope And Status

Stage 6 PR12 extends the existing GossipSub runtime rather than creating a
second mesh, network loop or content store. This note records inspected sources
and implementation choices, not a final acceptance verdict. The exact-head live
matrix and independent review remain required before merge.

Pinned sources:

- libp2p specs `6b6203ee6f62938ce67efdb33498173f475851c0`:
  `pubsub/gossipsub/gossipsub-v1.2.md`, `gossipsub-v1.3.md`,
  `partial-messages.md` and `extensions/extensions.proto`.
- Go PubSub `0ed6f6fdad7eb820486892336bed3081f8fa7f25`:
  `gossipsub.go`, `gossipsub_feat.go`, `extensions.go`, `pubsub.go`,
  `partialmessages/partialmsgs.go` and their focused tests.
- Rust libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
  `protocols/gossipsub/src/{behaviour,handler,config}.rs`,
  `protocols/gossipsub/src/extensions/partial_messages.rs` and PubSub protobufs.

At this pinned revision v1.3 is a Candidate Recommendation and Partial Messages
is a Working Draft. Compatibility is with this explicit source baseline, not a
promise about unspecified future revisions.

Donor checkouts stay read-only. The pinned Rust `ConfigBuilder::protocol_id`
custom-version enum contains only v1.0/v1.1. Assigning a v1.2/v1.3 string through
that setter would falsely label v1.1 routing behavior. New cases use the native
default protocol set, restrict the counterpart's genuine supported versions
and assert the actually negotiated protocol. Existing forced v1.0/v1.1 cases
retain their valid custom-ID configuration; no observations rewrite protocols.

## Composition

| Donor mechanism | Forge owner | Required proof |
| --- | --- | --- |
| v1.2 IDONTWANT reception, expiry and send suppression | `pubsub_idontwant`, existing inbound/outbound/heartbeat aspects | Limits, expiry, same-message suppression before forwarding, no duplicate-only penalty |
| v1.3 first-stream-RPC advertisement | Existing authenticated inbound/outbound stream generations | First RPC, absence, unknown fields, reconnect, stale generation and fallback |
| Topic requests/support flags | Existing subscription state and outbound admission | Replacement/unsubscribe cannot be undone by delayed announcements |
| Application partial receive/publish hooks | `node` partial operations and `pubsub_partial` | Real off-mesh metadata/request/part exchange and unchanged full-message fallback |
| Partial gossip with bounded application state | Existing heartbeat and router selection | Local advertisement expiry, busy callbacks, byte bounds and joined cancellation |

Forge retains standard field numbers and presence semantics. Partial body and
metadata are optional byte fields: absent and present-empty are distinct.
Requesting parts implies sending support. Sending support without requests
allows metadata only, not even an empty-but-present data body.
For subscriptions, the wire default of an omitted `subscribe` is false; the
public C++ construction default is not a protobuf default. Go `pubsub.go`
derives sending support as `requestsPartial || supportsSendingPartial`, matching
the pinned protobuf comment. Forge follows that implication without rewriting
the decoded presence flags. Pinned Rust stores the two received flags literally;
ordinary native Rust subscriptions emit both. Do not claim the omitted-support
edge as a demonstrated Rust behavior.
The protobuf decoder preserves singular-message merge semantics and aggregate
bounds across repeated occurrences. Opaque IDONTWANT IDs are not text. Their
suppression is rechecked after outbound waits, immediately before native write;
pre-enqueue filtering alone is insufficient. Donor size/expiry defaults can
legitimately differ without changing protocol semantics.

Go supplies callback-based partial actions; Rust supplies typed metadata/partial
traits and actions. Forge uses existing Asio awaitables and node-owned tracking,
with immutable registration tokens and cooperative cancellation. No vendor
runtime types escape the Forge API. The application owns reconstruction and
semantic validation; the library does not guess a part format or promote partial
data to signed full-message delivery.
Off-mesh replies and eager parts are first-class paths. Unsupported body sends
must fail explicitly, not silently discard the supplied body and report success.
Heartbeat group gossip works without a full-message cache. Full-message
signatures and ordinary reception remain unchanged for mixed networks.

Go delayed subscription announcements reconstruct current topic flags. Forge
must likewise validate canonical subscription state at actual write admission,
not merely when a multi-peer announce operation starts. The spec's per-stream
first-RPC rule is authoritative; a donor's more permissive peer-level state is
not a reason to let a replacement stream inherit stale capabilities.

Local advertisement count/byte/TTL limits, callback admission and registration
generations are Forge ownership mechanisms. They reuse existing lifecycle,
resource and write-gate components. They do not add wire fields or require the
remote implementation to use identical operational defaults.

## Evidence Boundary

Unit fixtures check protobuf layout, malformed input, capability combinations,
replacement, callback failure, backpressure, cancellation and shutdown. Native
fixtures observe actual framed writes/reads; synthetic decoder tests alone are
not interoperability evidence.

The live matrix comprises IDONTWANT, v1.3 advertisement and Partial Messages over
native QUIC, native TCP/Yamux and private TCP/Yamux, in both Forge/Go and
Forge/Rust directions. The partial consumer starts without the group; it must
learn metadata, request missing parts and reconstruct from actual received bytes
over a non-mesh edge. Full-message fallback is verified independently.

The fixture's small deterministic part encoding is application test data, not a
new Forge wire protocol or a standard libp2p part format. Process identity,
negotiated protocol, transport ownership, immutable receipts, application event
references and actual task completion are checked separately.

The [local Rust QUIC source patch](forge-net-p2p-rust-quic-shutdown-v1.md)
only exposes a native error cause for observation. It is not submitted upstream.
Original traffic and patched shutdown runs retain separate binaries, source
hashes and results. Original Rust QUIC shutdown remains `NOT_PROVEN`; neither
the old PR11 receipt nor another process's completion proves PR12 shutdown.
