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
- Go libp2p PubSub `0ed6f6fdad7eb820486892336bed3081f8fa7f25`:
  `subscription.go` and `topic.go` (`Subscribe`, `Close`).
  Multiple consumer subscriptions share one topic; the last cancellation
  removes the network subscription. Cancellation enters the existing PubSub
  owner instead of starting an independent network worker.
- Forge Stage 6 merge `94714f0ff465435ef317ec1891d44b1e9380c01e`:
  `libraries/net/p2p/include/forge/net/p2p/node.cppm`,
  `provider_registration.cppm`, `host_event_subscription.cppm`, and the current
  `plugins/net/p2p/{node,resolver,pubsub,diagnostics}` contracts at that commit.
  That four-plugin layout is the historical adapter baseline, not the new
  package surface: PubSub and diagnostics now belong to node; resolver stays
  separate.

## Accepted And Rejected Patterns

| Donor pattern | Forge integration | Required evidence |
| --- | --- | --- |
| One host supplies authenticated connectivity and protocol routing | Node plugin owns one node; local role-specific contracts delegate to it | Raw/plugin parity and no plugin network workers |
| Protocol registration and stream creation are distinct from host shutdown | Owned custom protocol admission, generation isolation and existing node streams | Replacement, built-in collisions, pre-start close and existing-stream survival |
| Host events are emitted by the host | Node events API forwards the existing typed subscription | Initial snapshot, overflow/resync, close and node shutdown |
| GossipSub behavior owns mesh, bounds and validation interaction | PubSub adapts full/partial operations to existing library callbacks and tokens | Full/partial lifecycle, topic policy, stale-token and cancellation tests |
| A facade may select capabilities without redefining protocols | DHT API delegates by profile and returns native provider registrations | Profile isolation, value/provider operations, persistence and shutdown |

The Forge local API boundary is intentionally narrower than Go's complete
`Host`: consumers do not receive mutable peer stores, transport managers or a
method to close the shared node. This is a local ownership decision, not a
claim that an in-process C++ interface is a security sandbox.

## Connection And Identify Configuration

The next declarative slice reuses existing Forge node policies. At the pinned
Go baseline, `p2p/net/connmgr/options.go` owns grace/silence configuration:
zero grace is allowed, whereas silence must be positive. The pruning selection
and protected-peer exclusion belong to `p2p/net/connmgr/connmgr.go`, not to the
host's configuration adapter. Forge likewise maps session bounds and backoff
into its existing connection manager; no plugin pruning worker is introduced.

Go `p2p/protocol/identify/opts.go` owns protocol/user-agent strings and interaction
timeout. `id.go` sets the 5-second timeout, 4-KiB own-message target and 10-part
receive bound. Rust `protocols/identify/src/behaviour.rs` owns its protocol and
agent version strings and automatically exchanges them through its behavior.
Forge maps versions and all existing Identify limits into its native service.
These settings do not replace authenticated peer identity or signed-record
validation. A configurable bound is not a claim that every value interoperates
with every donor's fixed receive limit.

Peer-state bounds configure Forge's existing bounded operational store and its
queue/hydration/pruning limits. Persistence remains plugin-owned and the private
cache schema does not change. Raw node validation remains authoritative for all
three groups. Required configured-path evidence includes actual pruning with
protected/tagged peers, remote receipt of version facts, invalid/oversized
Identify isolation and bounded retained peer records. Source mapping alone does
not satisfy those tests or the final Go/Rust acceptance gate.

Do not equate removing a raw protocol handler with cancelling API-publication
sessions. API publications have separately established application ownership.
Do not serialize validators, connection gates or callbacks into YAML; contribute
typed code before startup and reuse node-side validation.

## PubSub Adapter Ownership

The node-owned private PubSub service multiplexes consumer callbacks over one
native topic registration. The standalone PubSub plugin and public source
mediator are removed; the public entry point is node's `pubsub_api`.
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

The official Partial facade reuses native generation tokens. Its required full
fallback participates in the existing full-handler aggregation, while partial
callbacks retain their own admission and cancellation state. A queued enable
reservation is not a native subscription owner: cancellation before acquiring
the transition gate must not prevent the last ordinary owner from leaving.
The deterministic regression checks native leave count, native topic count and
plugin quota reuse, and was also verified to fail with the former predicate.

The private immutable operation table delegates to that same native node.
Deterministic tests substitute fallible operations to prove compensation and
joined cleanup after mutation; this is not a public provider API or another
network runtime. Real configured-plugin fixtures separately exercise network
delivery. Package tests import the installed node PubSub contract directly,
not a private implementation or removed plugin component. Local consolidation
regression results are recorded in the implementation plan; they do not replace
configured-path donor acceptance for the completed Stage 7. None of these checks
claims a new Go/Rust wire implementation.

