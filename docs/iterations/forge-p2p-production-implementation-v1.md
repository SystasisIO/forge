# Forge P2P Production Implementation v1

> **Status:** implementation roadmap.
>
> This document sequences the accepted findings in
> `forge-p2p-production-hardening-v1.md`. It does not replace that audit and
> does not by itself claim that a feature is production-ready.

## 1. Objective

Bring `forge_net_p2p` and the official P2P plugins to one production path in
which every advertised protocol is either:

- owned by the normal node lifecycle and proven through the official plugin;
- intentionally caller-driven and documented as such; or
- rejected as unsupported without a nominal-success stub.

Production means more than a compatible codec. The node must own bounded
state, activation, maintenance, resource reservations, persistence,
diagnostics and deterministic shutdown. Scope and implementation evidence have
different machine-readable owners:

- `tests/libp2p_interop/p2p_donor_capabilities.json` is the donor-first scope
  manifest. It separately records support requirement, default activation,
  Go/Rust interop applicability and the explicit roadmap decision for every
  pinned-donor capability;
- `tests/libp2p_interop/p2p_feature_inventory.json` records only Forge surfaces
  that exist in the current tree and their actual evidence state.

A roadmap entry is not an implementation claim. The inventory checker derives
its expected Forge feature IDs from the donor-first manifest rather than from
a local hardcoded allowlist. Each profile also carries an explicit capability
scope lock, checked against the classified entries, so accidental removal or
unassigned addition fails the gate. Semantic completeness still requires the
recorded donor audit and independent review; a source checker cannot infer
arbitrary protocol meaning from Go, Rust and specification text.

## 2. Invariants

- `forge::net::p2p::node` owns transport-neutral libp2p mechanics.
- Official plugins adapt configuration and Forge application dependencies;
  they do not implement a second host lifecycle.
- Product protocols own authorization and application semantics.
- Durable history is not an operational routing table.
- Network-controlled queues, caches, queries and maintenance batches are
  bounded and observable.
- Codec, component and donor fixture evidence cannot by themselves establish a
  production claim.
- Standard `/ipfs/kad/1.0.0` support means peer, provider and validated value
  operations. A provider-only profile must use a product-owned protocol ID and
  must not negotiate the standard full-profile ID.
- A handler must never return protocol success for an operation whose state
  contract was not performed.

## 3. Evidence Gates

Evidence is cumulative:

1. exact codec and malformed-input fixtures;
2. bounded state-machine and shutdown tests;
3. raw-node lifecycle integration;
4. official-plugin configuration and lifecycle integration;
5. restart, scale and bounded-work proof;
6. adversarial admission and resource proof;
7. live Go and Rust libp2p interoperability.

An inventory item may be `live` only when its activation, owner, resource
release, diagnostics and applicable evidence are explicit. `mapped` in the
donor matrix means that a donor case has a Forge test; it does not mean that the
feature is activated or production-ready.

Production readiness is profile-specific. Stage 8 may promote the native
TCP/QUIC profile and the private-network profile only after all of their
required manifest entries pass. Browser transports do not silently gate or
inherit that claim: WebSocket is Stage 9, while WebTransport and WebRTC remain
explicit future-profile decisions.

`support_requirement` says whether a selected profile must support a
capability. `default_activation` independently says whether a deployment gets
it by default, requires an explicit opt-in, or cannot activate it. `interop_applicability`
states whether a Go/Rust run is required, Go-only with an explicit Rust
limitation, Rust-only with an explicit Go limitation, or not applicable.
`decision` is the roadmap disposition. Thus an extension
such as Partial Messages can be required Stage 6 support while still being
opt-in at runtime; no field is inferred from another.

## 4. Stage 1: Support Claims And Inventory

### Scope

- audit and enumerate the protocol, transport, discovery, security, resource
  and host capabilities exposed by the pinned libp2p specs, Go and Rust donors;
- assign every donor capability to a production profile and an explicit
  delivery or rejection disposition, including capabilities not implemented by
  Forge yet;
- enumerate every built-in and transport-upgrade protocol ID, capability bit
  and public nested operational component;
