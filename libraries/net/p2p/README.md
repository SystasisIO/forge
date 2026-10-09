# forge_net_p2p

`forge_net_p2p` is the peer-to-peer layer above transport sessions: peer identities,
sessions, protocol stream negotiation, peer exchange, relay reservations,
reachability probes, hole punching, path scoring, discovery protocol machinery
and GossipSub/pubsub.

API status: the `forge_net_p2p` P2P surface is Preview under the approved Stage
6 assumption. `forge.net.p2p.resource_manager` remains Preview while P2P
production hardening replaces manual counters with move-only reservations. The
Stage 3 migration intentionally removes `try_acquire_*`/`release_*`; callers
retain the returned reservation for the complete operation lifetime instead.
Staged stream scope binding returns an explicit result: only `policy_rejected`
is backpressure; `invalid_transition` and `runtime_failure` are internal
failures.

`forge.net.p2p.dialing` is Preview. The node-owned dial scheduler integrates
bounded DNS expansion, Happy Eyeballs and black-hole detection for direct dials,
including raw multiaddr, peer-store and bootstrap roots. One logical dial owns
the deadline and bounded concrete attempts; shutdown drains those attempts.
Native/private IPv6 detection remains enabled by default; the TCP-only private
profile disables UDP detection. Local dual-stack node tests and deterministic
detector/scheduler recovery tests are distinct from Go/Rust DNSADDR wire
evidence. Final-head acceptance remains pending; integration is not a
production-readiness claim.

## Stage 6 Carrier Migration

| Previous surface | Current Preview surface |
| --- | --- |
| `endpoint_record.endpoint` | `endpoint_record.address` as `forge::multiformats::multiaddr` |
| `bootstrap_peer.address` as `endpoint` | `forge::multiformats::multiaddr`; convert with `endpoint.to_multiaddr()` |
| `identify::document.listen_endpoints` as `vector<endpoint>` | the same field as `vector<forge::multiformats::multiaddr>` |
| DHT peer/provider `endpoints` as `vector<endpoint>` | the same field as `vector<forge::multiformats::multiaddr>` |
| Discovery and Rendezvous `endpoints` as `vector<endpoint>` | the same fields as `vector<forge::multiformats::multiaddr>` |
| Node diagnostics source and diagnostics plugin API major 1 | major 2; node plugin 6.0.0 and diagnostics plugin 2.0.0 |
| Private ObjectDB P2P cache v2 | v3 marker; use `schema-policy: reset` rather than hydration |

No compatibility aliases are provided for these carrier changes.

Bootstrap roots use the node-owned DNS expander and dial scheduler, including
recursive `/dnsaddr` resolution. `lifecycle.listen` remains concrete `endpoint`.
A suffixless root learns a peer for connected-session checks and protection,
but does not pin that identity for later resolutions after disconnect. A
terminal `/p2p/<peer>` remains an explicit identity constraint. Bootstrap
updates validate the complete list (at most 4096 roots) before replacement.
Private nodes accept unresolved `/dnsaddr` roots but reject explicit QUIC and
circuit routes; resolved candidates pass through the existing TCP-only filter.
Plugin YAML endpoint syntax is unchanged: its existing parsed endpoints are
converted with `to_multiaddr()`. Raw DNSADDR YAML configuration is Stage 7 work.

## Private-Network Profile

`forge.net.p2p.private_network::options` composes the product-neutral
`forge_net_pnet` protector into a direct TCP/Yamux node. The byte-stream order is
TCP, pnet, security multistream negotiation, Noise or TLS, then Yamux. This
profile accepts only direct TCP endpoints and disables QUIC, relay reservations,
relay paths and DCUtR. `/pnet` is stream protection, not peer authentication: a
wrong PSK normally becomes a later security-negotiation failure.

The registered live interop contract covers the four direct Forge/Go/Rust
TCP/Yamux directions and separately indexes missing-key and mismatched-key
controls with an observed outbound dial boundary and listener ingress, followed
by rejection before Identify or an application stream. It records PNET negotiation
and the non-secret operational fingerprint on both endpoints. Registration is
not a passing-run claim and does not extend to QUIC, Relay, or DCUtR.

## Current Support State

`node::async_connect_coordinated(endpoint, coordinated_connect_options)` is an
additive explicit direct operation, separate from automatic/manual relay DCUtR.
It requires an expected authenticated peer, a concrete IP source owned by an
active local listener, an explicit security initiator/responder role and one
overall timeout (10 seconds by default). DNS, circuits, ephemeral source fallback
and private-network QUIC are rejected. TCP security role is independent of
connect/accept direction; QUIC responder sends listener-owned probes and waits
for authenticated inbound admission, never treating probes as success.

At most eight operations are active per node. Conflicting local/remote tuples
are rejected rather than coalesced. Each operation leases the actual listener
generation and exact remote/local tuple before native work, and owns one logical
dial permit through native drain and publication. Another session to the same
peer cannot fulfill it. Standard Asio caller cancellation cancels and joins this
operation's native work without stopping the node or its existing relay streams.
Cancellation racing publication does not roll back an already published session.
Host shutdown cancels and joins these owners before session/persistence teardown.
Native regression and donor/live execution evidence remain separate; the API
does not by itself establish passing simultaneous-open/NAT acceptance.

Preview source migration: `node::session_info` now includes the admitted native
owner ID, muxer negotiation facts, delegate roles and circuit ownership. Its
aggregate arity is no longer the old five-field shape. Consumers should read
named members rather than use a five-element structured binding; no legacy
tuple facade is provided. Existing connect operations and libp2p wire formats
are unchanged.

`session_info` and session diagnostics retain optional `security_role`
(`forge::net::tls::endpoint_role`, client/server for TLS or Noise) and `yamux_role`
(`forge::net::yamux::side`). These are recorded only after the respective native
security upgrade and Yamux construction succeed, not from requested roles or
TCP connect/accept direction. Transports without those delegate receipts leave
the fields unset; canceled unpublished upgrades cannot contribute session facts.

Relayed sessions additionally retain their authenticated logical
`circuit_endpoint` and the actual outer `carrier_session_id`. The corresponding
carrier diagnostics contain its native socket endpoints; inner relay sessions
do not fabricate native `local_endpoint`/`remote_endpoint` values. A circuit
address is a route over the carrier, not an independently observed inner socket.

This library contains substantial libp2p-compatible protocol substrate, but it
is not yet a complete autonomous production host. Direct QUIC and TCP/Yamux,
secure peer authentication, node-owned bootstrap, automatic Identify,
session/stream/dial admission and explicitly reserved outbound P2P write memory
are on the normal node path. Managed topology now has one node-owned lifecycle for
bounded DHT, configured Rendezvous and Forge Peer Exchange discovery, while
`static_only` disables autonomous discovery and dialing. GossipSub has bounded
connected-peer mechanics, scoring and live interop fixtures; these do not replace
the later integration and production/hostile/soak gates.