## Native Diagnostics Delegation

The existing native routing table and GossipSub score engine remain the only
owners of these states. Node `diagnostics_api` 1.0 exposes their native
`dht::routing_status` and `pubsub::score_snapshot` values without a second cache,
score calculation or maintenance loop. This follows the existing host/behavior
inspection boundary, not a new libp2p protocol.

Reads retain the native node and configured diagnostic bounds under the
configuration mutex, then delegate outside it. No external source callback
remains. Admitted synchronous reads may finish while shutdown proceeds;
returned values own their data. After node retirement, new reads fail with
the initialization error.

The updated regression plan checks routing counts after a real DHT query,
a nonzero native application-weighted score, retained values across shutdown,
and concurrent native reads. Obsolete source-callback barriers cannot establish
this new direct ownership and are replaced rather than kept as fake evidence.
The TCP fixture authenticates certificate identities but uses test-only memory
persistence and dependency doubles. It is not configured production persistence
or donor-network evidence, and publishing for score sampling does not prove
delivery to a remote subscriber.

## Custom Protocol Ownership

The local `node::protocol_api` separates a move-only raw registration from API
publication ownership. Go `core/host/host.go` and
`p2p/host/basic/basic_host.go` distinguish removing a stream handler from
closing existing streams. Forge preserves this behavior: unregister closes
admission and updates Identify availability; admitted raw streams retain the
handler generation they entered. API-publication sessions retain their existing
application-level cancellation and drain semantics.

One private registry arbitrates raw/API names, configured DHT profiles and all
built-in protocol names. Replacement requires the exact active raw owner;
closing a superseded token cannot remove a newer route. Pre-start claims count
toward the configured bound. A failed native mutation retains the name claim
until a normal unregister confirms absence, including a false return when the
handler was already removed. Configuration candidates publish neither native
options nor reservations if parsing or name reservation fails.

The private `protocol_operations` boundary delegates directly to native node
registration/removal in production. Deterministic tests replace only those two
fallible calls and exercise the real registry/slot implementation, including
errors before/after table mutation and attach/close/stop barriers. These are not
claims of reproducing a native Identify Push allocator failure. Separate real
TCP/TLS/Yamux tests cover authenticated stream creation, generation replacement,
rejected new opens, admitted-stream survival and startup after rejected config.

## Peer Operations And Contributions

Connect and Ping delegate to the existing native node, including expected-peer
authentication and operation cancellation. The plugin does not introduce a
transport, probe loop or identity check. Ordinary secure plugin fixtures use
TLS/Yamux with Secrets and ObjectDB through both supported macOS backends;
insecure memory fixtures are not accepted as authentication evidence.

Peer protection remains distinct from numeric pruning preference, as in Go's
connection-manager ownership model. The plugin supplies bounded unique keys
and move-only tokens instead of exposing global keyed removal to consumers.
Native tag aggregation and pruning remain authoritative. Separate positive and
negative subtotals prevent overflow under any token release order. Native
removal precedes quota release, and shutdown retires only plugin-owned keys.
This changes no libp2p wire semantics or background maintenance.

Token lifetime and native connection-manager tests are not evidence that the
configured plugin actually prunes the expected peer. That integration case
remains an explicit policy-mapping acceptance gate.

## Policy Preflight

The plugin must validate native policy before reading identity secrets or opening
its P2P persistence adapters. The new native `validate_policy(node::options)`
separates policy checks from resource presence, without synthetic certificates,
keys, persistence or permissive callbacks. Full native validation retains
production resource requirements and node construction owns key parsing.

QUIC bounds reuse its existing client/server validator through the existing P2P
QUIC profile mapping. Availability follows the actual direct registry: public
nodes register QUIC independently of the capability bit; private nodes do not.
No transport negotiation or libp2p wire behavior changes in this refactoring.
Tests distinguish absent resources, zero-denial resource scopes, legacy
normalization, callback presence without invocation, and QUIC limit boundaries.

## Configuration Ownership

### Sparse Resource Limits

At Go libp2p `9cfe2cc00be5b20a0be737f002c99f81b92255c5`,
`p2p/host/resource-manager/limit_defaults.go` separates absent/default values
from explicit denial: `LimitVal.UnmarshalJSON` converts an explicit numeric
zero into `BlockAllLimit`; `ResourceLimits.Build` and `PartialLimitConfig.Build`
overlay configured dimensions onto concrete defaults. The inspected ranges are
lines 118-175, 297-346 and 447-489. This is configuration semantics, not a
new wire protocol or a claim of identical numerical defaults.