- classify every public P2P declaration through its exact owning module or
  macro-only header and that surface's feature mapping; a source digest covers
  the complete type, method and option text, so any declaration change requires
  an explicit inventory update and reclassification;
- verify that each public module root is registered to its canonical CMake
  target, component and module prefix;
- classify protocol, topology, persistence, resource and plugin surfaces with
  the accepted state vocabulary;
- record normal activation, owner, resource lifetime, persistence,
  maintenance, diagnostics, donor evidence and intended disposition;
- separate wire/component evidence from production support;
- correct current READMEs and donor metadata where wording overstates the
  normal application path.

### Exit gate

- source-only inventory check passes;
- every donor capability belongs to a named profile, references pinned donor
  sources and has explicit support requirement, activation, interop and
  roadmap-decision semantics;
- every profile scope lock exactly matches its classified capabilities, so
  removing or adding an entry requires an explicit baseline update;
- the donor capability manifest, donor fixture matrix and implementation
  inventory reference each other, and every existing Forge feature is owned by
  exactly one donor-capability entry;
- every built-in/negotiated protocol, capability and tracked public nested
  component is covered exactly once;
- every public P2P module belongs to exactly one canonical target and its
  complete declaration snapshot matches the inventory;
- no unknown state, missing owner or orphan with a claimed owner exists;
- every `live` claim has raw-node, official-plugin and donor evidence;
- live scenarios use exact runner profile/scenario selectors, local donor
  documents resolve, and external donor files match pinned donor revisions
  whenever the donor tree is supplied;
- no README promotes a manual, partial, stub or orphan surface as a complete
  production feature.

## 5. Stage 2: Peer-State Foundation

> **Implementation status:** the Stage 2 foundation is implemented by
> `forge-p2p-peer-state-v1`. Async persistence, bounded operational state,
> k-bucket admission, the private ObjectDB adapter and secret-backed official
> plugin startup are covered by focused memory, P2P and DB Store suites. This
> status does not promote Kademlia or the node to `live`: autonomous refresh,
> complete Identify ownership and standard value operations remain Stage 3/4.

### 5.1 Async persistence boundary

Replace the synchronous peer-store backend with an asynchronous persistence
port owned by `forge_net_p2p`. The port supports paged hydration, atomic
batches, bounded expiry pruning, flush and deterministic close. It exposes P2P
domain records, not a database driver API.

Expiry pruning reports exact removed identities rather than category counts,
and durable mutation outcomes distinguish commit failure from a committed
mutation whose sync acknowledgement is uncertain. This keeps operational state
aligned with the database while surfacing the uncertainty as a typed failure.
That diagnostic remains degraded until a later durable apply or explicit flush
confirms durability; ordinary pruning cannot clear it. Provider and Rendezvous
records use the same per-peer endpoint and total variable-byte bounds as the
peer directory, both for live writes and hydration.

Remove the private RocksDB codec and direct `forge_rocksdb` dependency. Keep a
deterministic in-memory implementation for unit and programmatic test paths.

### 5.2 Bounded operational state

The in-memory peer directory provides point lookup and bounded indexed
candidate selection. High-frequency observations use one node-owned bounded,
coalescing persistence queue. Writes acknowledged to a remote peer, including
accepted provider and Rendezvous registrations, complete durably before the
response.

Queue saturation or persistence failure produces backpressure and degraded
diagnostics. No detached task may outlive the node.

### 5.3 Kademlia routing state

Replace the flat routing table with node-owned k-buckets:

- configured `k` and deterministic XOR-distance ordering;
- bounded replacement lists;
- Identify and hydrated records remain candidates until a successful validated
  DHT exchange admits the peer as a server;
- failure-driven replacement and eviction;
- bounded closest-peer and diagnostic snapshots;
- startup candidates revalidated before admission.

Iterative query seeds and `FIND_NODE` responses use this routing state, never a
full durable-store scan.

### 5.4 ObjectDB adapter

`plugins.p2p.node` owns private ObjectDB adapter components over a dedicated
named `plugins.db.store` Object layer. The Stage 2 schema v1 stores peer and
endpoint facts, Identify data, Rendezvous registrations, relay metadata and
monotonic sequence state. Indexes cover hydration priority, expiry,
namespace/sequence and uniqueness. Stage 4 replaces provider rows with a
separate per-profile DHT record-store schema while retaining the same physical
ObjectDB store.

