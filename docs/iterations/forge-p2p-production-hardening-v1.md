# Forge P2P Production Hardening v1

> **Status:** accepted release-blocking direction, implementation pending.
>
> This document records the production integration gaps found while preparing
> Forge Content Swarm. Completing this program is the primary Forge network
> objective. Content Swarm may develop in parallel from Stage 7, subject to the
> separate development and production gates in section 10. This document does
> not change the public API by itself.

## 1. Audit Standard

A libp2p feature is not considered delivered merely because its codec, protocol
handler, low-level `forge_net_p2p` method or interoperability test exists. A
production claim additionally requires:

- configuration through both `forge_net_p2p` options and the official
  `plugins.p2p.node` adapter;
- autonomous activation and deterministic shutdown through the node lifecycle,
  with the plugin delegating to that lifecycle;
- bounded scheduling, cancellation, retry and backoff;
- diagnostics that expose the effective state;
- focused raw-node tests and an integration test that starts official plugins
  over the same node behavior.

The low-level implementations remain valuable and should be reused. The gap is
primarily incomplete node autonomy and official-plugin wiring, not a request to
introduce a second P2P stack.

### 1.1 Production Definition

`forge_net_p2p` is production-ready only when all of the following are true:

- every advertised libp2p protocol follows the applicable active libp2p
  specification and has explicit donor traceability;
- protocol codecs, state machines, operational indexes and maintenance tasks
  are connected through one `node` lifecycle rather than being independently
  callable demonstrations;
- the implementation reuses the appropriate Forge runtime, transport,
  cryptography, configuration, diagnostics and DB components instead of
  introducing protocol-local substitutes;
- all network-controlled work has bounded memory, concurrency, queueing,
  deadlines, retry and shutdown behavior;
- the official plugin configures and exposes the same node behavior without
  reimplementing network mechanics;
- raw-node, official-plugin, restart, scale, adversarial and live donor
  interoperability tests prove the production path;
- no public class, method, option, capability, metric or protocol handler is a
  stub, an isolated test fixture or a partially implemented compatibility
  claim.

Protocol compatibility alone is insufficient. A codec that exchanges valid
bytes while the node omits routing, maintenance, admission or state transitions
is not a delivered protocol.

### 1.2 Implementation-State Vocabulary

Every public P2P surface and internal production component must be classified
using one of these states during the hardening work:

| State | Meaning | Allowed in production |
|---|---|---|
| `live` | Connected to normal node lifecycle with bounded operation, diagnostics and integration coverage. | Yes. |
| `manual-only` | Correct operation exists, but only an explicit caller or test activates it. | Only when the API is intentionally manual application intent. |
| `partial` | Some required state, policy, maintenance or security behavior is absent. | No. |
| `stub` | Wire/API surface accepts an operation without implementing its contract. | No. |
| `orphan` | Component is implemented and tested in isolation but production code never uses it. | No. |
| `unverified` | Implementation exists, but donor parity or lifecycle evidence is incomplete. | No. |

Before the production release, every `manual-only`, `partial`, `stub`, `orphan`
and `unverified` item must be completed, deliberately narrowed and rejected as
unsupported, or deleted. Keeping a misleading surface because isolated tests
are green is forbidden.

### 1.3 Evidence Standard

Acceptance evidence is layered and cumulative:

1. Codec fixtures prove exact wire compatibility and malformed-input handling.
2. State-machine tests prove transitions, cancellation and resource release.
3. Raw-node integration proves the component is connected to normal lifecycle.
4. Official-plugin integration proves configuration and dependency wiring.
5. Restart and scale tests prove persistence and bounded operational cost.
6. Adversarial tests prove admission, scoring, timeout and memory behavior.
7. Go and Rust libp2p interoperability proves enabled protocol behavior rather
   than only successful negotiation.

A donor manifest entry cannot be marked `ported` from codec or isolated unit
coverage alone. It must identify the corresponding production call path and
the highest completed evidence layer.

## 2. Current Reliable Baseline

The following paths are connected through the official plugin today, subject to
the production-startup gap below:

- direct QUIC and TCP/Yamux sessions to configured or already known peers;
- bounded bootstrap reconnect and bootstrap-session protection;
- inbound application protocol routing and Forge API over a known `peer_id`;
- session admission and the currently connected relay resource limits;
- diagnostics over the shared node;
- GossipSub publish/subscribe over peers that are already connected.

This is a static/bootstrap-centric topology. It is not yet an autonomous
libp2p mesh.

Generic stream and dial limit methods exist in `resource_manager`, but are not
connected to node stream opening or dialing. They are therefore not part of the
reliable baseline yet.

## 3. Node Autonomy Principle

`forge::net::p2p::node` is the reusable libp2p host, not a passive collection of
protocol methods that requires a Forge App plugin to remain healthy. Once
constructed with explicit policy and started, it owns all transport-neutral
network mechanics and their maintenance:

- bootstrap connection maintenance;
- Identify and Identify Push;
- observed-address confidence, AutoNAT and effective reachability;
- mDNS and DNSAddr discovery/resolution;
- DHT routing refresh and provider-record maintenance;
- Rendezvous registration/discovery refresh;
- Peer Exchange scheduling;
- optional UPnP mapping renewal and loss detection;
- AutoRelay candidate selection and reservation renewal;
- DCUtR attempts and relay fallback;
- adaptive dial ordering and UDP/IPv6 black-hole suppression;
- staged connection gating and scoped host resource accounting;
- connection watermarks, scoring, liveness sampling and GossipSub heartbeat.

`plugins.p2p.node` is an application adapter. It decodes configuration, prepares
or injects durable dependencies, constructs the node, delegates start/stop and
exports narrow local APIs. It must not implement parallel protocol loops,
inspect diagnostics as control state or become required for a programmatic
`forge_net_p2p` deployment.

Explicit application intent remains outside autonomous maintenance. For
example, Content Swarm decides which content key to provide; after
`async_provide(key)`, the node owns DHT publication, TTL refresh and routing
mechanics for the caller-controlled registration lifetime. The caller must be
able to withdraw or release that registration when content is no longer
available. Products continue to own authorization and network membership.

## 4. Original Hardening Baseline Matrix

The following matrix records the audit state before Stages 2-5. It separates
the then-working protocol substrate from missing operational production
behavior and remains the accepted historical backlog, not a current support
snapshot. Current implementation claims are owned by
`tests/libp2p_interop/p2p_feature_inventory.json` and the implementation
roadmap.