The following surfaces are not production claims yet:

- Kademlia now provides isolated Amino and product profiles, bounded node-owned
  k-buckets, autonomous routing refresh, durable validated values, owned
  provider registration and `/pk`/`/ipns` interoperability. Managed topology
  consumes peer-capable profiles without creating a second routing refresh;
- Rendezvous and the Forge-specific Peer Exchange feed the same bounded
  topology manager. Donor/live evidence remains classified separately in the
  inventory and must not be inferred from lifecycle activation alone;
- Rendezvous currently has registered Forge/Rust directions only. Go is an
  explicit limitation because no official Go rendezvous behaviour donor is
  pinned; this is not a claim of Go compatibility;
- the Go/Rust Ping wire protocol is current; bounded periodic Ping health
  sampling, AutoNAT v1 node-level reachability and AutoNAT v2 address-level
  evidence remain separate Stage 6 host-local inputs to the managed topology
  score;
- DNSAddr, Happy Eyeballs, native/private IPv6 black-hole detection and native
  UDP black-hole detection are integrated into node-owned dialing; their
  inventory readiness remains `unverified` pending final-head acceptance;
- observed-address confidence/expiry, public mDNS, private fingerprinted mDNS
  remain separately tracked Stage 6 capabilities;
- automatic UPnP router port mapping is unsupported and deferred beyond the
  initial production profile. It does not gate AutoRelay or Stages 7/8;
  direct reachable/manually forwarded addresses and relay/DCUtR paths still
  require their own acceptance evidence;
- AutoRelay owns bounded candidate verification, reservation renewal and relay
  replacement within the node lifecycle. Its execution evidence is tracked
  independently from the still-pending autonomous DCUtR path manager;
- `node::options::connection_gater` is a synchronous, concurrent-callable
  host policy hook. Its five donor-aligned stages run before logical peer
  dial, each concrete address dial, inbound acceptance, authenticated security
  and mux upgrade. A denial closes the native attempt without recording endpoint
  backoff; implementations must be nonblocking and thread-safe;
  relay HOP/STOP transport sessions use the same peer, accept, security and
  upgrade boundaries, but do not invent a direct endpoint where the relay
  control exchange carries none.
- `forge.net.p2p.resource_manager` owns explicit system, transient, peer,
  protocol, service, connection and stream reservations. Its scoped memory and
  file-descriptor limits apply only to allocations and native lifetimes that
  acquire a child reservation; they are not a process-RSS, kernel-buffer or
  complete native-transport-heap limit. TCP listeners and connections reserve
  native file descriptors; QUIC listeners reserve one shared UDP descriptor
  while inbound QUIC connections do not double-charge it. Outbound P2P write
  chunks retain their explicit child memory reservations until transport drain,
  acknowledgement or reset. Full ingress, handshake, decoder and native queue
  memory accounting remains future Stage 6 work;
- GossipSub v1.0/v1.1 scoring and autonomous mesh selection have a native
  implementation and registered acceptance suite. Original Rust QUIC shutdown
  remains `NOT_PROVEN`; explicitly patched shutdown observations have a separate
  scope. PR12 adds v1.2/v1.3 and opt-in Partial Messages, whose final-head live
  acceptance is still pending. Transport topology belongs to the managed
  topology service above.

### GossipSub Scoring (Preview)

`pubsub::options::scoring` enables validated per-peer/per-topic P1-P7 scoring.
The node owns decay, negative-score retention, threshold eligibility, mesh
repair, opportunistic graft and successfully sent IWANT promises. Ignore,
retry, cancellation and local queue pressure are not malicious behavior.
Application score callbacks run outside node and score locks. IP colocation
uses authenticated direct endpoints, never third-party advertised hints or
the relay's address attributed to a remote peer.

`node::pubsub_scores()` returns copied read-only score and actual mesh facts.
The optional synchronous tracer receives completed original framed RPCs and
committed validation/delivery facts outside state locks. Its spans are valid
only during the callback; observers must copy any data they retain, must not
block the network executor and cannot mutate routing through the trace API.

Message history uses heartbeat windows. Local flood publish, unsubscribed
fanout and forwarding through a subscribed mesh are separate paths. PRUNE
encoding follows the negotiated stream: v1.0 excludes v1.1 PX/backoff fields.
IHAVE and IWANT are split incrementally by actual protobuf payload size.
`max_rpc_size` excludes the length prefix, which still counts toward queued
memory. Individually unencodable gossip IDs are skipped without hiding durable
GRAFT/PRUNE intents or creating an IWANT promise for unsent data.
Remote ephemeral replies retain their original authenticated session; a retired
owner cannot penalize a replacement peer lifetime or create its delivery promise.
Cached MESSAGE replies use individually bounded RPCs, even when several matching
entries would exceed the payload limit in a combined response.
For `codec::next_gossip()`, start with a default cursor and keep the input control
immutable until traversal ends. An out-of-range
cursor is rejected with `invalid_options`, rather than permitting a stalled loop.

`max_graft_per_peer` limits outbound GRAFT entries in one durable batch, not
inbound processing. Remaining GRAFT intents stay queued; PRUNE is selected first.
Inbound GRAFT entries all pass the existing topic, score, backoff and mesh checks,
subject to the RPC byte/count bounds. Callers that previously relied on this
field as an inbound cap must use those receive/resource limits instead.

The scoring acceptance target is `test_forge_p2p_pubsub_acceptance`; codec,
score-engine or synthetic checker tests alone do not establish interoperability.
See the [donor traceability note](../../../docs/donors/forge-p2p-gossipsub-scoring-v1.md)
for composition, Rust score-counter visibility and the unresolved typed QUIC
shutdown-cause observation gate. Unavailable native causes are not promoted to
successful shutdown using diagnostic strings, another stream's cause or retries.

### GossipSub Extensions (Preview)

`pubsub::options::preferred` can select v1.2 or v1.3, retaining the default v1.1.
The standard protocol IDs are `/meshsub/1.2.0` and `/meshsub/1.3.0`; lower-version
peers use the existing bounded protocol fallback. v1.2 IDONTWANT suppresses
redundant sends with per-peer/node count, byte and heartbeat bounds. It is not a
delivery acknowledgement, and a duplicate after IDONTWANT is not by itself a
scoring offence.

v1.3 advertises enabled extensions in the first RPC of each stream generation.
Unknown extensions are ignored. A new stream must establish its own facts; it
does not inherit an old stream's advertisement. Global extension support and
per-topic partial-message flags are separate.

