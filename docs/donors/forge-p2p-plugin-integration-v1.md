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

## Configuration Composition Baseline

Kubo `e931b379e7dc9a74d62e005097e7b9601484b5dc`, `config/swarm.go`,
composes `RelayClient`, `RelayService`, `Transports`, `ConnMgr` and `ResourceMgr`.
Optional relay/resource settings retain the host defaults when omitted.
Pinned Go's `config/config.go` similarly composes the connection/resource
managers, transports and service options instead of implementing their runtime
inside the configuration layer. Forge adopts that ownership pattern, not every
Kubo deployment feature or knob: unsupported WebSocket, UPnP and alternative
browser transports do not become configurable by analogy.

Forge keeps themed records in one `node.config` module, sparse overrides for
native bounds, and explicit typed contributions for application callbacks.
Native validation remains authoritative. Mapping, rejection/rollback, restart
and configured wire behavior are separate acceptance requirements; a large
schema or a donor field-name match proves none of them.

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

## Relay Reservation Admission

Pinned Go `9cfe2cc0`, `p2p/protocol/circuitv2/relay/relay.go`, rejects CONNECT
when the destination has no current reservation. The pinned Circuit Relay v2
specification also describes a deprecated active-relay mode for controlled
environments. It is not a proven Forge deployment profile and must not be
silently exposed by Stage 7 configuration. Forge's supported service requires
a live reservation, including the post-STOP identity/session/expiry recheck.
This source decision is separate from the configured positive, expired and
unreserved-target runtime evidence.

## TCP And QUIC Configuration

At pinned Go `9cfe2cc0`, `defaults.go` composes TLS and Noise as stream-security
protocols independently of the QUIC transport. `p2p/transport/quic/transport.go`
uses libp2p TLS identity for QUIC. Rust `22fb4c78`,
`transports/quic/src/config.rs`, similarly owns its QUIC configuration separately
from TCP security upgrades. The plugin therefore separates `tcp.security` from
`quic` limits rather than inventing a uniform enforcement claim across transports.

Forge's existing `quic_profile` passes transport limits to QUIC. Enforcement is
in `engine_listener_impl`, `quic_engine_connector`, `quic_engine_connection`,
`quic_engine_stream` and `engine_connection_impl`. Connection limits are local
to a listener, outbound queued data belongs to a connection, and inbound queue
bounds count UDP packets before processing. They are not interchangeable with
Yamux windows or node-wide resource reservations. The native transport
`max_frame_size` field has no enforcement in this path. The separate framed-stream
API takes its own independent codec options, so neither is exposed as a P2P
configuration setting. No framing or wire change is made. Local stream opens
check active-stream count; incoming bidirectional credit is separately advertised.
These are not a combined inbound/outbound ceiling, and unidirectional credit is
separate. Native validation must reject stream credit above the QUIC `2^60` limit
before it reaches ngtcp2 transport-parameter encoding.

The adapter audit also reproduced an existing local-admission race: four opens
waiting for remote credit could all allocate after `MAX_STREAMS`, leaving five
active streams under a local limit of two. The native loop now rechecks local
admission after every credit wait, before allocation on the connection strand.
The regression uses authenticated QUIC, observes all four waiters, and sends a
real ngtcp2 credit extension without changing either configured limit. It fails
on the previous implementation and passes after the fix, including reset and
joined cleanup. This is native Forge evidence, not a new Go/Rust interop claim.

Required adapter evidence includes real authenticated TCP TLS and Noise
exchanges, disjoint-security rejection, QUIC operation under TCP Noise-only
selection, native stream/queue rejection and cleanup, atomic config rejection
and startup freeze. Donor source inspection and local Forge pairs are separate
from the final configured-path Go/Rust gate.

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

## Private-Network Secret Provisioning

Inspected Go `9cfe2cc00be5b20a0be737f002c99f81b92255c5`:
`p2p/net/pnet/protector.go` requires a 32-byte PSK;
`p2p/net/upgrader/upgrader.go` applies protection before security and muxer
negotiation; `defaults.go` and `p2p/transport/quic/transport.go` distinguish
private TCP defaults from unsupported private QUIC composition. Rust
`22fb4c784fc55ad8b15d05fdc9f98d663107d4cb` uses the same 32-byte key and 24-byte
nonce/XSalsa20 protection in `transports/pnet/src/lib.rs`; its smoke fixture
composes pnet, Noise and Yamux. Donor key Debug/Display behavior is not imported.

The plugin uses these already implemented native mechanisms unchanged. Secrets
supplies decoded bytes for `p2p.private-network.psk`; permission checks and source
decoding stay in Secrets. The returned vector moves into `secret_bytes`, and the
existing native `pre_shared_key` makes its own secure copy. This is not a claim
of entirely zero-copy provisioning. The declarative snapshot contains only the
reference/egress policy, not the provisioned protector. Freeze, setup accounting
and shutdown joining reuse the existing plugin lifecycle.

