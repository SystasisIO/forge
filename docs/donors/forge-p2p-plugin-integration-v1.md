# Stage 7 P2P Plugin Integration Donors

## Scope And Status

Stage 7 adapts existing node behavior to official local plugin contracts and
configuration. It does not implement another host, protocol loop or wire format.
This source traceability note is not live interoperability evidence. Runtime
acceptance is pending and is tracked in
[the implementation plan](../iterations/forge-p2p-plugin-integration-v1.md).

## Inspected Baselines

- Go libp2p `9cfe2cc00be5b20a0be737f002c99f81b92255c5`:
  `core/host/host.go` and `p2p/host/basic/basic_host.go`.
  The host owns identity, connections, protocol handler selection, new streams
  and its event bus. Handler removal updates protocol availability rather than
  constituting an application-wide cancellation API.
- Rust libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
  `protocols/gossipsub/src/behaviour.rs`, including `subscribe`,
  `subscribe_partial`, `unsubscribe`, `peer_score` and `publish`.
  Topic operations enter the existing behavior, its message bounds and mesh
  state; the consumer does not implement a second GossipSub maintenance loop.
- Forge Stage 6 merge `94714f0ff465435ef317ec1891d44b1e9380c01e`:
  `libraries/net/p2p/include/forge/net/p2p/node.cppm`,
  `provider_registration.cppm`, `host_event_subscription.cppm`, and the current
  `plugins/net/p2p/{node,resolver,pubsub,diagnostics}` contracts.
  These existing types and ownership rules are the adapter baseline.

## Accepted And Rejected Patterns

| Donor pattern | Forge integration | Required evidence |
| --- | --- | --- |
| One host supplies authenticated connectivity and protocol routing | Node plugin owns one node; local role-specific contracts delegate to it | Raw/plugin parity and no plugin network workers |
| Protocol registration and stream creation are distinct from host shutdown | Owned custom protocol admission, generation isolation and existing node streams | Replacement, built-in collisions, pre-start close and existing-stream survival |
| Host events are emitted by the host | Diagnostics forwards the existing typed subscription | Initial snapshot, overflow/resync, close and node shutdown |
| GossipSub behavior owns mesh, bounds and validation interaction | PubSub adapts full/partial operations to existing library callbacks and tokens | Full/partial lifecycle, topic policy, stale-token and cancellation tests |
| A facade may select capabilities without redefining protocols | DHT API delegates by profile and returns native provider registrations | Profile isolation, value/provider operations, persistence and shutdown |

The Forge local API boundary is intentionally narrower than Go's complete
`Host`: consumers do not receive mutable peer stores, transport managers or a
method to close the shared node. This is a local ownership decision, not a
claim that an in-process C++ interface is a security sandbox.

Do not equate removing a raw protocol handler with cancelling API-publication
sessions. API publications have separately established application ownership.
Do not serialize validators, connection gates or callbacks into YAML; contribute
typed code before startup and reuse node-side validation.

## PubSub Adapter Ownership

The adapter multiplexes consumer callbacks over one native topic registration.
Its per-topic transition gate and sticky completion notification reuse
`forge_asio`; they are not a second GossipSub runtime. Pending subscriptions
count toward limits, failed mutating joins are compensated, and shutdown joins
admitted operations and callbacks before releasing their captures.

Full-topic unsubscribe after node admission closes performs local cleanup only.
It does not send PRUNE or promise completion of already admitted callbacks;
the native lifecycle still owns that join. Partial registration tokens remain
generation-scoped and fail closed after owner shutdown. This local adapter
cleanup contract does not change libp2p subscription frames or donor behavior.

Cancellation filters are isolated inside necessarily awaited child operations
where cleanup must suppress cancellation. They must not leak into the caller's
next operation. The focused tests include caller cancellation after self-leave,
mutating join failure, native stop races and callback-capture destruction.

The native exact-token Partial downgrade preserves the full topic and sends
an ordinary subscribed update with no Partial flags. It introduces no wire
extension or extra maintenance loop. A failing recipient does not prevent
independent recipients from receiving the update; the first peer-local error
remains observable after the attempts. Owner cancellation uses the existing
stream cancellation bridge and joins actual writes. Local retirement is not
rolled back after a write error, and this operation does not claim a remote
acknowledgement or completion of already admitted application callbacks.

Focused native regressions cover preservation of full-message delivery, mesh,
cache and callback accounting, a reset at the first of two recipients, and
caller/node cancellation while Yamux send credit blocks a write. These local
tests do not replace the Stage 7 configured-path donor checks.

## Evidence Boundaries

No new donor compatibility claim follows from compiling a plugin adapter. The
affected configured paths must be exercised through the plugin and the existing
live donor fixtures. The original Rust QUIC shutdown limitation from
[PR12](forge-net-p2p-rust-quic-shutdown-v1.md) remains `NOT_PROVEN`; the approved
local observation patch and its results remain explicitly separate.

Spine integration is a later discussion. It is not part of Stage 7 acceptance,
and eventual product feedback cannot replace Stage 8 production proof.