Partial Messages requires `pubsub::options::partial_messages = true`, v1.3 and
the partial `node::async_subscribe` overload. Its `partial_options` supplies both
receive and gossip callbacks. `requests_partial` defaults to false: this allows
metadata exchange and sending parts while still requesting ordinary full
messages. Only peers requesting parts may receive a partial data body. An absent
body differs from a present empty body on the wire.

The returned `partial_topic` is a non-owning registration token. It binds
`async_send_partial`, `async_partial_peers`, `async_advertise_partial`,
`async_forget_partial` and scoped unsubscribe to the same node/topic generation.
Copying or destroying it does not unsubscribe. Replacement invalidates the old
token, preventing a delayed callback from changing the replacement's state.

The application owns group IDs, part encoding, authenticity checks, metadata
replacement and reconstruction. A group ID must be usable before the full
message is known; blindly hashing the completed payload is not a suitable
general group scheme. The transport authenticates the sending peer, not the
truth of each part. Partials never become accepted full messages or earn full
message delivery scores automatically. Ordinary signed full messages and cached
IWANT responses retain their existing validation path.

Callbacks receive owned payloads and a cancellation token outside node locks.
They may send to eligible mesh or non-mesh peers without holding a write gate.
Local group advertisements have explicit count/byte/heartbeat limits and must be
refreshed by their owner; remote traffic cannot renew them. At most one gossip
callback per topic runs at a time, including across registration replacement.
Callback work and bytes stay accounted until the callback actually finishes.
Unsubscribe closes admission and requests cancellation; node shutdown joins
callbacks. A callback must therefore cooperate with cancellation and must not
await its own node's shutdown.

This is not file storage, automatic chunking or a Swarm implementation. See the
[extension donor note](../../../docs/donors/forge-p2p-gossipsub-extensions-v1.md)
for the pinned draft specification, ownership mapping and evidence boundaries.

The native receive boundary enforces signature policies before cache admission
and IWANT fulfillment. StrictNoSign requires all four authentication protobuf
fields to be absent, not merely empty; StrictSign requires an eight-byte sequence
number. Raw codec and cryptographic helper operations are not substitutes for
receive-policy validation. Self-origin replay from a foreign peer is a P4
rejection even after local message history expires, without a malformed strike.

`codec::decode_received()` shares the raw decoder's single protobuf parse and
returns the decoded RPC plus rejected message topics. It checks authentication
field presence and strict sequence width, not cryptographic authenticity; the
native node verifies remaining signatures before cache, promise or handler use.
Rejected messages still count toward the original RPC element limit.

P2P WebSocket `/ws` and `/wss` multiaddrs remain parseable but unsupported for
dial/listen until Stage 9. WebTransport and WebRTC are separately deferred
browser-profile capabilities; native TCP/QUIC readiness does not imply them.