The private-profile QUIC/relay/DCUtR refusal is Forge's approved composition
boundary. Native preflight also checks configured Rendezvous points: previously
a QUIC point could pass configuration and fail only when discovery dialed it.
The fix uses the same native TCP check as other configured endpoints, including
when discovery is disabled. Public QUIC Rendezvous remains valid.

Configured Forge pairs and deterministic held-lookup regressions test the
adapter boundary. They are not new configured-plugin Go/Rust evidence. No donor
source or libp2p wire format is modified by this checkpoint.

## DNS, Dial And Path Configuration

The DNS/dial checkpoint reuses native owners rather than introducing another
resolver or scheduler. Reviewed Go `9cfe2cc00be5b20a0be737f002c99f81b92255c5`:
`p2p/net/swarm/swarm_dial.go`, `swarm.go`, `dial_ranker.go` and
`black_hole_detector.go`. Reviewed Rust
`22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
`transports/dns/src/lib.rs`,
`swarm/src/connection/pool/concurrent_dial.rs`,
`protocols/dcutr/src/behaviour.rs` and `protocols/stream/src/shared.rs`.

Forge retains its documented Go-style batch-before-rank contract and bounded
Happy Eyeballs scheduler. Rust interleaves resolution/dialing and defaults to
eight concurrent root futures; Forge's existing ceiling remains four attempts
per logical dial. These local scheduling differences are not wire changes.
Forge's cancellation/local-rejection feedback remains neutral rather than
copying Go's treatment of cancelled losing dials as failures. Loopback tests
cannot establish public-network black-hole behavior; deterministic native
detector tests cover block/probe/recovery separately.

The canonical plugin config adds sparse resolution/resolver, ranker/detector
and direct-path limits. Numeric conversions are checked once; native pure
validation remains the authority for cross-field bounds, numeric nameservers,
IPv6 scope and scheduling ceilings. Private TCP still disables the UDP
detector in its native scheduler policy. No callbacks or per-call deadlines are
serialized into YAML.

Native path preflight now rejects a configured direct budget above the existing
scheduler request ceiling, instead of accepting it and failing on a later
protocol open. Both the native node and plugin remote-API policy receive the
same configured budget. Explicit connect keeps its per-call options; this is
not a new global attempt limit or a reason to alter donor protocol negotiation.

The new configured no-reply regression initially returned P2P `internal` (15)
instead of `canceled` (13) on both local disk drivers. Cancellation already
reached the DNS operation, but the expander's catch-all erased Asio's
`operation_aborted`. The native error boundary now maps that exact code to
`canceled`; other system errors remain `internal`. A/AAAA/TXT tests distinguish
abort from connection-reset, and the original configured expectation is not
weakened. Resolver ownership, query cleanup and shutdown mechanics are unchanged.

Configured DNS tests use the existing local DNS service and real Secrets/
ObjectDB plugin composition. They are separate from Go/Rust process evidence;
no donor support state is promoted by schema or local adapter tests.

## Evidence Boundaries

### PubSub Deployment Policy

Native scoring, validation retry, IDONTWANT and Partial Messages keep their
existing protocol owners. The plugin maps bounded deployment settings and
calls the native validators; it does not duplicate heartbeat or mesh logic.
Scoring is opt-in, with named topic policies and pure numeric settings. Consumer
callbacks and algorithmic mesh/history/gossip tuning remain available through
the authoritative typed native snapshot rather than an exhaustive YAML mirror.

Pinned Go pubsub `0ed6f6fd`, `pubsub.go`, sets `DefaultMaxMessageSize` to 1 MiB.
Pinned Rust `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`,
`protocols/gossipsub/src/config.rs`, defaults `max_transmit_size` to 65536 bytes.
These are deployment policies, not a universal negotiated GossipSub maximum.
Forge preserves the existing 1 MiB consumer payload bound and its extra
1024-byte message envelope; the configured RPC limit now also covers protobuf
framing instead of failing to carry its own envelope. Explicit smaller limits
fail configuration. Products still need compatible peer limits. A large
configured Forge pair is not evidence of a stock donor accepting the same size.

### Configured Fixture Identity And Observability

The configured-plugin process uses the same Ed25519 libp2p identity generator
as the existing raw-node fixture, extracted into a shared test-only pair. The
identity still passes through Secrets and the official application shell; it
does not bypass configured TLS/Noise or ObjectDB ownership. The pinned Rust
fixture enables Ed25519 identities, but not the optional RSA identity feature.
This lane therefore does not claim RSA interoperability. The existing RSA
unit/runtime fixtures remain unchanged.

For public TCP, the unchanged Go listener uses its existing `echo` scenario
because that scenario publishes the completed connection/stream trace. The
challenge remains the same 192 KiB binary payload. Go's `echo_large` dialer
provides actual connection state, application stream identity and same-connection
automatic Identify evidence, not an upgrade-phase trace; the checker verifies
those facts against the configured Forge receipt rather than inventing phases.
Ready addresses must contain exactly one matching peer ID. Duplicate or foreign
suffixes, conflicting socket/identity facts and incomplete shutdown receipts
fail acceptance. Original failed development-run artifacts are retained.

The initial `plugin-node` matrix covers configured public/private transport,
identity and DNS paths. It is not the whole Stage 7 services/policies gate and
does not promote the production-support inventory.

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

### Rendezvous Shutdown Completion

Pinned Rust `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`,
`protocols/rendezvous/src/server.rs`, handles `Message::Unregister` by removing
the authenticated peer's registration and returning no application response.
`protocols/request-response/src/handler.rs` then closes the stream in its
`ResponseOmission` path. The request is length-delimited; the server does not
need the client's FIN to decode it. Forge must not invent an unregister ACK.

The configured shutdown regression exposed a gap between locally completing
the UNREGISTER write and the server processing it. The correction keeps the
existing bounded operation and stream resource owner alive until normal remote
FIN, then closes locally. Reset, connection loss, cancellation, timeout and
unexpected response data are not successful completion. Additive typed EOF
exceptions retain the existing transport error categories and numeric codes;
they distinguish normal remote FIN from those failures without parsing text.

This is an orderly-completion barrier, not a transactional remote durability
guarantee: a failed or malicious peer can leave a registration until its TTL.
The existing raw `rendezvous_lifecycle` donor fixture invokes manual unregister
and must not be reported as automatic node-owned shutdown evidence. New focused
regressions and fresh donor/configured acceptance are required before crediting
this correction as verified.

### Rendezvous Write Half-Close Candidate

The subsequent four-case focused Rust run used source head
`0834111f650b2a25d50dc4a80bdd98f86275d43e`. Both Forge-to-Rust cases,
`rendezvous_register_discover` and `rendezvous_lifecycle`, failed; both reverse
Rust-to-Forge cases passed. The failed Forge path reported
`peer_write_rejected` before native FIN submission, at `stream.cpp:69` and
`quic_engine_stream.cpp:185`. Failed artifacts are retained. This is a focused
2/4 result, not full canonical Stage 6 or Stage 7 acceptance. The failure can
precede the new automatic-manager-unregister phase; its successful completion
against Rust is not proven by compilation or the reverse-direction results.

Pinned Rust `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb` documents one new
substream per request in `protocols/request-response/src/lib.rs:25-27`.
Its outbound handler uses `write_request -> close(write) -> read_response`
(`protocols/request-response/src/handler.rs:188-193`). The Rendezvous codec
reads one framed message, not request EOF (`protocols/rendezvous/src/codec.rs:248-257`),
and the server writes its response, closes its write direction and drops the
stream (`protocols/request-response/src/handler.rs:149-157`). UNREGISTER removes
the registration before omitting the response
(`protocols/rendezvous/src/server.rs:324-332`). No application ACK is added.

The Rust QUIC adapter closes only the send half
(`transports/quic/src/connection/stream.rs:79-86`). The fixture lock resolves
Quinn 0.11.11 and quinn-proto 0.11.16. Quinn's receive-half destructor calls
`stop(0)` unless EOF was consumed (`src/recv_stream.rs:509-535`); quinn-proto
explicitly allows a redundant STOP_SENDING even after data/FIN arrival
(`src/connection/streams/recv.rs:89-102`). Stopping reception before EOF is
permitted by [RFC 9000 section 3.5](https://www.rfc-editor.org/rfc/rfc9000.html#section-3.5).
The STOP code is application-defined, not a transport NO_ERROR or Rendezvous
success status ([section 20.2](https://www.rfc-editor.org/rfc/rfc9000.html#section-20.2)).
Code zero alone cannot distinguish normal stream disposal from early rejection.

The correction adds `async_close_write` for register, discover
and unregister before response/remote-EOF reads. Its required contract is a
write-only FIN, serialized by the caller with writes and terminal close, with
the read direction, cancellation and P2P resource reservation retained until
terminal cleanup. QUIC and Yamux are the intended supported models; other
transport models default to typed `transport::exceptions::unsupported_protocol`.
QUIC success must prove actual FIN submission and send-prefix drain, not merely
successful terminal cleanup after prior STOP/rejection/reset. Once that FIN
prefix has drained, a late STOP cannot revoke the write result. An incoming
RESET_STREAM remains a typed read failure, not a failure of the independent
write direction. A live stream's pending cancellation request joins the existing
owner-strand reset/drain path before FIN admission; normal native-close
notifications already mark the stream closed and do not invent cancellation.
Early write half-close removes the
read-response-before-FIN ordering gap, but does not exclude STOP racing FIN
submission after the server decodes the complete request. Native correctness,
focused regressions and fresh live validation remain pending. No donor patch,
new ACK, retry or relaxed acceptance is part of this candidate; the original
Rust whole-host QUIC shutdown limitation remains `NOT_PROVEN`.