Read-modify-write and sequence changes share one ObjectDB transaction. Schema
mismatch fails startup with a typed error. The v1 recovery path is explicit
cache removal followed by bootstrap hydration; no implicit migration or full
startup scan is allowed.

### 5.5 Official plugin foundation

The plugin depends on DB Store and Crypto Secrets. During `after_initialize()`
it registers private Object models. During `startup()` it loads identity
secrets, opens and hydrates persistence, constructs the node, and only then
opens listeners. Shutdown reverses that ownership.

Configuration uses `peer-store.store`, `identity.certificate-secret` and
`identity.private-key-secret`. Inline PEM is removed from plugin configuration;
programmatic low-level identity construction remains supported. Insecure memory
mode is an explicit test path only.

### Exit gate

- memory and ObjectDB persistence fixtures have parity;
- MDBX and RocksDB reopen, commit and rollback paths pass;
- hydration, pruning, queueing and diagnostics remain bounded with durable
  history much larger than live routing capacity;
- no listener opens before secrets, schema and hydration succeed;
- closest-peer operations do not scan ObjectDB;
- the old RocksDB backend API and handwritten codec are absent.

Current focused evidence includes
`p2p_peer_store_memory_persistence_hydrates_bounded_pages`,
`p2p_dht_k_bucket_bounds_active_and_replacement_capacity`,
`p2p_dht_iterative_lookup_walks_many_peer_topology` and
`p2p_node_plugin_production_lifecycle_reopens_persisted_peer_state`.

## 6. Stage 3: Node Lifecycle, Identify And Resource Ownership

> **Implementation status:** implemented by `forge-p2p-node-lifecycle-v1`.
> The node now owns bounded bootstrap and maintenance, automatic per-session
> Identify and coalesced full-snapshot Identify Push, move-only network
> reservations, transport-backed queued-byte accounting and tracked shutdown.
> This status does not promote Kademlia, topology or the complete host to
> production readiness; their Stage 4/5 gates remain unchanged.

- move bootstrap startup and maintenance from the plugin into the node;
- apply one bounded startup budget, bounded parallelism, jittered retry and
  deterministic cancellation;
- run Identify after every authenticated session and persist only verified
  remote protocol/address facts;
- emit Identify Push after local protocol or address changes;
- replace manual stream/dial counters with move-only reservations on every
  inbound and outbound path;
- connect queued-byte accounting to real buffers or remove the claim;
- make maintenance use direct bounded state rather than diagnostics snapshots;
- expose effective limits, modes and degradation reasons without using
  diagnostics as control state.

### Exit gate

- raw and official-plugin sessions learn the same verified remote facts;
- all dial/stream/queue resources release on success, error, cancellation and
  shutdown;
- bootstrap and Identify continue to work for programmatic node users without
  the official plugin;
- adversarial limits and reverse shutdown are proven.

Focused evidence includes
`p2p_node_strict_bootstrap_retries_until_shared_startup_budget`,
`p2p_node_connect_waits_for_identify_and_push_replaces_protocol_snapshot`,
`p2p_node_identify_failure_keeps_authenticated_session_usable`,
`p2p_node_queued_byte_budget_is_held_until_quic_ack` and
`yamux_retains_chunk_lifetime_while_flow_control_is_blocked`.

## 7. Stage 4: Complete Kademlia And Durable Record Lifetimes

> **Implementation status:** present on `forge-p2p-kademlia-v1`; the exit gate
> remains unverified until the focused persistence, adversarial, package and
> three-process Go/Rust interoperability suites pass on the exact reviewed head.
> This status does not yet promote Kademlia or the P2P host to production.

### Standard profile

When `/ipfs/kad/1.0.0` is enabled, Forge implements:

- peer lookup and routing-table refresh;
- provider add/get, bounded durable storage, expiry and republish lifetime;
- value put/get with application-selected validators and selectors, conflict
  handling, expiry and bounded persistence.