| Area | Current state | Confirmed problem | Required disposition |
|---|---|---|---|
| Identity, multistream negotiation and secure peer authentication | `live` substrate | Authentication is not followed by automatic Identify, so protocol/address truth is incomplete. | Preserve substrate; integrate Identify lifecycle. |
| Direct QUIC and TCP/Yamux sessions | `live` | Production topology still depends primarily on configured or already known peers. | Preserve and cover through autonomous topology tests. |
| Persistent peer store | `partial` | Direct RocksDB dependency, private codec/layout, process-local serialization and whole-store operational scans. Official plugin supplies no production store. | Replace with ObjectDB adapter and bounded operational indexes. |
| `dht::routing_table` | `orphan` and incomplete | Node never owns it. The class stores a flat map, scans and sorts all peers, and failure counts do not drive replacement or eviction. | Replace its internals with donor-consistent bounded Kademlia routing state and make node the owner. |
| Kademlia peer lookup | `manual-only`, `partial` | Iterative query exists, but seeds and server responses scan persistent peer history instead of an operational routing table; no autonomous refresh. | Integrate routing table, server-role admission, bootstrap and bucket refresh. |
| DHT provider records | `manual-only`, `partial` | One-shot provide/find works, but registrations have no owned lifetime, withdrawal or TTL republish loop. | Add registration handle, renewal, withdrawal and bounded persistence. |
| DHT value records | `stub` | `PUT_VALUE` echoes the supplied record without storage or validation; `GET_VALUE` returns only closer peers. Public node API does not expose a complete value-store contract. | Implement validated record storage/selection completely or reject/remove value operations and advertise provider-routing-only scope. |
| Identify | `manual-only`, `partial` | Inbound handlers work, but ordinary session establishment does not initiate Identify. New sessions initially copy local capabilities as if they were remote capabilities. | Identify every new session, verify and persist remote facts, emit Identify Push on local changes. |
| Peer Exchange | `manual-only` | Inbound response and explicit request work, but node never schedules outbound exchange. | Integrate bounded exchange into topology maintenance. |
| Rendezvous | `manual-only`, `partial` | Registration and discovery work only when explicitly called; there is no renewal/discovery lifecycle. | Add role configuration, registration lifetime and refresh loop. |
| AutoNAT | `manual-only`, `partial` | v1/v2 handlers and explicit probes exist, but no node owns v1 reachability lifecycle, v2 address evidence or their bounded reconciliation. | Add separate v1 node-level and v2 address-level policies with one effective reachability projection. |
| Relay and AutoRelay | `partial` | Relay mechanics and AutoRelay loop are live, but candidate supply is starved by missing Identify/discovery lifecycle. | Feed verified topology into existing reservation management. |
| DCUtR hole punching | `partial` | Operational DCUtR code exists separately from the public `hole_punch::attempt` state object, which is only unit-tested. Per-peer attempt ownership is not represented by that helper. | Establish one private per-peer attempt state machine; integrate it or delete the orphan class. |
| Ping | `manual-only` | Responder and explicit RTT query work; no optional liveness policy updates health/backoff state. | Add bounded configurable sampling or document responder-only mode explicitly. |
| Connection manager | `live`, needs scale proof | Session admission, protection and pruning are connected. | Preserve; test under topology churn and shared resource policy. |
| Resource manager sessions and relay scopes | `live` | Connected paths enforce limits. | Preserve and expose validated effective limits. |
| Resource manager stream scopes | `orphan` | `try_acquire_stream` and release methods are tested but unused by node stream paths. | Integrate RAII stream reservations on every inbound/outbound stream path. |
| Resource manager dial scopes | `orphan` and incomplete | `try_acquire_dial` is tested but unused, and its current counter contract has no production release path. | Redesign as owned dial reservation and integrate before dialing. |
| Generic resource queued-byte limit | `orphan` | Configured and validated, but generic resource accounting does not consume it; only transport/relay-specific queues do. | Connect byte reservations to stream buffers or remove the misleading generic limit. |
| GossipSub delivery, validation and heartbeat | `live` core | Message propagation and heartbeat operate over connected peers. | Preserve while completing topology and scoring. |
| GossipSub peer scoring | `partial` | Invalid and duplicate counters change scores, delivered score input is unused, and mesh admission/pruning selects by container order instead of score. | Implement donor-consistent thresholds, decay, mesh selection, opportunistic grafting and score retention. |
| Discovery refresh facade | `manual-only` | `async_refresh_discovery()` combines one-shot DHT/Rendezvous work, but no node lifecycle invokes it. | Replace one-shot orchestration with managed topology services while retaining explicit diagnostics/admin triggers where useful. |
| Bootstrap maintenance | `partial`, wrong owner | Official plugin performs sequential startup and its own retry loop. | Move bounded bootstrap lifecycle into node. |
| Official `plugins.p2p.node` | `partial` | Does not configure DHT/Rendezvous/AutoNAT roles or persistent storage and duplicates bootstrap maintenance. | Reduce to configuration/dependency adapter over complete node lifecycle. |
| Diagnostics | `partial` | Snapshots exist, but control loops use projections and missing protocol lifecycle leaves health ambiguous. | Keep diagnostics read-only and expose effective mode, state, limits and degradation causes. |

### 4.1 Donor-First Completeness And Production Profiles

The historical baseline matrix above cannot prove completeness by itself: it
only lists surfaces already noticed in Forge at the start of hardening. The
current implementation state lives in the machine-readable feature inventory.
The authoritative reviewed scope catalog is
`tests/libp2p_interop/p2p_donor_capabilities.json`. It is pinned to the same
libp2p specs, Go and Rust revisions as the fixture matrix and must classify
every donor capability before implementation support is evaluated.

Each profile duplicates its reviewed capability IDs as a scope lock. The gate
compares that lock with the entries, validates donor paths against pinned
checkouts when available and maps every existing Forge feature exactly once.
This catches accidental omission and drift; semantic completeness remains a
donor-audit and independent-review responsibility because source text cannot be
classified mechanically without reimplementing the donor architecture.

Forge uses explicit production profiles rather than claiming every donor crate:

| Profile | Production boundary | Gate |
|---|---|---|
| Native | TCP/Yamux and QUIC, secure identity, adaptive dialing, autonomous discovery/routing, reachability, relay/path management, bounded resources and GossipSub | Stage 8 |
| Private network | TCP/Yamux plus a transport PSK layer before the normal secure channel, autonomous routing/pubsub and a fingerprinted mDNS namespace | Stage 8 |
| Browser transport | WebSocket `/ws` and `/wss` first; WebTransport and WebRTC require separate decisions | Stage 9 and later |
| Experimental/legacy | HTTP transport, Fetch, UDS, Perf, Floodsub, Mplex, plaintext, SECIO and Relay v1 are explicitly deferred, application-owned, test-only or rejected | Never implied by native readiness |

The private-network profile is not an alias for every native transport. Its
scope lock deliberately selects TCP/Yamux plus routing, discovery and pubsub
under the PSK transport layer; QUIC, Circuit Relay and DCUtR are excluded until
a donor-backed PSK-compatible design exists. AutoNAT and UPnP require one
explicit private-profile Internet-egress policy. Stage 8 evaluates that explicit
profile rather than inheriting unsupported native paths.

The first donor-first audit found these missing or incomplete host mechanisms:

| Capability | Why it matters | Delivery |
|---|---|---|
| mDNS | Public mDNS has Go/Rust interop; private fingerprinted mDNS is Go-compatible and carries an explicit Rust limitation. | Stage 6, `forge-p2p-mdns-v1` |
| DNSAddr | Resolves TXT records containing complete peer multiaddrs; ordinary DNS host lookup is not equivalent. | Stage 6, `forge-p2p-address-resolution-v1` |
| Observed-address manager | Requires independent observations, confidence and expiry before publishing an external address. | Stage 6, `forge-p2p-reachability-v1` |
| UPnP | Optionally owns native NAT mappings and their renewal/loss lifecycle; private use requires explicit Internet egress. | Stage 6, `forge-p2p-nat-mapping-v1` |
| Private network PSK | Isolates a deployment through a transport PSK layer before the normal secure-channel handshake; it is not a negotiated protocol ID. | Stage 6, `forge-p2p-private-network-v1` |
| Connection gater | Rejects at peer dial, address dial, accept, secured identity and upgraded-connection stages. | Stage 6, `forge-p2p-host-protection-v1` |
| Full resource scopes | Bounds memory, file descriptors, transient work and services in addition to sessions/streams/bytes. | Stage 6, `forge-p2p-host-protection-v1` |
| Adaptive dialing | Happy Eyeballs and UDP/IPv6 black-hole state avoid serial latency and repeated known-bad paths. | Stage 6, `forge-p2p-address-resolution-v1` |
| Typed host events | Exposes address, connection, reachability and path changes without polling diagnostics as control state. | Stage 6, `forge-p2p-reachability-v1` |
| Modern GossipSub | Scoring/mesh repair and v1.0 fallback are separate from v1.2/v1.3/Partial Messages extensions. | Stage 6, `forge-p2p-gossipsub-scoring-v1` then `forge-p2p-gossipsub-extensions-v1` |
| P2P WebSocket | Enables proxy/browser-compatible `/ws` and `/wss` transport. Parsing a multiaddr is not transport support. | Stage 9, `forge-p2p-websocket-v1` |

WebTransport, WebRTC, HTTP transport and other active/working drafts remain
visible in the manifest even when they do not gate the native profile. Legacy
protocols are rejected explicitly. Adding a new donor revision or support claim
therefore requires reclassification instead of silently expanding or shrinking
the meaning of "production libp2p".

### 4.2 Routing Table And Peer Store Are Different Components

The Kademlia routing table is mandatory whenever DHT client or server mode is
enabled. It is the bounded, continuously maintained operational view used to
seed iterative queries and answer closest-peer requests. The peer store is a
durable historical repository of identities, addresses, observations and
protocol metadata. It must not be queried as the routing algorithm.

The production routing table must provide:

- donor-consistent XOR-prefix or k-bucket organization with configured `k`;
- explicit admission of identified and successfully queried DHT server peers;
- bounded capacity, replacement and liveness/failure policy;
- efficient closest-peer lookup without materializing all known peers;
- bucket refresh timestamps and random-target generation;
- safe updates from successful outbound queries and accepted inbound DHT peers;
- a bounded snapshot for diagnostics, not a mutable public control surface.

Startup may hydrate candidates from the persistent peer store, but admission is
revalidated and capped by routing-table policy. Runtime routing changes update
the in-memory table first and persist durable observations through the bounded
persistence path.

### 4.2 False-Positive Test Coverage

The present suite contains tests that prove isolated classes or wire messages
while leaving the production feature inactive. Confirmed examples include
`dht::routing_table`, `hole_punch::attempt`, resource-manager stream/dial
methods, manual discovery calls and DHT value-record codec paths.

The hardening program must add a feature-to-lifecycle inventory gate. For every
advertised protocol or capability it records:

- owner and normal activation point;
- configuration and disable behavior;
- resource reservations and release path;
- persistence and restart behavior, if stateful;
- maintenance task and shutdown owner;
- diagnostics and metrics;
- raw-node, plugin and donor-interoperability tests.

An isolated public component with no production owner is a defect, not future
proofing. It must be integrated, made private to a real owner, or removed.

## 5. Priority Findings

### P0: Production Node Startup And Persistence

`forge_net_p2p` correctly requires a persistent peer store outside insecure
test mode. The official plugin neither exposes a peer-store path/backend in its
configuration nor supplies one when constructing the node. Consequently, the
configured plugin cannot currently satisfy the production node contract; its
integration tests use insecure test mode.

Required outcome:

- expose a product-neutral persistent peer-store configuration through an
  ObjectDB-backed adapter rather than another backend-specific database surface;
- let the node own peer-store use through its lifecycle, while the plugin maps a
  configured named DB store or injects a backend;
- reject invalid or unwritable storage before network admission opens;
- retain learned peers, endpoint observations, DHT/provider records,
  Rendezvous registrations, relay reservations and backoff state across restart;
- add a secure official-plugin startup/reopen test without
  `allow-insecure-test-mode`.

#### Iteration 1: ObjectDB Peer Store Foundation

The current RocksDB peer-store backend is an interim implementation, not the
target production architecture. It directly links `forge_rocksdb`, owns a
private binary codec and key-prefix layout, serializes synchronous database
operations under one process-local mutex and performs whole-prefix scans for
nearest DHT peers and Rendezvous discovery. The existing
`dht::routing_table` is not connected to the node and also computes nearest
peers by scanning and sorting every entry. Replacing the RocksDB calls with
ObjectDB calls without correcting these operational paths is insufficient.

The first implementation iteration therefore establishes the following
boundary:

```text
forge::net::p2p::node
  |-- bounded in-memory peer directory and Kademlia routing state
  `-- peer_store persistence port
        `-- ObjectDB-backed persistence adapter
              `-- forge.db.core driver (MDBX or RocksDB)