The node plugin uses optional budget fields for the same distinction. It maps
native defaults, derived session-admission defaults and explicit resource
patches in that order. Empty scopes must not replace finite native limits with
the standalone native scope type's unlimited member defaults. An explicit zero
must reach the existing native resource manager unchanged. Configuration
mapping owns no reservations, counters or network workers.

Required adapter evidence includes all scope/dimension mappings, untouched
defaults, explicit denial, checked narrowing, nested unknown-field rejection
and rejection without configuration mutation. A configured secure-capable TCP
dial denied before authentication is resource-admission evidence, not proof of
an authenticated connection or donor exchange.

### Frozen Contributions

Local composition installs native gater, DHT value-policy and PubSub callback
objects. The
plugin owns an immutable candidate snapshot and freezes complete policy before
startup I/O, rather than introducing network policy in its adapter. Native DHT
declaration validation is shared with full validation; only the latter accepts
a profile for runtime use. Amino policy cannot be replaced by a contribution.

Existing Forge notification epochs provide the sticky setup-completion barrier.
The node plugin follows the existing PubSub owned-child shutdown pattern, with
stop initiation itself inside cancellation isolation: retiring application
callbacks can synchronously cancel the caller. No new scheduler, task tracker
or wire-level cancellation scheme is introduced. Configuration callback copies
and destruction occur outside the configuration mutex, and stale candidates
cannot replace concurrent contributions.

Diagnostic bounds are the only post-start mutable configuration. Updating them
requires structural equality of all other declarative fields; mixed policy
updates fail atomically. Typed contributions remain retained. Serializing
configuration to compare it would invoke unrelated conversion policy and is
not used.

Focused tests exercise real gater delegation and real Secrets/ObjectDB startup
preflight. Insecure gater control fixtures do not prove identity, and source
reviews do not replace the still-required configured donor exchanges.

## DNS Fixture Isolation And Scheduling Boundary

Two existing DHT fixtures advertised synthetic `example.com` addresses without
configuring their resolver. They now use the existing joined local DNS fixture
with explicit NXDOMAIN responses on every participating node. Request names are
recorded; the original topology, provider count, assertions and deadlines stay
unchanged. Go's pinned `p2p/net/swarm/resolve_test.go:14-24` similarly controls
resolution of synthetic names rather than depending on public DNS.

At Go `9cfe2cc00be5b20a0be737f002c99f81b92255c5`,
`p2p/net/swarm/dial_worker.go:180-193` calls `addrsForDial` before ranking;
`swarm_dial.go:293-314` completes the resolver pass first. Forge's documented
batch-before-dial contract follows that order. A stalled DNS sibling can delay
a known numeric address until resolution completes or the logical deadline
expires. Stage 7 does not claim to remove this limitation.

Rust `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb` differs:
`transports/dns/src/lib.rs:219-301` dials concrete candidates within each root,
while `swarm/src/connection/pool/concurrent_dial.rs:54-100` bounds concurrent
root futures. These are different scheduling policies, not different wire
protocols. Immediate numeric dialing during pending DNS would require a
separately scoped change preserving global budgets, peer admission, root
provenance and joined cleanup; it is not inferred from donor compatibility.

An experimental immediate-numeric test failed in both root orders and its RED
result was retained. It tested that additional behavior, not the accepted
batch contract. The retained regressions instead require completion of the
batch before a deduplicated numeric dial and prove that caller cancellation
and scheduler close wait for a held DNS callback, without launching siblings.
They do not weaken existing limits or skip an accepted acceptance gate.

## Evidence Boundaries

The consolidated node checkpoint preserves the donor ownership pattern rather
than adding a protocol loop. A real TLS/Identify/Yamux two-plugin PubSub test
holds the delivered callback and then its capture destructor independently;
awaited node shutdown must wait for both. A separate ObjectDB fixture injects
one durable flush failure and proves shared per-attempt shutdown results and a
successful external retry without dropping persistence ownership. These are
local adapter/lifecycle regressions, not external donor or production storage
acceptance. Failed fixture runs remain recorded rather than counted as PASS.

No new donor compatibility claim follows from compiling a plugin adapter. The
affected configured paths must be exercised through the plugin and the existing
live donor fixtures. The original Rust QUIC shutdown limitation from
[PR12](forge-net-p2p-rust-quic-shutdown-v1.md) remains `NOT_PROVEN`; the approved
local observation patch and its results remain explicitly separate.

Spine integration is a later discussion. It is not part of Stage 7 acceptance,
and eventual product feedback cannot replace Stage 8 production proof.