The iterative shortlist starts with the local `k` closest peers; `alpha` only
bounds concurrent RPCs. Provider quorum determines operation acceptance, not
fanout width, so reaching quorum does not cancel publication to the remaining
closest peers. Third-party provider results are never promoted to durable local
ownership without an authenticated `ADD_PROVIDER` from that provider.

`PUT_VALUE` performs bounds checking, validation, deterministic selection and a
durable commit before returning the donor-compatible echo. A node without the
complete Amino validator/store contract rejects the profile during
configuration and does not advertise `/ipfs/kad/1.0.0`.

### Provider registration lifetime

Providing is represented by an owned registration that supports renewal,
withdrawal and shutdown. The node owns TTL republish while the caller owns the
decision that content remains available.

Peer-directory persistence and DHT record persistence are distinct async
ports. The official plugin maps both to one physical ObjectDB store, using the
private cache schema v2 for isolated profile values/providers. A v1 cache is
recoverable state and is explicitly reset and rehydrated; it is not migrated.

### Provider-only deployments

A product may define a peer/provider-only DHT, but it must use a distinct
product protocol ID and explicit compatibility documentation. It cannot claim
interoperability with the standard Kademlia profile.

### Exit gate

- peer, provider and value paths pass codec, raw-node, restart, adversarial and
  live donor tests;
- provider and value records have bounded storage and maintenance;
- no nominal-success stub remains;
- routing refresh and queries remain independent of durable-history size.

The current `dht_find_peer` fixture connects directly to the searched peer and
can be satisfied from the local peer store. It is not live outbound DHT evidence
until a third-peer route or explicit Kademlia negotiation is proven.
The `/pk` and `/ipns` fixture uses writer-only PUT, listener persistence
confirmation and reader-only GET from a distinct fresh process whose local
store is reset before each attempt. The focused exact-head matrix emits all
three roles in each evidence artifact.

## 8. Stage 5+: Production Host Completion

### Stage 5: Topology and discovery

Integrate bounded DHT, Rendezvous and Peer Exchange refresh into one node-owned
topology manager with low/target/high session watermarks. Static topology is an
explicit supported mode, not an accidental default.

> **Implementation status:** present on `forge-p2p-topology-v1`. The manager
> owns coalesced periodic refresh, bounded per-source observations, scored
> dialing and pruning, while the plugin only maps configuration. This status is
> not a production promotion until the exact-head hidden-peer, donor interop,
> shutdown and bounded-state suites pass and the inventory evidence is updated.

Rendezvous client state is scoped by configured `(point, namespace)` pairs:
cookies are opaque, registration renews from the server-returned TTL and local
signed-address generation, and shutdown performs best-effort unregister.
Forge Peer Exchange is a product extension rather than a libp2p standard; its
hints remain untrusted until authenticated connect and Identify. Stage 4 owns
Kademlia bucket refresh, so Stage 5 consumes its results without duplicating
that maintenance loop.

### Stage 6: Donor parity, reachability and path management

Stage 6 closes every required native/private-host gap discovered by the
donor-first manifest except WebSocket. It is delivered as focused PRs rather
than one transport monolith. The canonical order, dependencies and permitted
capability owners are the exact machine-readable
`stage_6_pr_registry` in `p2p_donor_capabilities.json`; it has no aliases or
locally reordered variants. The roadmap, chrono and crypto prerequisite PRs
own no capability entry.

0. `forge-p2p-stage6-roadmap-v1`: freeze the four-field manifest, donor evidence,
   profile locks, exact PR ownership and acceptance matrix; no runtime code;
1. `forge-chrono-v1`: add only reusable `forge_chrono` formatting and parsing
   for explicit `std::chrono` timestamps. It owns no clock, scheduler, thread,
   deadline/backoff policy or P2P lifecycle;
2. `forge-p2p-host-protection-v1`: staged connection gater plus memory, file
   descriptor, transient and service resource scopes;
3. `forge-crypto-xsalsa20-v1`: add the crypto boundary required by private
   transport protection, with pinned `libsodium` discovery and tests;
4. `forge-p2p-private-network-v1`: TCP/Yamux plus a transport PSK layer before
   the normal secure channel, with no `/pnet` protocol ID, QUIC, Relay or DCUtR;
   AutoNAT and UPnP require one explicit Internet-egress policy;