```

`forge_net_p2p` continues to own peer-store domain records, validation, expiry,
scoring and the in-memory backend used by focused tests. It must not depend on a
concrete DB driver or on an official plugin. ObjectDB persistence belongs in a
separate focused adapter boundary so in-memory and embedded consumers do not
acquire an unconditional storage dependency. The exact target/component name is
chosen in the implementation plan under the normal `create-library` rules.

The ObjectDB adapter owns private P2P application models in a dedicated object
family. It must not allocate IDs from the global DB Object system space and must
not expose its model vocabulary as product API. Its persisted schema covers:

- peer identity, verified Identify data, endpoints, capabilities and signed peer
  records;
- endpoint and peer success/failure observations, latency, score, reachability
  and backoff state;
- DHT provider records keyed by content key and provider, with expiry;
- Rendezvous registrations indexed by namespace, sequence and peer, with
  expiry;
- relay reservation metadata and the small monotonic state required by durable
  registration ordering.

ObjectDB transactions must make read-modify-write updates and sequence/state
changes atomic. Persisted models carry an explicit schema version and a defined
upgrade or reset policy; adopting ObjectDB does not make migration automatic.
DB Revision and BlobDB are not required for peer state.

The durable store is not the DHT query engine. On startup the node loads a
bounded, policy-selected set of valid records and hydrates its in-memory peer
directory and a donor-consistent Kademlia bucket table. New observations update
those operational indexes directly. Nearest-peer selection, scoring, bootstrap,
AutoRelay and maintenance work use bounded in-memory indexes rather than a
database scan. Startup hydration and expiry pruning are paged and bounded by
configured limits; neither operation may materialize an unbounded peer store.

The persistence contract must be asynchronous. An ObjectDB awaitable must never
be hidden behind a blocking wait on an Asio runtime thread. Writes whose success
is acknowledged to a remote peer, including accepted provider or Rendezvous
registrations, are durably completed before the response. High-frequency
observational updates may be coalesced only through a node-owned bounded queue
with explicit backpressure, failure diagnostics and deterministic shutdown.

For the official application path, `plugins.p2p.node` references a dedicated
named Object layer supplied through `plugins.db.store`, registers the private
models during `after_initialize` and injects the prepared persistence adapter
before node startup. The node opens no listener until hydration succeeds.
Programmatic `forge_net_p2p` consumers may inject an already prepared adapter
without using either official plugin. The P2P plugin does not expose raw ObjectDB
handles and does not reimplement DB driver configuration or lifecycle.

Completion of this iteration requires:

- removal of the direct `forge_rocksdb` dependency and
  `peer_store::make_rocksdb_backend()` from `forge_net_p2p`;
- removal of the hand-written RocksDB codec, key prefixes, sequence storage and
  backend-specific exception translation;
- retention of a deterministic in-memory backend for tests;
- atomic ObjectDB commit/rollback and reopen coverage for every persisted model;
- bounded startup hydration, expiry and pruning tests with a store larger
  than the live routing-table capacity;
- proof that nearest-peer and maintenance work do not scale linearly with total
  persisted peer records;
- secure official-plugin startup and reopen through the named DB Store path;
- parity tests over both MDBX and RocksDB DB Core drivers where available.

This iteration does not by itself complete secure production startup. Stable
identity-source delivery remains the next required dependency and is handled by
the separate identity finding below.

### P0: Operational Kademlia Routing State

The current DHT query and server paths call
`peer_store::closest_routing_peers()`. The RocksDB backend scans and decodes all
persisted peer records and then sorts them by XOR distance. The disconnected
`dht::routing_table` performs the same full materialization over a flat map and
does not apply its failure observations to admission, replacement or eviction.

This violates the purpose of a Kademlia routing table and makes query cost
depend on durable peer history rather than configured live routing capacity.

Required outcome:

- make one node-owned donor-consistent routing table the source for query seeds
  and closest-peer responses;
- organize entries by XOR-prefix distance with bounded buckets and replacement
  candidates;
- admit only authenticated, identified peers that advertise and prove the
  appropriate DHT server role;
- update liveness from successful queries, failed dials and closed sessions;
- run startup and periodic bucket refresh, including lookup near the local ID;
- persist observations through the peer-store boundary without making DB
  queries part of the routing hot path;
- prove bounded memory and sublinear closest-peer work when durable history is
  much larger than live routing capacity.

The existing flat implementation may be replaced rather than preserved for
source compatibility. A misleading public routing-table abstraction is not a
compatibility requirement while the P2P contract is being hardened.

### P0: Protocol Claim Integrity

Forge decodes DHT `PUT_VALUE` and `GET_VALUE`, but the server does not implement
a value-record store. `PUT_VALUE` echoes the received record and `GET_VALUE`
returns closer peers without a value. This can appear interoperable at the
codec level while violating application expectations.

Required outcome:

- decide explicitly whether Forge DHT v1 owns generic value records or only
  peer and provider routing;
- if supported, add validator/selector policy, bounded durable storage,
  expiry, conflict handling and live Go/Rust interoperability;
- if unsupported, reject the operations deterministically, remove unsupported
  public vocabulary where possible and never advertise value-store support;
- prohibit protocol handlers that return nominal success without completing
  the operation's state contract.

### P1: Production Resource Policy

The low-level node has bounded defaults for sessions, streams, protocols,
pending dials, malformed messages, relay traffic, transport queues and discovery
messages. However, generic stream and dial reservation methods are only tested
in isolation and are not called from node stream/dial paths. The generic queued
byte limit is not consumed by generic resource accounting. The official plugin
also exposes only a small subset of the limits.

Required outcome:

- expose structured, product-neutral configuration for session totals and
  directions, sessions per peer and pending session admission;
- expose stream totals, per-peer/per-protocol limits, dial budgets, malformed
  message budgets and relay byte/queue limits;
- expose transport queue/buffer limits and Peer Exchange message/record limits;
- expose discovery concurrency, result, timeout and refresh limits without
  duplicating protocol-specific configuration records;
- expose API stream item, buffered-byte, idle, shutdown and inflight limits;
- replace manual counter pairs with move-only reservations whose destruction
  closes every success, cancellation and exception path;
- acquire stream reservations for every inbound and outbound protocol stream;
- acquire and release dial reservations around every direct and relay dial;
- connect generic byte accounting to the buffers it claims to limit, or remove
  that limit and rely on explicitly owned transport/protocol queues;
- validate cross-field invariants before constructing the node;
- report the effective limits in diagnostics so deployment configuration can be
  audited;
- keep conservative bounded defaults; configuration is not permission to make
  queues or admission unbounded.

Not every private implementation constant needs a public setting. A value must
be configurable when it changes deployment capacity, exposure or failure
behavior.

### P1: Bootstrap Startup And Maintenance

The plugin currently implements bootstrap maintenance itself and attempts every
configured endpoint sequentially inside `startup()`. Each failed endpoint can
consume its full connect timeout, so application startup latency grows linearly
with the bootstrap list. The maintenance loop then scans a full diagnostics
snapshot once per bootstrap peer and uses deterministic retry delays without
jitter. This logic belongs in the node because programmatic users require the
same behavior.

Required outcome:

- make disconnected startup versus required initial connectivity an explicit
  node policy supplied by the deployment;
- bound the complete initial-bootstrap phase by one startup budget;
- attempt bootstrap peers with bounded parallelism rather than sequentially;
- move non-required retry work to a node-owned managed maintenance task;
- add randomized jitter to exponential backoff to prevent synchronized retry
  storms after a shared bootstrap failure;
- use an allocation-free internal session lookup rather than constructing
  diagnostics snapshots in the control loop;
- protect successful bootstrap sessions as today and release protection when a
  configured bootstrap entry is removed;
- cancel in-progress DNS/connect waits and maintenance timers deterministically
  during shutdown.

Diagnostics snapshots remain an operator-facing projection. They must not be
used as an internal point-query API.

### P1: Identity Material Ownership

The plugin accepts certificate and private-key PEM as complete config string
values. Schema redaction prevents ordinary diagnostics from printing the key,
but there is no node-owned file, encrypted-file or secret-provider source
contract. A stable PEM supplied by the application still produces a stable
identity; the gap is secure operational delivery, not identity derivation.

Required outcome:

- support a stable identity source suitable for mounted secrets and encrypted
  local configuration;
- reuse Forge secret-source/loading mechanics or extract a neutral reusable
  component instead of adding a P2P-only file parser;
- load and validate identity material before opening listeners;
- avoid copying private material into diagnostics, generated examples or error
  context;
- define reload behavior explicitly; v1 may require restart rather than rotate
  a live libp2p identity;
- preserve programmatic construction for callers that already own a secure
  identity provider.

### P1: Identify And Remote Capability Truth

Forge responds to Identify and accepts Identify Push, but an ordinary bootstrap
or direct connection does not initiate Identify. Identify Push is not emitted
when the local address/protocol set changes. New session records are initially
populated from local capabilities rather than verified remote capabilities.

Required outcome:

- run bounded Identify after each newly established session;
- treat TLS/Noise peer authentication and Identify protocol advertisement as
  separate facts;
- populate session and peer-store capabilities only from the remote document;
- emit Identify Push after a relevant local endpoint or protocol change;
- reject or quarantine inconsistent peer/address/capability observations;
- prove Forge-to-Forge capability learning through official plugins.

### P1: Discovery Lifecycle And Topology Maintenance

Kademlia DHT, provider records and Rendezvous are implemented in
`forge_net_p2p`, but the node has no autonomous discovery lifecycle and the
official plugin neither configures one nor calls `async_refresh_discovery()`.
Discovery results alone are also insufficient: the node must maintain a bounded
set of useful sessions.

Required outcome:

- add explicit node options for DHT client/server and Rendezvous
  client/server roles, mapped by the plugin;
- let the node start an initial discovery pass after bootstrap connectivity;
- run one node-owned cancellable refresh loop with TTL-aware refresh, bounded
  parallelism, retry and jittered backoff;
- maintain configurable low/target/high peer watermarks;
- select and dial scored discovered peers without displacing protected
  bootstrap sessions;
- expire stale observations and disconnect obsolete unprotected sessions;
- stop discovery and dialing deterministically before node teardown.

Products may disable decentralized discovery for a deliberately static
deployment, but the mode must be explicit and diagnostics must report it.

### P1: Peer Exchange Activation

The Peer Exchange protocol can answer inbound requests and its capability is
advertised, but the node never schedules an outbound exchange. Therefore it
does not currently expand either a programmatic or official-plugin topology.

Required outcome:

- let the node request Peer Exchange from a bounded subset of identified
  compatible peers;
- trigger it during initial topology formation and at a bounded refresh rate;
- retain existing endpoint sanitization, record limits and per-peer backoff;
- feed accepted records into the same topology manager as DHT and Rendezvous;
- prove that a node learns and connects to a non-bootstrap peer.

### P1: Reachability, AutoRelay And Hole Punching

AutoNAT handlers and manual probing exist, while AutoRelay maintenance is
started automatically. The node does not autonomously schedule reachability
probes, and the plugin cannot supply their policy. AutoRelay is normally starved
of candidates because Identify, DHT, Rendezvous and Peer Exchange do not
populate the production peer set. Circuit Relay v2 and DCUtR hole punching
therefore work mainly in focused raw-node/interoperability tests.

Required outcome:

- configure trusted AutoNAT observers and bounded re-probe policy through node
  options, keeping v1 node-level reachability separate from v2 address-level
  evidence;
- aggregate observations instead of trusting one peer;
- publish effective public/private/unknown reachability in diagnostics;
- feed verified relay-capable peers into the existing AutoRelay loop;
- renew and replace reservations before expiry;
- attempt DCUtR only with valid relay context and preserve relay fallback;
- prove direct, relayed and hole-punched official-plugin paths, including
  reservation expiry and peer loss.

The existing Relay, AutoRelay and DCUtR mechanics should be completed through
node-owned orchestration rather than duplicated in plugins.

### P2: Liveness And Operational Maintenance

Ping supports inbound responses and explicit RTT measurements, but the node has
no optional periodic health-sampling policy. Connection closure is observed,
yet diagnostics do not provide an actively maintained peer-health view.

Required outcome:

- define whether a deployment enables bounded periodic Ping sampling;
- avoid a mandatory ping storm for idle or very large networks;
- feed successful RTT and failures into existing scoring/backoff state;
- expose last successful contact, RTT and failure/backoff status;
- distinguish responder-only Ping support from active health monitoring in docs.

### P2: API Call And Peer Admission Policy

Forge API wire v2 has a negotiated 60-second idle timeout and real per-call
deadline timers. A zero total deadline intentionally permits long-lived streams;
it does not disable idle detection or cancellation. The node plugin nevertheless
exposes only a small part of the stream policy, defaults its total deadline to
zero and does not let a publishing plugin select the existing P2P
`require_known_peer` topology guard.

Required outcome:

- document total deadline, idle timeout and open deadline as separate concepts;
- let a published API choose bounded stream/session limits appropriate to that
  contract;
- allow an explicitly configured total deadline without forcing one onto
  legitimate long-lived streams;
- prove that abandoned and stalled calls release inflight capacity while other
  calls keep the multiplexed session active;
- expose topology admission such as `require_known_peer` only as an optional
  coarse guard;
- keep authenticated remote `peer_id` in request metadata for product-owned
  authorization and quotas;
- state explicitly that presence in peer store, transport reachability and
  successful mTLS/Noise identity verification do not by themselves authorize an
  application operation.

Product authorization remains in the published API implementation or its
binding policy. The node plugin must not turn peer discovery into an implicit
access-control list.

### P2: Maintenance Path Efficiency

Node-owned background maintenance must use direct internal state and bounded
work rather than operator diagnostics projections or repeated whole-store
materialization.

Required outcome:

- use direct internal point queries for active session, peer and reservation
  state needed by maintenance;
- ensure one maintenance tick is bounded by configured work limits rather than
  total peer-store size;
- avoid holding the node mutex while constructing large projections or awaiting
  network operations;
- instrument maintenance duration, attempted work, skipped work and backpressure;
- add scale regressions with many peers, sessions and bootstrap entries.

### P2: GossipSub Topology Completeness

The official PubSub plugin is operational over connected peers and its
heartbeat, validation and resource bounds are active. Its mesh cannot become
independent of configured bootstrap/static sessions until discovery and
topology maintenance are completed.

Required outcome:

- retain the working wire, validation, cache and heartbeat paths rather than
  replacing them with a second implementation;
- allow the shared topology manager to supply identified compatible peers;
- complete donor-consistent scoring inputs, decay and retention;
- apply score thresholds to message admission, gossip, publishing, GRAFT and
  PRUNE behavior;
- select mesh survivors and opportunistic graft candidates by score and
  outbound diversity instead of deterministic container order;
- verify mesh repair after bootstrap loss and discovered-peer replacement;
- add Sybil, invalid-message, duplicate-spam and low-score recovery tests;
- keep PubSub delivery explicitly non-durable and non-exactly-once.

## 6. Ownership Boundaries

`forge_net_p2p` owns protocol codecs, peer store, Identify, Ping, AutoNAT, DHT,
Rendezvous, Relay, DCUtR, GossipSub, scoring, resource-manager mechanics and the
autonomous maintenance loops that keep those mechanisms operational. Its node
lifecycle must be complete for both direct library consumers and plugins.

`plugins.p2p.node` owns configuration mapping, dependency preparation, the
application-owned shared-node lifetime and narrow local APIs for other official
plugins. It calls the node lifecycle but does not reimplement bootstrap,
discovery, reachability, relay, liveness or gossip maintenance. It must not
expose an unrestricted raw-node escape hatch.

Product plugins own authorization, network/realm membership, application
protocols and business routing. `plugins.p2p.resolver` continues to open a typed
API on an already known peer; it does not become peer or content discovery.

## 7. Donor And Forge Reuse Discipline

Each implementation block starts with a focused donor note that names the exact
specification and Go/Rust source paths reviewed, accepted behavior, intentional
Forge differences and tests carrying each invariant.

Minimum donor baselines are:

- `libp2p/specs` for protocol state and wire requirements;
- `go-libp2p-kad-dht` and Rust libp2p Kademlia for routing-table admission,
  refresh, query and provider-record behavior;
- Go/Rust Identify services for session-time Identify and Push propagation;
- Go libp2p resource manager for scoped resource ownership and denial paths;
- Go libp2p GossipSub for score calculation, thresholds, mesh diversity and
  adversarial behavior;
- Go/Rust implementations of Rendezvous, AutoNAT, Circuit Relay v2 and DCUtR
  for lifecycle and interoperability.

Forge keeps its own C++23 implementation and public API. Donor code is not a
runtime dependency, but its externally observable invariants are not optional.
Intentional deviations require rationale and an interoperability test.

Before creating any new helper, the implementation must inspect existing Forge
Asio scheduling, gates, cancellation, transport sessions, crypto identity,
configuration, diagnostics, ObjectDB and plugin lifecycle components. P2P-local
thread pools, ad-hoc detached task ownership, duplicate backpressure queues,
parallel storage abstractions and custom secret loaders are forbidden when the
corresponding Forge facility exists.

## 8. Implementation Program

### Stage 1: Support-Claim Freeze And Inventory

- freeze new P2P and Swarm features;
- classify every public P2P type, method, option, capability and protocol using
  the state vocabulary in this document;
- correct READMEs, diagnostics and donor manifests that imply stronger support
  than the production path provides;
- add structural inventory coverage that rejects unowned protocol handlers,
  capabilities and public implementation components;
- decide the supported DHT value-record scope before further DHT work.

### Stage 2: Peer State Foundation

- replace direct RocksDB ownership with the ObjectDB persistence adapter;
- introduce bounded in-memory peer directory and routing candidates;
- make hydration, expiry, persistence queues and shutdown bounded;
- add secure identity-source delivery and production plugin startup.

### Stage 3: Host Lifecycle And Resource Ownership

- establish one node-owned start, maintenance, stop and join lifecycle;
- move bootstrap maintenance out of the plugin;
- integrate Identify and Identify Push at session boundaries;
- integrate session, dial, stream and byte reservations with RAII-style
  ownership;
- expose and diagnose effective resource policy.

### Stage 4: Production Kademlia

- replace the flat orphan routing table with bounded donor-consistent routing
  state;
- integrate client/server roles, admission, query seeds and server responses;
- add bucket bootstrap, refresh, failure and replacement behavior;
- add provider registration lifetime, republish, withdrawal and expiry;
- implement or explicitly remove generic value-record support.

### Stage 5: Unified Topology Discovery

- make DHT, Rendezvous and Peer Exchange feed one scored topology manager;
- maintain peer low/target/high watermarks with bounded dialing;
- expire stale observations and preserve protected peers;
- prove discovery beyond bootstrap through raw node and official plugin.

### Stage 6: Donor Parity, Reachability And Path Management

- build one observed-address/effective-reachability service from independent
  Identify, host-local v1 node-level and v2 address-level AutoNAT observations,
  expiry and bounded periodic Ping liveness; Ping and AutoNAT client/service
  wire roles retain independent Go/Rust gates;
- add public Go/Rust mDNS, private fingerprinted mDNS with its Rust limitation,
  and DNSAddr discovery without parallel topology loops;
- add the PSK transport layer as an explicit TCP/Yamux private profile, with no
  negotiated `/pnet` ID, QUIC, Relay or DCUtR; AutoNAT and UPnP need explicit
  private-profile Internet egress;
- add Happy Eyeballs, IPv6 black-hole state for native/private profiles, UDP
  black-hole state for the native profile and typed host-state events;
- preserve Circuit Relay v2 client/transport semantics, keep AutoRelay candidate
  and reservation orchestration host-local, prove the opt-in public Relay v2
  service as a Go/Rust service-client role, and unify
  DCUtR/coordinated-dial-and-port-reuse ownership while preserving relay
  fallback; reject deprecated `/libp2p/simultaneous-connect` negotiation;
- add staged connection gating and memory, file descriptor, transient and
  service resource scopes;
- complete GossipSub scoring, thresholds, decay, mesh diversity, v1.0 fallback,
  v1.2 and v1.3 first-RPC extension advertisement, unknown-extension ignore and
  capability matching, plus opt-in Partial Messages implementation;
- deliver only these 13 focused implementation PRs, with no new plugin-owned
  network loops, ordered as PR0 through PR12:
  `forge-p2p-stage6-roadmap-v1`, `forge-chrono-v1`,
  `forge-p2p-host-protection-v1`, `forge-crypto-xsalsa20-v1`,
  `forge-p2p-private-network-v1`, `forge-p2p-address-resolution-v1`,
  `forge-p2p-reachability-v1`, `forge-p2p-mdns-v1`,
  `forge-p2p-nat-mapping-v1`, `forge-p2p-autorelay-v1`,
  `forge-p2p-path-management-v1`, `forge-p2p-gossipsub-scoring-v1` and
  `forge-p2p-gossipsub-extensions-v1`.

The exact order, dependency DAG and permitted capability owners are locked by
`stage_6_pr_registry` in `p2p_donor_capabilities.json`; the roadmap, chrono and
crypto prerequisite PRs deliberately own no capability entries. The
`interop_acceptance_registry` is source-only registration. It cannot produce an
execution PASS: the CMake promotion target launches the canonical enabled runner
and immediately validates the exact artifact from that invocation, including
clean exact-`HEAD`, manifest, runner argv/roots, tool and binary provenance,
effective enabled roles and capability dependencies. Native QUIC, native
TCP/Yamux and private TCP/Yamux+pnet are separate evidence scopes; an enabled
optional run is not a default-activation claim. An absent or stale provenance is
`NOT_RUN`; a canonical runner nonzero exit or recorded failure is `FAILED`.
`limited` is allowed only for a registry-classified limitation and yields
`PASS_WITH_DOCUMENTED_LIMITATIONS`. SHA-256 evidence is tamper-evident local
consistency evidence, not signed remote attestation or malicious-artifact
unforgeability. The inventory checker remains the paired source-manifest gate;
PR0 itself remains source-only. Every donor acceptance scenario names one
closed `forge.p2p.evidence.<scenario-id>.v1` contract in the manifest registry.
The inventory checker requires exact, unique coverage, and the acceptance
checker has a semantic validator for every registered contract: a status,
activation or capability label alone cannot establish PASS. Private consumers
must name the PSK capability except the root PNET scenario; private AutoNAT
also requires allow-dialback evidence plus an independently hashed deny-egress
control proving rejection without an external dial.
The acceptance checker derives `--transport` from the manifest contract rather
than raw artifact metadata: native QUIC is `quic`, native TCP/Yamux is `tcp`
except the exact TLS contract's `tcp-tls`, and private TCP/Yamux+pnet is
`tcp-pnet` for both endpoints. The PNET root also requires indexed missing-key
and mismatched-key controls that reject before Identify/application streams.
Future contract schemas require correlated wire fields and separately indexed
negative controls; they block promotion until the canonical runner and fixtures
emit those execution controls. A manifest label, activation flag or source-only
registration cannot establish behavior.
The current native contracts use the same rule: launcher `--transport` is not
endpoint proof. QUIC promotion requires an observed `/quic-v1` connection, its
authenticated remote peer ID and Identify evidence. Current multistream, Noise
and TLS promotion requires a result-owned ordered upgrade transcript and exact
security/muxer selections. Rust's current public Swarm events expose peer and
remote multiaddr but not the selected security/muxer phases, so that absence is
promotion-blocking rather than a reason to copy requested CLI configuration into
the result. Current Kademlia promotion requires a correlated Amino provider,
independent querier, returned peer, address, stream and query proof; a provider
count does not establish wire behavior.

The manifest's inline-muxer evidence names Go TLS and Noise support plus the
pinned Rust TLS fixed-ALPN and Noise extension/fallback paths. Its GossipSub
v1.3 evidence names Go `extensions.go` first-RPC behavior and Rust
`behaviour.rs` advertisement handling in addition to codec/spec sources.
The coordinated port-reuse trace additionally pins Rust
`swarm/src/dial_opts.rs` and `transports/tcp/src/lib.rs` alongside the existing
connection-pool and Go direct-dial sources; it remains source-only until its
concurrent fixture/control exists.

Private-network and address-resolution depend on host protection; path
management also depends on address resolution. Reachability has no direct
address-resolution dependency because AutoNAT, Ping and observed-address policy
operate on configured or Identify-observed endpoints and do not resolve
`/dnsaddr`. In the private profile, every AutoNAT lifecycle/client/service role
and UPnP depends on the Forge-owned Internet-egress policy; native roles do not.
Private fingerprinted mDNS and that egress policy are Forge extensions informed
by donor patterns and limitations, not donor-spec claims. Rendezvous is
Rust-supported with an explicit Go limitation: no
official Go rendezvous behaviour donor is pinned, so it cannot be presented as
Go-compatible.

`forge_chrono` supplies only formatting and parsing for explicit
`std::chrono` timestamps; it owns no clock, scheduler, deadline/backoff policy
or P2P lifecycle.
`forge-crypto-xsalsa20-v1` requires pinned `libsodium`; address resolution over
`net_dns` requires pinned `c-ares`. Those dependency checks and focused tests
belong to their runtime PRs. Official plugin configuration mapping is Stage 7
work after the raw-node contracts are stable.

### Stage 7: Official Plugin And Operational Surface

- expose complete validated production configuration and host/protocol metrics
  without duplicating node records;
- map the validated Stage 6 raw-node options into official plugin configuration;
- remove all plugin-owned network maintenance;
- expose narrow typed contributions and read-only diagnostics;
- prove configuration, restart and shutdown parity with programmatic nodes.

After Stage 6 completion, Content Swarm development may proceed in parallel
under the boundaries in section 10, without delaying P2P delivery.

### Stage 8: Native Production Proof And Release Gate

- run scale, churn, cancellation, malformed-input and resource-exhaustion
  suites;
- run live Go and Rust interoperability for every enabled protocol;
- demonstrate zero `stub`, `orphan`, unintended `manual-only`, `partial` or
  `unverified` inventory entries;
- update support documentation only from passed evidence;
- exercise Content Swarm integration without replacing raw-node/plugin proof;
- declare `forge_net_p2p` production-ready before permitting Content Swarm
  production use, which also requires Swarm's own acceptance gates.

The declaration applies only to the native TCP/QUIC and private-network
profiles whose required manifest entries pass. It does not imply browser
transport support.

### Stage 9: P2P WebSocket And Browser Transport

- implement `/ws` and `/wss` dial/listen, secure upgrade, proxy/backpressure
  behavior, resource ownership, explicit WSS certificate/AutoTLS policy and
  deterministic shutdown;
- prove Go and Rust transport interoperability rather than relying on
  multiaddr parse fixtures;
- keep WebTransport and WebRTC as separately planned future-profile work until
  their own contracts and evidence are approved.

The earlier high-level order is therefore replaced by these dependency-ordered
stages. Each stage should be delivered as one or more focused PRs; a stage is
complete only after its exact-head review and evidence gates pass.

## 9. Production Acceptance Gates

- A secure node starts with real identity material and persistent peer storage.
- Identity material can be supplied without embedding private PEM directly in a
  normal YAML document, and is absent from diagnostics and errors.
- Effective resource, transport, discovery and API stream limits match validated
  plugin configuration.
- Initial bootstrap work has bounded concurrency and one total startup budget;
  latency does not grow as the sum of every endpoint timeout.
- Simultaneous bootstrap loss does not produce synchronized retry storms.
- A programmatically constructed node performs the same configured bootstrap,
  discovery, reachability and relay maintenance without `plugins.p2p.node`.
- The official plugin contains no independent protocol maintenance loops and
  delegates lifecycle to that node behavior.
- Three nodes given only bootstrap entry points discover and connect to each
  other within bounded time.
- Nodes on one LAN discover each other through mDNS without bootstrap or
  Internet access, including the fingerprinted private-network namespace.
- DNSAddr expansion is bounded, cycle-safe and feeds the same authenticated
  topology path as configured multiaddrs.
- External addresses are advertised only after configured-listen, signed,
  independently observed or owned NAT-mapping evidence reaches its policy.
- Private-network nodes reject a mismatched or absent transport PSK before
  normal secure-channel negotiation and never negotiate a `/pnet` protocol ID.
- Happy Eyeballs, native/private IPv6 black-hole state and native UDP
  black-hole state improve path selection without permanently suppressing
  recovered transports.
- A restarted node restores valid peer/discovery state and safely expires stale
  records.
- Peer persistence passes the same reopen and transaction fixtures over MDBX and
  RocksDB, while `forge_net_p2p` has no direct dependency on either driver.
- Nearest-peer selection uses bounded in-memory Kademlia state and does not scan
  or sort the complete persisted peer set.
- Routing-table capacity, bucket refresh, replacement and failure behavior are
  donor-traced and remain bounded under hostile peer churn.
- ObjectDB hydration and expiry processing remain bounded when durable peer
  history exceeds live routing-table capacity.
- DHT peer/provider lookup and Rendezvous discovery run through official plugin
  lifecycle and stop cleanly.
- DHT provider records renew while owned, disappear after withdrawal/expiry and
  survive restart without stale indefinite advertisement.
- DHT `PUT_VALUE`/`GET_VALUE` are either fully implemented with validation and
  persistence or deterministically unsupported; echo stubs are absent.
- Peer Exchange discovers a non-bootstrap node without accepting non-routable or
  identity-mismatched endpoints.
- Remote session capabilities match the peer's actual advertised protocols.
- Public and private reachability lead to the expected direct, relay and DCUtR
  paths.
- Connection gating and memory, file descriptor, transient and service scopes
  reject work before unbounded allocation and release reservations on every
  terminal path.
- GossipSub scoring and mesh repair pass v1.0 fallback and advertised
  v1.2/v1.3 negotiation fixtures, including opt-in Partial Messages behavior.
- Loss of bootstrap, relay or a discovered peer repairs topology without an
  unbounded retry/task/memory increase.
- GossipSub continues delivery after bootstrap loss when other mesh peers remain.
- GossipSub mesh admission, pruning and recovery use the configured score and
  outbound-diversity policy rather than peer container ordering.
- Long-lived API streams remain supported, while abandoned calls release
  inflight capacity under negotiated idle/deadline policy.
- Every accepted session, dial and stream owns a matching resource reservation
  that is released on success, cancellation, failure and shutdown.
- Peer-store membership alone never grants product authorization.
- Bootstrap and discovery maintenance do not construct full diagnostics
  snapshots or perform unbounded work per tick.
- Diagnostics identify disabled, idle, degraded and healthy discovery states.
- Live libp2p interoperability remains green for every enabled and applicable
  direction, proven by the CMake-owned exact-`HEAD` runner invocation and its
  immediate artifact validation rather than case registration, artifact replay
  or a source-inventory result; documented Go/Rust limitations do not become
  bilateral claims.
- The implementation inventory contains no `stub`, `orphan`, unintended
  `manual-only`, `partial` or `unverified` production surface.

## 10. Content Swarm Entry Gate

The maintainer-approved delivery order separates development from production.
It supersedes the earlier prohibition on all Swarm implementation until after
Stage 8; it does not declare the current P2P lifecycle production-ready.

### Parallel development gate: Stage 7

- Stage 6 is complete and its P2P contracts are fixed before Swarm development
  resumes alongside Stage 7;
- the scope covers content identifiers, manifests, file-backed storage,
  integrity verification, transfer scheduling, Forge API streaming and
  deterministic tests;
- network integration uses the official plugin as the validated Stage 7
  configuration and contribution surfaces become available; a temporary
  product-owned raw-node configuration path is not an alternative;
- Swarm does not implement missing discovery, provider renewal, relay, NAT
  traversal or network maintenance above the P2P layer;
- parallel development must not delay completion of P2P Stages 7 and 8.

### Joint validation: Stage 8

Run multi-process Swarm integration tests covering interrupted transfers,
recovery, slow peers, memory bounds and long-duration operation. These tests
supplement the mandatory raw-node, official-plugin and Go/Rust evidence; they
do not replace it.

### Production gate: after Stage 8

- P2P stages through Stage 8 are complete for the required deployment profiles;
- `forge_net_p2p` and `plugins.p2p.node` satisfy every production acceptance
  gate above;
- provider publication has an owned registration lifetime and automatic
  renewal/withdrawal;
- a node can discover non-bootstrap providers and open an application protocol
  through the official plugin without product-owned discovery loops;
- resource, cancellation and shutdown tests prove that content-sized workloads
  cannot bypass P2P admission;
- Swarm's own acceptance gates pass independently: P2P readiness alone does not
  authorize Swarm production use.

Stage 9 WebSocket support is not a prerequisite for native Swarm deployment;
browser transport support requires its separate gates.

Swarm owns content manifests, piece exchange and seeding intent. It reuses
the completed P2P provider discovery and Forge API streaming rather than
compensating for incomplete node mechanics.

## 11. Current Audit Anchors

These source anchors record the implementation evidence behind the current
matrix. They are not permanent API references and must be refreshed as each
phase lands:

- [`dht.cpp`](../../libraries/net/p2p/dht.cpp) contains the disconnected flat
  `dht::routing_table` and full-sort closest lookup;
- [`peer_store.cpp`](../../libraries/net/p2p/peer_store.cpp) contains the
  RocksDB full-prefix scan used for closest routing peers;
- [`node.cpp`](../../libraries/net/p2p/node.cpp) contains one-shot discovery,
  provider, Ping and reachability calls and starts only selected maintenance;
- [`node_impl.cpp`](../../libraries/net/p2p/node_impl.cpp) contains session
  establishment, DHT handlers, GossipSub heartbeat/scoring, AutoRelay and DCUtR;
- [`resource_manager.cpp`](../../libraries/net/p2p/resource_manager.cpp) and
  [`resource_manager.cppm`](../../libraries/net/p2p/include/forge/net/p2p/resource_manager.cppm)
  contain the isolated generic stream/dial counters;
- [`hole_punch.cppm`](../../libraries/net/p2p/include/forge/net/p2p/hole_punch.cppm)
  contains the orphan public attempt state;
- [`plugin.cpp`](../../plugins/p2p/node/plugin.cpp) contains plugin-owned
  bootstrap startup and maintenance activation;
- [`config.cpp`](../../plugins/p2p/node/config.cpp) shows the currently mapped
  node capabilities and the missing production discovery-role configuration;
- [`p2p_tests.cpp`](../../tests/quic_p2p/p2p_tests.cpp) contains both valuable
  protocol fixtures and isolated tests that must be supplemented by lifecycle
  evidence;
- [`donor_cases.json`](../../tests/libp2p_interop/donor_cases.json) is the donor
  support manifest whose claims must be recalibrated to the evidence standard.

## 12. Non-Goals

- product authorization or chain/network membership;
- content-provider key design, seeding policy or transfer scheduling;
- replacing Forge API or `plugins.p2p.resolver`;
- a parallel peer-state database or model outside the canonical `peer_store`
  boundary;
- DB Revision, BlobDB or product content storage inside the peer store;
- using ObjectDB queries as a substitute for an operational Kademlia routing
  table;
- unbounded background scanning, dialing, pinging or task creation.