The donor-first scope manifest is
[`p2p_donor_capabilities.json`](../../../tests/libp2p_interop/p2p_donor_capabilities.json).
It records what the pinned specs, Go and Rust donors provide through four
independent fields: `support_requirement`, `default_activation`,
`interop_applicability` and `decision`. The machine-readable implementation inventory is
[`p2p_feature_inventory.json`](../../../tests/libp2p_interop/p2p_feature_inventory.json).
It contains only current Forge surfaces and evidence. Neither manifest is a
record of currently executed optional interop tests or a release-readiness
verdict. A `mapped` donor case names a compatibility fixture with declared
Forge coverage; it does not prove normal lifecycle activation or a passing
current donor run. The canonical Stage 6 registry is the exact ordered
`stage_6_pr_registry` in that manifest:
`forge-p2p-stage6-roadmap-v1`, `forge-chrono-v1`,
`forge-p2p-host-protection-v1`, `forge-crypto-xsalsa20-v1`,
`forge-p2p-private-network-v1`, `forge-p2p-address-resolution-v1`,
`forge-p2p-reachability-v1`, `forge-p2p-mdns-v1`,
`forge-p2p-autorelay-v1`,
`forge-p2p-path-management-v1`, `forge-p2p-gossipsub-scoring-v1` and
`forge-p2p-gossipsub-extensions-v1`. These retain PR0 through PR7 and PR9
through PR12. Former PR8, `forge-p2p-nat-mapping-v1`, moves to the
[subsequent router configuration block](../../../docs/iterations/forge-p2p-production-implementation-v1.md#subsequent-block-automatic-router-configuration).
The registry also fixes dependencies and allowed capability owners; the roadmap,
chrono and crypto prerequisite PRs own none.

The source inventory checker validates this registration only and never emits a
live interop PASS. A standalone artifact evaluator can report local consistency,
but the CMake promotion target owns the canonical enabled runner invocation and
immediately validates the artifact produced in that same invocation. It binds
the reviewed clean `HEAD`, manifest, canonical argv and roots, current Python,
fixture binaries, effective role/capability configuration and unique evidence
per direction. Native QUIC, native TCP/Yamux and private TCP/Yamux+pnet are
independent proofs; an optional capability is tested only as an explicit enabled
run, never inferred to be default. Planned Stage 6 scenario IDs are comprehensive
acceptance requirements, not runner registration or evidence. Missing or stale
provenance is `NOT_RUN`; a canonical runner nonzero exit or recorded failure is
`FAILED`; a documented limitation reports `PASS_WITH_DOCUMENTED_LIMITATIONS`.

The SHA-256 evidence index is tamper-evident, self-consistent local evidence,
not signed remote attestation or a claim that malicious artifacts are unforgeable.

`test_forge_p2p_inventory` remains the paired source-manifest gate and PR0 runs
only source validation plus the deterministic acceptance-checker self-test. The
explicit `test_forge_p2p_stage6_acceptance` CMake target performs the live
promotion; it never promotes a replayed externally supplied artifact.

`discovery::policy` and `node::limits::discovery` remain Stable source
compatibility surfaces. Node construction normalizes non-default legacy values
into the single managed topology policy and rejects conflicting non-default
legacy and topology settings. `peer_store::apply_peer_exchange` likewise
remains the legacy capability-union mutator; received third-party Forge Peer
Exchange facts do not call it and remain capability-free until Identify.

## When To Use

- Nodes need to connect by peer identity, not just host/port.
- Application protocols need named streams such as `/example/1`.
- Direct transports should be tried first, with explicit relay/hole-punch
  fallback.
- Application/plugin composition needs a shared P2P transport owner; use
  `forge::plugins::net::p2p::node` as the lifecycle/config/route facade above this
  low-level engine.

## When Not To Use

- Do not put application message semantics or storage semantics here.
- Do not treat P2P as authorization. Peer identity is transport identity;
  application authority is owned by consumers.
- Do not put application receipt, durable queue, storage or authorization semantics
  into peer networking. DHT, rendezvous, AutoRelay and GossipSub mechanics
  belong in `forge_net_p2p`; application protocols decide what an operation means.

## Public Modules

- `forge.net.p2p.identity`, `forge.net.p2p.endpoint`, `forge.net.p2p.node`,
  `forge.net.p2p.lifecycle`, `forge.net.p2p.private_network`.
- `forge.net.p2p.protocol`, `forge.net.p2p.message`, `forge.net.p2p.negotiation`.
- `forge.net.p2p.peer_store`, `forge.net.p2p.discovery`,
  `forge.net.p2p.topology`, `forge.net.p2p.dht`,
  `forge.net.p2p.dht.record_store`, `forge.net.p2p.ipns`,
  `forge.net.p2p.provider_registration`, `forge.net.p2p.rendezvous`.
- `forge.net.p2p.pubsub`.
- `forge.net.p2p.relay`, `forge.net.p2p.scoring`,
  `forge.net.p2p.resource_manager`, `forge.net.p2p.connection_gater`.
- `forge.net.p2p.exceptions`.

Target: `forge_net_p2p`.

Dependencies: `forge_api_core`, `forge_asio`, `forge_net_transport`,
`forge_net_tcp`, `forge_net_quic`, `forge_net_pnet`, `forge_net_yamux`, `forge_multiformats` and
Boost.Asio. The library has no database dependency; durable state is supplied
through the asynchronous `peer_store::persistence` and
`dht::record_store::persistence` ports.

Foundation compatibility modules below P2P live in `forge_multiformats`:
`forge.multiformats.varint`, `forge.multiformats.multicodec`,
`forge.multiformats.multihash`, `forge.multiformats.multibase` and
first-class multiaddr/address support.

IPNS uses `forge::chrono::timestamp` directly for EOL (end of lifetime),
including nanosecond precision through year 9999. `forge_net_p2p` depends on
`forge_chrono` for the value and shared RFC3339Nano codec; IPNS owns validation,
expiry, typed P2P errors and preservation of the original signed RFC3339 bytes.
Decode never replaces signed validity text with canonical formatted text.

Source migration: replace the removed `forge::net::p2p::ipns::time_point`
with `forge::chrono::timestamp` and import `forge.chrono.timestamp` directly.
There is no compatibility alias or timestamp clock; acquire current time with
`std::chrono::system_clock::now()` and explicitly cast to nanoseconds when
constructing a timestamp. Wire encodings and signed golden records are unchanged.

## Production Network Direction

`forge_net_p2p` is the owner for production peer-network mechanics. The direction is
a clean C++23 libp2p-compatible implementation: FORGE public types stay
FORGE/Boost-style, while supported libp2p protocols must be wire-compatible with
go-libp2p and rust-libp2p.

Compatibility is not a direct libp2p dependency and not a Go/Rust runtime clone.
It means the same peer identity model, address encoding, protocol negotiation,
handshake, protocol IDs and message rules for protocols FORGE marks as supported.

The canonical block order and donor test rules live in
[`docs/network/quic-p2p.md`](../../../docs/network/quic-p2p.md). Keep this README
as a library overview; do not duplicate the block sequence here.

Current direction: P2P sits above first-class multiaddr, reusable
`forge_net_transport`, and reusable TCP/STCP/Yamux/QUIC layers. QUIC and
TCP+TLS/Noise+Yamux direct paths are wired through private direct profiles.
Future transports must plug into the same multiaddr and transport session
boundary, not fork P2P core. The private profile is TCP/Yamux plus a transport
PSK layer before the normal secure channel, not a negotiated `/pnet` protocol
ID. It excludes QUIC, Relay and DCUtR; AutoNAT lifecycle/client/service and
any future UPnP each require explicit private-profile Internet egress, while
native runs do not inherit that dependency. Optional public mDNS has a separate
Go/Rust acceptance suite; fingerprinted private mDNS has Go evidence and an explicit
pinned Rust limitation. Neither mDNS delivery is attributed to the reachability PR.

The private `interface_watcher`/`interface_state` pairs provide the native
interface prerequisite, not an mDNS service. A single owner-executor operation
subscribes before taking a bounded snapshot (Linux netlink, macOS PF_ROUTE and
sysctl), retaining address prefixes, IPv6 readiness flags and local scope.
Updates replace the full table; interface generations invalidate stale owners
after changes, deletion/reuse or a loss of notification continuity. They are
local generations, not kernel incarnation identifiers. Darwin notifications
can be lost silently, so a bounded owner Asio timer also resynchronizes and
rotates generations. No worker thread or detached task is created. The caller
must serialize calls and await `async_stop()` before destroying the watcher.
Snapshot limits or exhausted resync attempts fail explicitly, never publish a
partial table. Synchronous Darwin snapshot/ioctl calls are bounded in count and
allocation, but cannot promise a hard syscall completion deadline. This layer
does not yet claim native interface-churn or multicast interoperability evidence.

IPv6 interface zones are local routing metadata. Explicit local endpoints may
contain `ip6zone`, but network-learned addresses and DNSADDR results cannot
select an interface on this host. Scoped listeners remain usable locally and
are omitted from ordinary Identify advertisements and observed-address reports.
An original verified signed peer record may remain in the peer store as
sequence evidence while its usable address projection excludes scoped entries.
Such an envelope is not forwarded by discovery; filtering its signed bytes
would invalidate the signature. The stored envelope is not a forwarding permit.

The direct QUIC profile keeps a bounded, peer-scoped cache of opaque QUIC
`NEW_TOKEN` values only for authenticated expected peers. Its key includes the
expected peer identity and direct endpoint host kind/address/UDP port, never
ALPN or local port. Unknown-peer and insecure-test dials explicitly disable
this cache. Profile stop closes the cache before active dial cancellation, so a
late transport callback cannot repopulate it.

`forge_net_transport` is the stream/session substrate for `forge_net_p2p`; it is not an API
or RPC layer. API-over-stream serving lives in `forge.api.stream`, where QUIC/P2P
bindings share frame serve-loop logic without putting `forge::api` into
`forge_net_transport`.

Network-level behaviors that must not be pushed into plugins:

- relay-only/no-direct path support;
- independent maintenance scheduling for peer exchange, reachability, relay
  reservation renewal and discovery;
- peer discovery and relay discovery;
- protocol capability negotiation;
- network limits, backpressure, metrics and shutdown behavior.

Stage 6 PR-6 implements typed host events and periodic Ping liveness within
`forge_net_p2p`; its final-head live evidence gate is still open. This host-local
policy is distinct from the already current Ping wire protocol.
`plugins.net.p2p.node` may map the validated configuration and
consume narrow local events, but it must not own Ping, observed-address,
AutoNAT lifecycle, mDNS, UPnP or topology maintenance loops. Coordinated direct
dial and port reuse replace the deprecated `/libp2p/simultaneous-connect`
negotiation.

### Reachability Ownership (Preview)

`node::options::reachability_policy` controls the node-owned client lifecycle,
observation expiry, independent-observer thresholds and bounded Ping work.
`async_start()` starts it after initial bootstrap; `async_stop()` cancels and
joins its operations before closing sessions and persistence. Manual
`async_probe_reachability(peer)` joins the same coalesced per-observer path.
Its return is that probe's result, not the confidence-filtered host verdict.

AutoNAT v1 votes determine node-level reachability. V2 evidence is scoped to a
particular address and requires an operation-owned nonce observed on an actual
inbound dialback. The authenticated dialer may differ from the observer's peer
identity, as in Go's separate probe host. LAN evidence never establishes public
Internet reachability; service refusal, protocol failure and Ping failure are
not negative NAT votes. Address-set changes invalidate pending generations.

Both AutoNAT services default off. Explicit `service_v1_enabled` and
`service_v2_enabled` advertise only their corresponding request protocols; the
v2 client advertises the dialback protocol independently. Service probes use
fresh, isolated native connections and do not populate the service's normal
peer/session topology. Private TCP/PSK nodes require explicit
`private_network::internet_egress_policy::allow_internet`; the default rejects
external probing before I/O and does not advertise these roles. This does not
enable QUIC, Relay or DCUtR in the private profile.

`node::reachability_status()` exposes the last published host state.
`node::host_events()` returns a move-only subscription with an atomic initial
snapshot and increasing generation. One reader is allowed per subscription;
slow readers receive the latest state with `resync_required`, not an unbounded
event queue. Closing a subscription or stopping the host wakes pending reads
with `nullopt`. A surviving subscription does not retain the node.

Observed addresses become advertised only after independent authenticated
Identify observations confirm a listening endpoint. Expiry or session removal
withdraws confirmations and triggers Identify Push. A v1 response's substituted
observed address is a vote, never advertisement authority. New host state is
ephemeral; no peer-cache schema or plugin YAML changes belong to this PR.

See [the PR-6 donor note](../../../docs/donors/forge-p2p-reachability-v1.md)
for accepted donor behavior and the still-pending runtime/live validation gates.

Circuit Relay v2 reservations belong to authenticated peer sessions. Renewal
keeps the same reservation generation and active-circuit accounting; the final
session disconnect releases the reservation. Configured relay duration must be
positive and exactly representable in the whole seconds advertised on the wire.
Per-direction byte limits close the direction as soon as its final permitted
byte is forwarded.

### Node-Owned AutoRelay

With `relay_policy.client_enabled` and `auto_discovery_enabled`, startup enables
AutoRelay on unknown/private reachability. The manager consumes bounded topology
and indexed peer-store hints; only an authenticated direct session with verified
Identify HOP support can acquire a reservation. It does not run another discovery
loop. `async_refresh_relay_candidates()` triggers this same manager.

`target_reservations`, `max_candidates_per_refresh` and
`max_parallel_reservations` independently bound retained leases, candidates and
concurrent operations. Renewal follows the accepted expiry. Failed candidates
back off without indefinitely hiding later candidates. Public reachability
withdraws automatic ownership and circuit addresses; `public_relay_allowed`
permits only manual reservations on a public host, not automatic advertising.

Generated circuit addresses are separate from caller-configured addresses and
exist only while their reservation and direct relay session remain valid.
Changes update the signed peer record and Identify Push. Cancellation and
shutdown invalidate pending grants, stop admission and join the workers before
peer persistence closes. `async_cancel_relay(peer)` cancels local outbound
ownership without closing the shared authenticated connection.

An unsolicited `closed` or `canceled` reservation result is a failed attempt,
not evidence of owner cancellation. It receives the normal per-peer jittered
backoff. Owner invalidation or completion without the same live automatic lease
receives an acquisition cooldown without a failure penalty. This cooldown does
not delay renewal of a different live lease; genuine failure backoff still does.
Both deadlines survive candidate/session rotation. Bounded history pressure
defers new acquisition instead of forgetting an unexpired deadline. Notifications
and manual refresh cannot bypass or extend it. Shutdown only drains existing
work, without retries.

Identify applies `max_listen_endpoints` to the combined advertisement before
signing. Live reservation-backed circuits have priority within that projection;
its signed and unsigned lists match. The full local control view and configured
addresses are not truncated or changed by this wire budget.

Circuit Relay v2 service is opt-in through `relay_policy.service_enabled`;
capability bits alone do not enable it. Service admission requires connected
direct owners, bounds request rates/reservations/circuits, and counts both
participants against per-peer limits. `limits.relay.handshake_timeout` bounds
the STOP handshake independently of the subsequent circuit duration. Absent or
zero remote duration/data limits retain their standard unlimited wire meaning;
local byte/stream/queue caps still apply. A voucher may be absent, as in Rust;
any supplied voucher must validate against the authenticated relay and client.

Diagnostics expose bounded live reservation owners separately from historical
peer cache facts. Private PSK configurations continue to reject Relay/DCUtR.
See [the AutoRelay donor note](../../../docs/donors/forge-p2p-autorelay-v1.md)
for scope, ownership and the required independent three-peer acceptance proof.

`forge_net_p2p` remains free of application plugins, storage and authorization
policy. Application protocols own idempotency, acknowledgement and
permission checks above P2P.

GossipSub validation keeps `accept`, `reject` and `ignore` terminal while the
message remains in bounded history. `retry`, handler failure and local
validation backpressure are transient: the receiving heartbeat requests the
cached payload from its source peer after a capped exponential cooldown,
independently of ordinary `IHAVE` history. A message becomes terminally ignored
after the configured validation or request-attempt limit. Each heartbeat
applies a round-robin retry budget, and retry records are evicted with the
payload history, so unreachable peers and repeated transient failures cannot
create unbounded work or a second cache.

## Examples

### Start A Node

```cpp
#include <boost/asio/awaitable.hpp>

import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;

boost::asio::awaitable<void> start_node(forge::asio::runtime& runtime) {
   auto options = forge::net::p2p::node::options{
      .certificate_pem = certificate_pem,
      .private_key_pem = private_key_pem,
      .peer_state = {.persistence = persistence},
      .lifecycle = {
         .listen = {forge::net::p2p::parse_endpoint(
            "/ip4/127.0.0.1/udp/9443/quic-v1")},
         .bootstrap = {forge::net::p2p::bootstrap_peer{
            .address = forge::net::p2p::parse_endpoint(bootstrap_endpoint).to_multiaddr()}},
      },
   };

   auto node = forge::net::p2p::node{runtime, options};
   const auto status = co_await node.async_start();
   if (status.degraded) {
      report_degraded_bootstrap(status);
   }
   co_await node.async_stop();
}
```

Production certificates must carry the signed libp2p identity extension. Peer
IDs are not derived from a bare certificate hash in production verification
paths.

Bootstrap endpoints should include `/p2p/<peer-id>` so the authenticated peer is
pinned before the dial. Peer-less legacy endpoints remain accepted: the node
learns and protects the peer only after transport authentication succeeds.

### Parse A libp2p QUIC Endpoint

`forge::net::p2p::endpoint` is FORGE-style public vocabulary. It accepts and emits the
libp2p address text format for compatibility, but callers do not need to model
their application API around the `multiaddr` term.

```cpp
import forge.net.p2p.endpoint;

auto endpoint = forge::net::p2p::parse_endpoint(
   "/ip4/127.0.0.1/udp/4001/quic-v1/p2p/12D3KooW...");

co_await node.async_listen(endpoint);

co_await node.async_listen(forge::net::p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4001"));
std::vector<forge::net::p2p::endpoint> advertised = node.local_endpoints();
```

QUIC and TCP+TLS/Noise+Yamux are currently registered direct transports. TCP
uses `node::options::stream_security`, with `node::stream_security::tls_and_noise`
as the default: offer libp2p TLS (`/tls/1.0.0`) first, then Noise (`/noise`) if the
peer rejects the TLS protocol during multistream-select. `node::stream_security::tls`
and `node::stream_security::noise` restrict both incoming and outgoing TCP
security negotiation to that single protocol, including PNET-protected TCP and
coordinated connections with reversed security roles. A selected protocol's
handshake or identity failure is terminal; it does not fall back to another
security protocol.

```cpp
forge::net::p2p::node::options options;
options.stream_security = forge::net::p2p::node::stream_security::noise;
```

QUIC still uses its native TLS security regardless of this stream policy. Relay
circuit endpoints currently support only Noise: TLS-only options must disable
`relay_policy.client_enabled` or node validation rejects them. TLS-only relay
service forwarding remains supported when its client role is disabled; the
service forwards the inner byte stream without terminating its security.

`/ws` and `/wss`
multiaddrs are parseable but direct dial/listen returns typed unsupported until
a dedicated compatibility block wires a production transport. Future transports
must use the same private direct profile boundary.

The Noise transport treats the secured connection as a byte stream and segments
large Yamux writes into independently authenticated Noise records whose encrypted
length fits the protocol's 16-bit record header. The live TCP Noise matrix sends
a 192 KiB echo payload in both Forge/Go/Rust directions so record segmentation is
proved across donor implementations rather than inferred from raw Yamux tests.

`local_endpoints()` is the full canonical listen/advertise set and each endpoint
includes `/p2p/<local-peer>`. `local_endpoint()` remains a first-endpoint
compatibility convenience for older single-listen consumers.

### Direct Dial Resolution

`async_connect(multiaddr, connect_options)` accepts raw DNS carriers, including
`/dnsaddr`; the existing endpoint overload uses the same node-owned scheduler.
The node expands a batch of peer addresses under one logical deadline and one
dial reservation, then starts bounded, ranked TCP/QUIC attempts. The attempt
limit counts native launches, not DNS roots. Private-network dials filter to TCP
before ranking, so an ineligible QUIC record does not hide an eligible TCP record.
Peer gating follows resolved identity inference; address gating runs on each
concrete candidate before native transport admission.

Only the authenticated winner enters the node session registry, after all
losers have completed native cleanup. Stop seals session admission before
draining the scheduler. A failed publication retires the exact session and
waits for its native close and reservation-release barrier.

Peer-store success and failure feedback retain the original address roots;
temporary DNS answers do not replace those roots. A root fails only when all
its eligible planned children were launched and failed attributably. Global
deadline, caller cancellation, filtered/unlaunched candidates and canceled
losers remain neutral. A cached DNS-child stream failure alone is not evidence
that fresh resolution of its root would fail.

### Peer And DHT Record Persistence

The low-level node requires `peer_store::persistence` outside explicit insecure
tests. The backend-neutral asynchronous contract provides paged hydration,
atomic mutation batches, bounded expiry pruning, flush and deterministic close.
Prune returns the exact peer and Rendezvous identities removed, so
the bounded operational directory applies the same deletion set even when the
durable store contains older records that were not hydrated.
The operational directory remains bounded and performs indexed point/candidate
queries without scanning durable history. Per-peer endpoint, protocol, relay and
total variable-byte limits prevent one remote peer from bypassing the global
peer and persistence-queue bounds. The endpoint and total variable-byte limits
also apply to each Rendezvous record, including records returned during
hydration, before any operational state is changed. DHT provider/value bounds
belong to the profile-scoped `dht::record_store` described below.

Identify address provenance is operational metadata used to replace each live
unsigned or certified snapshot without appending stale addresses. The existing
ObjectDB cache schema v3 separates peer/Rendezvous rows from profile-scoped DHT
value/provider rows while retaining one physical named store. Schema v2 caches
must be reset instead of hydrated because they cannot safely represent raw DNS
address carriers. Hydrated peer
endpoints conservatively re-enter as
learned cache facts and age through the existing peer-health/expiry policy;
the next verified Identify refresh establishes provenance for its live
snapshot.

Identify receive limits distinguish one length-delimited frame from the merged
multipart message. Defaults accept the Go libp2p profile of at most ten 8 KiB
parts while reserving the complete decode budget before reading. Partial
Identify Push follows Rust libp2p merge semantics: omitted scalar fields and
empty repeated fields preserve the previous verified facts. A valid signed
PeerRecord with an equal sequence is accepted as a refresh because Rust
sequences have second granularity; a lower sequence is rejected. Lifecycle
diagnostics expose the last non-cancellation bootstrap failure directly.

The official P2P plugin supplies the production ObjectDB adapter. Direct users
may implement the persistence port over their own lifecycle owner. The memory
implementation is deterministic but intended only for tests and explicit local
experiments.

Peer state and DHT records are intentionally separate operational domains even
when the official plugin stores them in one physical ObjectDB store. Each DHT
profile owns an isolated routing table, value/provider record store, query and
maintenance lifecycle. The Amino profile fixes `/ipfs/kad/1.0.0`, `k=20`,
`alpha=10`, `/pk` and `/ipns`; product validators/selectors require a distinct
product protocol ID. Queries initialize the shortlist from the local `k`
closest peers and use `alpha` only as the concurrent RPC bound.
Per-profile diagnostics expose whether autonomous maintenance is enabled, the
startup lookup and in-flight state, consecutive failures and the bounded delay
until the next attempt.

Custom value validators report deterministic record invalidity with
`exceptions::record_rejected`. Capacity, persistence, key resolution and other
operational failures must retain their original typed error so a GET quorum
cannot silently discard a locally valid record.
`record_store::async_put_received()` is the explicit network-ingress boundary:
it returns an empty result only for validator-origin record rejection. Durable
apply failures, including a backend exception carrying the same error code,
still propagate and mark persistence degraded. Direct application writes use
`async_put()` and never silently discard an invalid value.
Value retention is controlled independently by `value_record_ttl`; provider
identity, provider addresses and provider republishing retain their separate
TTL settings. All wire-derived lifetimes are positive and bounded by the DHT
`uint32` TTL representation before deadline arithmetic.

Amino keeps the donor-compatible 16 KiB outbound message limit and accepts the
larger bounded inbound messages used by Go/Rust implementations. Inbound peer
lists and endpoint lists are parsed into fixed local bounds instead of rejecting
an otherwise valid response solely because it contains more candidates than
Forge will retain.

`async_provide()` returns a move-only `provider_registration`. Its first
publication is acknowledged only after the local record is durable and the
requested remote quorum succeeds. The node renews that exact endpoint snapshot
with jitter while an owner remains; the last withdrawal removes local
ownership. Restart never resumes publication without a fresh product-owned
registration. Reaching the caller's quorum does not stop publication to the
remaining closest peers: the node attempts the complete `k`-bounded fanout and
reports success only when the requested quorum was reached.

Only an authenticated `ADD_PROVIDER` from the claimed provider creates durable
remote ownership. Providers returned by third-party `GET_PROVIDERS` responses
are bounded discovery results and are not assigned a fresh local TTL. Likewise,
`GET_VALUE` returns the remaining lifetime of a stored record rather than
replaying the TTL from the original request.

For a mutation requesting durable acknowledgement, persistence distinguishes a
failed commit from a commit whose subsequent durable flush could not be
confirmed. The latter is applied to operational state, marks the store degraded
and raises typed `durability_uncertain`; callers must not blindly retry the
logical operation as though it were known not to have committed.
The degraded state remains sticky across non-durable maintenance and is cleared
only by a later confirmed durable apply or explicit flush.
`node::diagnostics()` exposes the queue depth, sticky degraded state, failure
count and last persistence failure so operators do not have to infer durable
health from a transient call error.

Production ObjectDB hydration validates one raw row at a time. Each bounded
page is read through its own operation-scoped snapshot; no unbounded snapshot is
held across the complete hydration sequence. Per-record limits are checked
before conversion into operational peer or DHT state, so a malformed durable
row cannot force an unbounded hydration page into memory. Before live-record
capacity is enforced, DHT hydration removes expired durable rows in bounded
prune pages. `max_hydration_pages` bounds each startup phase; exhaustion
returns typed backpressure instead of holding the persistence gate forever, and
a later hydration attempt continues from the already committed cleanup. DHT
deadlines bound the remote wire exchanges. Once a provider or
Rendezvous record has been accepted for durable acknowledgement, its persistence
step remains owned and awaited by the caller instead of being abandoned after a
possibly committed transaction.
Rendezvous servers use `async_register_rendezvous()` so the configured per-peer
registration limit is checked under the persistence gate before a durable write.
Client discovery materializes the wire TTL into a local absolute expiry before
the accepted registration enters operational or durable peer state.

```cpp
auto node = forge::net::p2p::node{runtime, {
   .certificate_pem = certificate_pem,
   .private_key_pem = private_key_pem,
   .peer_state = {.persistence = persistence},
   .lifecycle = {
      .listen = {listen_endpoint},
      .bootstrap = {{.address = bootstrap_endpoint.to_multiaddr()}},
   },
}};

auto status = co_await node.async_start();

auto test_store = forge::net::p2p::peer_store{
   {.persistence = forge::net::p2p::peer_store::make_memory_persistence()}};
```

### Register A Protocol

```cpp
#include <cstdint>
#include <vector>

node.register_protocol_handler(forge::net::p2p::protocol_id{.value = "/example/1"},
                               [](forge::net::p2p::node::incoming_protocol_stream incoming)
   -> boost::asio::awaitable<void> {
   std::vector<std::uint8_t> frame = co_await incoming.stream.async_read_frame();
   co_await incoming.stream.async_write_frame(frame);
});
```

An explicitly registered handler takes precedence over an enabled built-in
handler for the same protocol ID. Unregistering it restores the built-in
fallback; registering a second explicit handler still requires removing the
first. Identify advertises each supported ID once. All handlers use the same
authenticated stream admission and scoped resources. Registration cannot bypass
private-network restrictions or the direct-session requirement for relay
HOP/STOP.

### Publish Typed APIs Above P2P

Application protocols that need request/response, typed errors and idempotent
operation receipts should expose an `forge_api_core` contract and mount it through the
P2P API binding or `forge::plugins::net::p2p::resolver`. P2P opens the stream and
enforces peer/path policy; API dispatch owns method calls and error projection;
the application handler owns authorization and durable state.

### Typed API Protocol Binding

`forge.api.p2p.binding` builds P2P API bindings on top of negotiated protocol streams.
The binding path uses `multistream-select` and the same direct, hole-punch and
relay path manager as ordinary P2P protocol streams; it must not reintroduce an
FORGE-only hello envelope into direct QUIC sessions. Once a protocol stream is
open, frame serving delegates to `forge.api.stream`; P2P keeps only P2P policy:
protocol id, known-peer checks and discovery scope.

### Connect And Open A Protocol Stream

This is the low-level engine path for custom transport owners and tests.
Application plugins should use `forge::plugins::net::p2p::node::api` instead of calling these
methods directly.

```cpp
boost::asio::awaitable<void> open_example_stream(forge::net::p2p::node& node) {
   forge::net::p2p::node::session_info session = co_await node.async_connect(remote_endpoint, {
      .expected_peer = expected_peer,
      .timeout = std::chrono::milliseconds{10'000},
   });

   forge::net::p2p::stream stream = co_await node.async_open_protocol_stream(
      session.remote_peer,
      forge::net::p2p::protocol_id{.value = "/example/1"});
   use_stream(std::move(stream));
}
```

### Learn Endpoints And Probe Reachability

```cpp
import forge.net.p2p.peer_store;

node.peers().learn_endpoint(
   remote_peer,
   forge::net::p2p::parse_endpoint("/ip4/127.0.0.1/udp/9444/quic-v1"),
   {.bits = forge::net::p2p::capabilities::direct_quic | forge::net::p2p::capabilities::peer_exchange});

boost::asio::awaitable<void> update_reachability(forge::net::p2p::node& node) {
   forge::net::p2p::reachability::state reachability = co_await node.async_probe_reachability(observer_peer);
   if (reachability == forge::net::p2p::reachability::state::relay_only) {
      schedule_relay_setup(remote_peer);
   }
}
```

### Reserve Relay Explicitly

```cpp
boost::asio::awaitable<void> open_relayed_stream(forge::net::p2p::node& node) {
   forge::net::p2p::relay::reservation::info reservation = co_await node.async_reserve_relay(
      relay_peer,
      {.ttl = std::chrono::milliseconds{60'000}, .max_streams = 8});

   forge::net::p2p::stream relayed = co_await node.async_open_protocol_stream(
      remote_peer,
      forge::net::p2p::protocol_id{.value = "/example/1"},
      {.allow_relay = true, .relay_peer = reservation.relay_peer});
   use_stream(std::move(relayed));
}
```

### Stop Cleanly

```cpp
boost::asio::awaitable<void> stop_node(forge::net::p2p::node& node) {
   co_await node.async_stop();
}

void request_node_stop(forge::net::p2p::node& node) {
   node.stop();
}

boost::asio::awaitable<void> finish_node_stop(forge::net::p2p::node& node) {
   co_await node.async_stop();
}
```

`stop()` closes admission and listeners and starts disconnecting current
sessions without blocking the caller. It intentionally removes those sessions
from the active set before their transport teardown has finished.
`async_stop()` is the completion barrier: it always waits for the teardown
started by `stop()`, including STCP/Yamux read-loop cleanup.

## Security Notes

Production options require mTLS identity with a signed libp2p certificate
extension. `allow_insecure_test_mode` exists for tests and explicit local
experiments only; in that mode the node may use the in-memory peer store when no
persistence is provided. Peer mismatch, TLS verification failure, missing
identity extension and invalid envelopes are correctness failures.

The node parses its configured identity key once during construction and reuses
the immutable key material for TLS, Noise, PubSub, rendezvous and relay
signatures. Insecure QUIC-only test nodes may omit signing material until an
operation that requires a signature is used.

## Risks And Anti-Patterns

- Do not treat peer identity as application authorization. It proves transport
  identity, not permission to perform application actions.
- Do not silently fall back to relay for operations that require a direct-peer
  policy. Relay use must be explicit and visible to the caller.
- Do not put durable delivery, exactly-once semantics or storage guarantees in
  `forge_net_p2p`; protocols above P2P own those contracts.
- Do not implement application retry or durable delivery loops against raw
  `node` in application plugins. Use typed request/receipt APIs for synchronous
  operations and a focused higher-level service for durable asynchronous work.
- Do not define a new P2P-only API error payload. API protocols use
  `forge::api::core::error_payload` in `forge::api::core::frame` error responses.
- Do not let protocol handler exceptions disappear in detached tasks. Expected
  application failures should be typed exceptions and unexpected failures should
  be counted/diagnosed.
- Do not treat `.peer_policy(...)` or `.max_inflight_per_peer(...)` as cosmetic.
  Unknown peers and too many active API calls are rejected before application API
  handlers run.
- Do not make `forge.api.p2p.binding` responsible for peer discovery, relay or node
  lifecycle. It is only the API protocol binding artifact.
- Do not implement AutoNAT, AutoRelay, DHT, rendezvous or pubsub loops in an
  infrastructure plugin. Network mechanics belong in `forge_net_p2p`; plugins only
  configure and consume them.

## Private mDNS Service Boundary

`details/mdns_service.hxx` owns the watcher, per-interface/family UDP sockets,
bounded outbound queues and registry. Node integration is opt-in through
`node::options::mdns`; enabled mDNS rejects `topology::mode::static_only` before
I/O. It starts after listeners and joins before node resources are released.
`start` tracks its lifetime; `request_stop` only signals the owner strand;
`async_join` waits for every receive/send/watcher operation before releasing
memory and descriptor reservations. The resource manager is retained by value
with its shared ledger. Integration callbacks must use weak node ownership or
independently owned state, not raw `node::impl` references.

The caller supplies the public `_p2p._udp.local` or private service name and
current listeners. Advertisements expand wildcard listeners to usable interface
addresses and omit local IPv6 zones. Shared PTR records never set cache-flush;
TXT/SRV/A/AAAA records may. Returned leases replace only the mDNS discovery
source; they do not authorize peers or trigger synchronous dialing.

Memberships and outbound routes select the interface explicitly. Darwin IPv4
sockets retain `IP_BOUND_IF` for their entire lifetime; no send temporarily
mutates socket routing. Received destination/interface metadata is checked;
unicast reception additionally requires a known matching interface prefix.
Removed generations are stopped before their workers are joined.

Deliberate donor composition: pinned Rust uses an ephemeral sending socket for
both queries and answers. Responses therefore are not rejected by source port,
and query handling always retains a multicast answer, with an additional
on-link legacy/QU unicast answer where appropriate. Answers never trigger
answers. Known-answer suppression, per-worker response rate limiting, bounded
queue coalescing and receive-batch yielding limit packet-driven work.

Topology keeps mDNS-only snapshots with steady deadlines and interface
generations, separate from persisted discovery observations. Packet updates wake
the existing bounded reconciliation loop, not DHT discovery. Queued candidates
are rechecked at admission; expiry and interface withdrawal cancel pending dials
without closing authenticated sessions. Direct attempts use transient provenance:
endpoint success/failure and discovery TTL are not persisted, while authenticated
Identify facts remain. `discovery::source::mdns` is operational-only and peer-store
writes reject it. Private networks use the separate 16-byte PNet network
fingerprint in `_p2p-<hex>._udp.local`, not the operational diagnostic hash.
This integration does not itself establish cross-platform or donor interop
acceptance; those require the separate live matrix.

Development Linux live runs passed the 38-case mDNS matrix: 28 authenticated
hidden-peer exchanges across IPv4/IPv6, TCP Noise/TLS and QUIC (private Go uses
TCP PNet/TLS), 8 explicit quiet namespace-isolation cases with same-network
positive controls, and 2 same-process/session interface-churn cases. Component
tests alone do not establish those results. Final-SHA acceptance still requires
the canonical `promote_stage6_acceptance.py --suite mdns` invocation and the
strict raw-evidence validator in `tests/libp2p_interop/mdns_acceptance.py`.

Pinned Rust cannot configure private mDNS namespaces. Go quiet receipts count
entered notifications and lifetime connection events, but its detached donor
callbacks cannot all be joined; this limitation is retained in the evidence.
Forge observation counts are runtime counts, not fabricated discovery-address
snapshots. Plugin configuration, cross-platform live support, scoped link-local
live routing and full Stage 6 production support are not claimed by this matrix.

## Typical Mistakes

- Do not pass plaintext secrets through protocol IDs or peer metadata.
- Do not register duplicate protocol handlers; the node rejects them.
- Do not use relay fallback silently for actions that require direct peer policy.

## Tests

`test_forge_quic_p2p` covers identity shape, codec rejection, direct protocol echo,
path manager fallback, connect/open timeouts, peer exchange, relay, reachability,
hole punching, DHT/rendezvous component behavior and production option
validation.