5. `forge-p2p-address-resolution-v1`: `/dnsaddr`, bounded recursive
   resolution, Happy Eyeballs, IPv6 black-hole suppression for native/private
   profiles and UDP black-hole suppression for the native profile over the
   `net_dns` boundary; this PR requires pinned `c-ares` discovery and tests;
6. `forge-p2p-reachability-v1`: observed-address confidence and expiry,
   host-local node-level AutoNAT v1 lifecycle and address-level AutoNAT v2
   evidence, opt-in bounded wire service roles, effective reachability,
   periodic Ping liveness distinct from the current Ping wire protocol and typed
   host-state events;
7. `forge-p2p-mdns-v1`: optional public mDNS with Go/Rust interop, plus the
   fingerprinted private-network service namespace with its documented Rust
   limitation;
8. `forge-p2p-nat-mapping-v1`: optional native UPnP mapping ownership, renewal, loss
   and confirmed external-address publication; private profiles use it only
   through the explicit Internet-egress policy;
9. `forge-p2p-autorelay-v1`: verified relay candidates, bounded reservations,
   renewal/replacement and Identify Push of circuit addresses are host-local
   orchestration; the public Relay v2 service is a separately configured,
   bounded Go/Rust service-client role;
10. `forge-p2p-path-management-v1`: one per-peer DCUtR/coordinated-dial state,
   modern port reuse, direct upgrade, backoff, cancellation, relay fallback and
   Go TLS/Noise inlined muxer negotiation. Forge-Rust keeps the pinned donor
   fallback and does not claim inline support; `/libp2p/simultaneous-connect`
   stays excluded;
11. `forge-p2p-gossipsub-scoring-v1`: donor-consistent scoring, mesh repair and
   v1.0 fallback proof;
12. `forge-p2p-gossipsub-extensions-v1`: v1.2 `IDONTWANT`, v1.3 extension
    support where one first-RPC advertisement is sent, unknown extensions are
    ignored and capability matching decides use, plus opt-in Partial Messages
    with Go/Rust interop evidence.

The existing DHT, topology and transport services are reused. mDNS, DNSAddr,
AutoNAT, AutoRelay and path upgrades feed the same topology manager; they do
not create parallel discovery or dialing loops. Typed events report state
changes without turning diagnostics into a control API.

Private-network and address-resolution PRs both depend on host protection
because they introduce bounded resolver, dial and mapping work. Path management
depends on address resolution because coordinated attempts consume normalized
candidate addresses. Reachability deliberately has no direct address-resolution
dependency: Ping, AutoNAT and observed-address evidence consume configured or
Identify-observed endpoints and do not resolve `/dnsaddr`.

The runtime PRs use `create-library` ownership. They may establish raw-node
typed options, but `plugins.p2p.node` schema and configuration mapping wait for
Stage 7, after those contracts are stable. `c-ares` and `libsodium` are explicit
build/test prerequisites of their named PRs, not incidental transitive
dependencies.

Stage 6 is complete only when the manifest has no `stage_6` entry,
each delivered capability has an implementation feature/evidence mapping, and
live Go/Rust tests cover every standard protocol or negotiation behavior that
the profile advertises. Registered donor fixtures are test declarations, not
passing evidence. AutoNAT receipts attribute each pair to its actual Forge role:
client requirements cover Forge-to-Go/Rust, service requirements cover
Go/Rust-to-Forge. This corrects the generic four-direction role template without
reducing bilateral coverage. The separate `test_forge_p2p_autonat_acceptance`
target requires all 41 pairs and controls, including TLS, private ALLOW/DENY and
the genuine Go v1 negative response. It does not replace the general Stage 6
acceptance target or count one receipt as both client and service evidence.
The general target runs these 41 cases once alongside the other Stage 6 cases,
with the same prepared binaries, provenance and promotion receipt; omitting
them fails validation even if all older cases pass.
The source checker validates only registry structure and
current runner registrations; it never emits an execution PASS. A standalone
artifact check can report only local `CONSISTENT`/`NOT_RUN`/`FAILED` status, not
promotion. The CMake promotion target owns the canonical runner invocation and,
in that same process, writes a local execution receipt with the subprocess exit
code and immediately validates the exact artifact it produced. It uses the
current resolved `sys.executable`, the exact canonical runner flags and roots,
enabled mode, the reviewed `HEAD`, a clean tracked tree, fixture `build-info`,
Python/binary path and SHA-256 provenance, and the manifest path/SHA-256.

The artifact records raw dialer/listener commands, their binary bindings,
launcher execution inputs/results, result payloads, and a relative SHA-256
evidence index. `effective_configuration` is only a checked projection of those
commands and parsed endpoint results; it does not repeat manifest capability
labels as authority. The checker parses each indexed JSON result and requires it
to equal the corresponding raw payload (except runner-owned `result_file` and
`attempts`), rejects failure text, non-JSON evidence, `/bin/false`, incomplete or
provenance-only argv, and reused proof files.

A future PR4 private TCP/Yamux+pnet fixture must pass the same absolute
`--pnet-key-file` outside the runner artifact directory to both dialer and
listener and use `--transport tcp-pnet`. The key bytes are neither serialized,
logged nor added to the evidence index.
Both endpoint result files must report `pnet_enabled=true`,
`negotiated_pnet=true`, and one shared non-secret `pnet_fingerprint`. Its exact
value is `SHA-256("forge.net.pnet.operational-fingerprint.v1" || 0x00 || decoded-PSK-bytes)`;
the PR4 runner and its newly registered validator must independently recompute
it from the configured file without serializing, logging or indexing the PSK
bytes. PR0 does not install a PNET validator or synthetic control artifact.
The root PNET scenario additionally needs one same-key positive run and two
separately indexed control executions: missing key and mismatched key. Each
control binds its launcher command, log and result to the expected peer and
must show an outbound dial boundary and actual listener ingress, zero
established connections and zero Identify/application streams with rejection
before Identify. The mismatched-key fixture key also remains outside the
artifact directory and evidence index.
If a future private AutoNAT scenario requires
`reachability.private_internet_policy`, both commands must additionally pass
`--private-egress-policy allow-internet`; both results must report that policy
and `egress_policy_enforced=true`, while the dialer reports a successful actual
dialback and the listener reports its receipt. A separate successful
`--private-egress-policy deny-external` control must emit a hashed result with
`status=rejected`, `external_dial_attempted=false` and
`rejection_reason=private_egress_policy`; it never serializes PSK bytes. The
implementing PR maps manifest dependencies to these concrete command/result
proofs rather than trusting enabled-capability labels. Native QUIC, native
TCP/Yamux and private TCP/Yamux+pnet remain separate proofs. An optional
capability is tested only through an enabled run and is never thereby claimed
as default.

The SHA-256 index is tamper-evident, self-consistent local evidence only. It is
not a signed remote attestation and does not claim malicious-artifact
unforgeability; the promotion authority is the CMake-owned execution and its
immediate validation, not external artifact replay.
Every donor acceptance scenario, including a planned scenario, has one closed,
exact contract name:
`forge.p2p.evidence.<scenario-id>.v1`. The manifest's `evidence_contracts`
registry must cover those values exactly, and the paired inventory and
acceptance checkers reject missing, unknown or reused names. Only a
`registration: registered` contract maps to an executable semantic validator;
a planned contract deliberately has no validator and blocks promotion until its
implementing PR supplies the real runner, observed runtime schema, negative
controls and donor evidence. `status=ok`, enabled-capability labels or a
scenario label alone cannot promote a result. Current runner-backed cases
validate emitted Ping, Identify, TCP/Yamux endpoint state and echo, DHT, Rendezvous and
implementation-specific Relay reservation/open fields. Merely enabling a
listener feature is not execution evidence. The future PR owns the exact
runtime schema and negative controls for its contract: correlated security/muxer
transcripts; AutoNAT probe/dialback allow and deny evidence; relay/DCUtR paths;
discovery no-dial controls; and correlated DHT, Rendezvous, GossipSub,
port-reuse and fragment evidence. Until that PR registers an observed runner
schema and separately indexed command/result/log evidence, its declared
contract ID is promotion-blocking; source labels are not behavior proof.
The native Go TLS and Rust fixed-ALPN TLS fallback inline-muxer contracts are
also exact TLS contracts, so their future launcher mapping is `tcp-tls`; the
private-profile variants remain `tcp-pnet` and need their own PR4 proof.
For current native scenarios, the command mapping is only a launcher check.
`quic_v1_transport` additionally requires the endpoint result to report the
observed `/quic-v1` transport, the authenticated remote peer ID and the Identify
proof. `tcp_yamux` requires endpoint-emitted `negotiated_transport=tcp`,
`negotiated_security=/noise`, `negotiated_muxer=/yamux/1.0.0`, the authenticated
remote peer ID and a completed echo. `multistream_select`, `noise_identity` and `tls_identity` require an
endpoint-emitted ordered multistream/security/Yamux/application transcript and
the exact negotiated security and muxer fields in that result; runner-owned
record fields cannot satisfy either requirement. Go can observe its connection
security/muxer state and QUIC peer/transport state, while the current Rust Swarm
public event surface exposes the authenticated peer and remote multiaddr but not
the selected Noise/TLS/Yamux upgrade phases. Those missing endpoint fields are a
specific promotion blocker, not a license to emit values derived from fixture
CLI options. The current Forge TCP fixture has the same transcript requirement
before a current TCP security case can be promoted.
`kademlia_amino` likewise requires the exact `/ipfs/kad/1.0.0` result plus
provider, independent querier and returned-provider equality, a provider address
count, and positive protocol-stream and query deltas. A `provider_count` alone
is only registration-path data and is rejected.
`limited` is valid only for a registry-classified limitation and
emits `PASS_WITH_DOCUMENTED_LIMITATIONS`; all other directions use the schema's
`passing_status`. Missing, stale or incomplete artifacts are `NOT_RUN`, never a
passing result. The inventory checker remains the paired source-manifest gate.
`test_forge_p2p_inventory` runs only source validation and the checker self-test;
PR0 does not require a live artifact. Promotion is explicit: it runs the runner
with live execution enabled and preserves a nonzero runner exit as `FAILED`.
Every checker error after that invocation is also `FAILED`, never `NOT_RUN`.
There is no cache variable for replaying an externally supplied artifact:

```bash
cmake -S . -B build/forge-p2p-stage6 -G Ninja
cmake --build build/forge-p2p-stage6 --target test_forge_p2p_stage6_acceptance
```

For a multi-config generator, CMake scopes the promotion directory by
`$<CONFIG>` so independently built fixture/provenance outputs cannot collide.
Within that configured base, the wrapper creates one fresh random invocation
directory for every promotion, binds its receipt to that directory and prints the
preserved evidence path; it never reuses a stale shared artifact path.

### Stage 7: Official plugin and operational surface

Expose the complete validated Stage 6 configuration, host/protocol metrics and read-only diagnostics
through the official plugins. Plugins remain dependency/configuration adapters:
they may not own mDNS, NAT, relay, dialing, GossipSub or resource-maintenance
loops. Programmatic nodes and plugin-created nodes must have lifecycle parity.

### Stage 8: Production proof

Run restart, churn, scale, hostile-peer, bounded-memory and long-duration tests
through both the raw node and official plugins, followed by live Go and Rust
interop. Only then may native and private-network inventory entries be promoted
to `live` and Content Swarm resume on the hardened substrate. This promotion
does not claim browser transport support.

### Stage 9: P2P WebSocket and browser transport profile

Implement donor-compatible `/ws` and `/wss` dial/listen behavior, secure upgrade,
resource accounting, proxy/backpressure handling, explicit WSS certificate
ownership/AutoTLS policy and Go/Rust interoperability.
Only then may Forge advertise the WebSocket browser-transport profile.
WebTransport and WebRTC remain separately classified future capabilities and
cannot be inferred from WebSocket support.

## 9. Delivery Discipline

- Each stage is a focused PR against current `dev`.
- Runtime PRs follow `create-library` and `create-plugin` exact ownership.
- One writable worktree and one build tree are used for this program.
- Only the coordinator builds, with `-j4`; reviewers remain read-only.
- Full CI is reserved for release gates. Focused suites prove each stage.
- Changes to wire compatibility, public support claims or persisted schemas are
  explicit in the corresponding PR and migration note.
