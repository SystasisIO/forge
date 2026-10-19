# Forge Swarm v1

> **Status:** accepted implementation direction, not shipped API. Development
> may resume alongside P2P Stage 7 after Stage 6 completion; production remains
> gated by P2P Stage 8 and Swarm's own acceptance tests.
>
> **Historical prerequisite branch:** `forge-api-live-streaming-v1`.
> Swarm implementation branches are chosen by the focused delivery plans.
>
> This document defines the cross-library boundary and implementation order for
> a neutral BitTorrent-like immutable-content distribution mechanism. Detailed
> public signatures are finalized in the focused PR that owns each component.
> **Plan revision, 2026-10-16:** reusable runtime belongs to `forge::swarm`;
> the optional plugin is a thin application adapter. Paid and free serving use
> product-supplied admission and quota policies over the same engine.
> The same revision makes Swarm a library family: Object Store metadata belongs
> to `swarm.db`, network records to `swarm.protocol`, and remote contracts to
> `swarm.api`. The family root stays empty; focused PRs deliver its leaves.
> The common conversion template belongs to existing `forge_core` through
> `forge.core.conversion`; domain specializations belong to `swarm.conversion`.
> Development, joint validation and production follow the separate
> [Swarm entry gates](forge-p2p-production-hardening-v1.md#10-content-swarm-entry-gate).
> The document filename is retained for existing links; `content` is not a
> planned library family or namespace.

## 1. Goal

Forge needs a reusable swarm mechanism in which a node that has verified part
or all of immutable content can immediately serve it to other nodes. The same
mechanism must support product snapshots, model artifacts and future
user-facing file distribution without moving product trust, billing, retention
or disk policy into Forge.

The result of a file download must be an ordinary file or directory tree. A DB
backend may store catalog, resume or metrics state, but it must not be the only
representation of user payload bytes.

## 2. Accepted Boundaries

The implementation is split into these layers:

1. `forge_swarm_core` owns canonical descriptor/identity values, Merkle, piece,
   picker and deterministic transfer/resume mechanics, plus the storage and
   metadata-persistence contracts consumed by the engine. It owns no DB schema.
2. `forge_swarm_protocol` owns transport-neutral network request/response and
   peer-message DTOs. It reuses canonical Core values, not ObjectDB records.
3. `forge_swarm_api` owns the peer `FORGE_API` interfaces over Protocol DTOs;
   it does not declare duplicate wire records.
4. `forge_swarm_db` owns indexed local metadata models and Object Store index
   descriptors. It owns no DB handle, runtime lifecycle, network operation or
   physical key/index implementation.
5. `forge_swarm_storage` implements the Core storage contracts: ordinary files,
   sidecar checkpoints and an ObjectDB metadata adapter over the DB models.
   Completed payload remains normal files, not DB objects or blobs. A generic
   content-storage family is not introduced.
6. `forge_swarm_runtime` owns the reusable active engine in
   `forge::swarm::runtime`: peer sessions, transfer orchestration, timers,
   bounded work, admission/quota
   enforcement and joined shutdown. It consumes the mechanisms and peer API
   above, without depending on Forge.App or plugin implementations.
7. `forge_plugins_swarm_node` is an optional thin adapter. It obtains official
   P2P capabilities, maps application configuration, publishes the peer API
   and delegates operations and shutdown to that same library runtime.
8. Host products own descriptor trust, source and destination selection,
   authorization, quota allocation, pricing, settlement, retention, seeding
   duration and origin fallback. Forge enforces the permissions and bounded
   allowances supplied by a product; it does not decide who has paid.

Forge does not ship a standalone swarm daemon. A user-facing daemon is a
product because it must choose paths, conflict behavior, CLI/API, retention and
account/origin policy.

Swarm is a standalone distribution engine, not a transport or a P2P-node
subcomponent. It uses `forge::api` for typed peer exchange and `forge::net::p2p`
through the official plugin for network mechanics. It therefore lives in
`forge::swarm`, not `forge::content::swarm`, `forge::net::swarm` or
`forge::net::p2p::swarm`. No `content` family is created speculatively.

## 3. Planned Project Shape

The library and plugin follow `create-library` and `create-plugin` without aggregate-only
modules, dummy sources or mixed file ownership:

```text
libraries/swarm/
  CMakeLists.txt                         # dispatcher only
  README.md
  core/                                 # values, mechanics, storage contracts
  protocol/                             # network DTOs
  db/                                   # metadata models and index descriptors
  conversion/                           # domain conversion specializations
  storage/                              # files and metadata persistence adapters
  api/                                  # FORGE_API interfaces
  runtime/                              # active engine and policy enforcement

plugins/swarm/node/
  CMakeLists.txt
  README.md
  include/forge/plugins/swarm/node/
  details/
```

Planned targets and package components:

| Target | Component | Namespace |
| --- | --- | --- |
| `forge_swarm_core` | `swarm_core` | `forge::swarm::core` |
| `forge_swarm_protocol` | `swarm_protocol` | `forge::swarm::protocol` |
| `forge_swarm_db` | `swarm_db` | `forge::swarm::db` |
| `forge_swarm_conversion` | `swarm_conversion` | `forge::swarm::conversion`; specializations in the primary template's namespace |
| `forge_swarm_storage` | `swarm_storage` | `forge::swarm::storage` |
| `forge_swarm_api` | `swarm_api` | `forge::swarm::api` |
| `forge_swarm_runtime` | `swarm_runtime` | `forge::swarm::runtime` |
| `forge_plugins_swarm_node` | `plugins_swarm_node` | `forge::plugins::swarm::node` |

Each leaf has its own explicit CMake source list and README. Public modules
live directly in `libraries/swarm/<leaf>/include/forge/swarm/<leaf>/` and use
`forge.swarm.<leaf>.*`; for example `forge.swarm.storage.file_store`. The
`forge::swarm` family root has no declarations, aggregate module or compiled
`forge_swarm` target. This supersedes the earlier flat layout before it ships;
there are no aliases for unimplemented names. The plugin module prefix and
contract id are both
`forge.plugins.swarm.node`. Its role is application ownership of an instance
of the library runtime, not implementation of that runtime or ownership of a
second network node. The `forge::plugins::swarm` grouping
namespace contains no public declarations.

Each source/module has one target owner. `swarm.api` is a domain contract leaf,
not a Forge.API transport binding. Non-template implementations use exact
public/private pairs, and aspect units split only coherent implementations.
The runtime remains in the Swarm library family, never in the plugin; adding
the DB/Protocol boundaries does not reverse that decision.

Core and protocol-only consumers must not acquire ObjectDB, file-storage,
API-session or plugin dependencies. The target graph is acyclic:

```text
forge_swarm_protocol -> forge_swarm_core + Forge Raw/reflection
forge_swarm_db -> forge_swarm_core + forge_db_object
forge_swarm_conversion -> forge_core + forge_swarm_db + forge_swarm_protocol
forge_swarm_storage -> forge_swarm_core + forge_swarm_db + Forge Asio
forge_swarm_api -> forge_swarm_protocol + forge_api_core
forge_swarm_runtime -> forge_swarm_core + forge_swarm_api + Forge Asio
forge_plugins_swarm_node -> forge_swarm_runtime + forge_swarm_storage
                         + Forge.App + official P2P/DB Store APIs
```

The runtime receives caller-owned execution, abstract storage/metadata services,
product policy and connection/discovery capabilities. Concrete official-plugin adapters remain
in the plugin; the library must not import plugin namespaces. Reuse existing
typed API handles, stream primitives, scheduler/compute executors and P2P
registration ownership rather than inventing a transport or task framework.
There is no runtime-only-in-plugin implementation and no second product copy.

### 3.1 DB, protocol and conversion ownership

DB models derive from `forge::db::object::object<...>` and describe their indexes
with the existing Object Store DSL. They are local persisted records, not wire
DTOs. Protocol records may compose the same canonical Core values, but must not
inherit an ObjectDB model, import `swarm.db`, expose backend keys or serialize
local object IDs, disk paths, policy handles or private accounting state.

Use composition for shared descriptor/identity values; inheritance is justified
only for a real value extension with explicit reflected field order. Do not
copy fields just to maintain parallel DB and network versions of one canonical
value. All reflection, schema and API metadata follow declarations at the bottom
of their owning module, as required by the library skill.

Conversion is explicit and is not necessarily reversible:

- DB-to-protocol assembly receives already-read records, selects only public
  fields and checks identity/relationship/completeness invariants. It performs
  no hidden reads or commits and is not a generic reflection mapper.
- Domain builders/converters specialize the common template imported from
  `forge.core.conversion`. Their source and package owner is
  `forge_swarm_conversion`, not Core, DB, Protocol or a plugin. The input layers
  do not depend on their conversion layer; only callers assembling a projection
  import it. Do not create a second primary template for Swarm.
- Protocol-to-DB conversion can prepare validated domain values, but network
  data cannot assert local verification, completion, permission, quota or object
  identity. The operation owner checks authority, obtains local IDs and performs
  an explicit transaction. A remote `HAVE` is a peer claim, never local verified
  availability.
- Do not require byte-for-byte DB/DTO round trips for projections that omit
  private fields. Test stable wire bytes, correct projection and reconstruction
  only for the explicitly reversible subset; DB schema and protocol versions
  evolve independently.

### 3.2 Shared conversion template in Core

The common customization point is planned in the existing `forge_core` target
and `core` package component, not in a new generic conversion library:

```cpp
export module forge.core.conversion;

export namespace forge::core {
template <class Result> class conversion;
}
```

Its source is `libraries/core/include/forge/core/conversion.cppm`. This is a
primary template declaration, not a virtual base class, runtime registry or
reflection-driven converter. Core has no default implementation that guesses a
mapping, and no dependency on Swarm, DB, Raw, Variant, reflection or API. A
declaration-only template module needs no dummy paired `.cpp` or separate
compiled target.

The consumer's `libraries/swarm/conversion/` leaf owns actual specializations
and their validation/assembly logic, importing `forge.core.conversion` and the
needed DB/Protocol records. Planned module `forge.swarm.conversion.conversion`
lives in `include/forge/swarm/conversion/conversion.cppm`, with nontrivial
specialization methods implemented out-of-line in its paired `conversion.cpp`.
Use coherent aspect units only if real implementation ownership warrants them.

C++ specialization declarations belong to the primary template's namespace
`forge::core`; their files, implementation and target remain domain-owned.
Domain helpers/errors, if needed, live in `forge::swarm::conversion`. This is
the specialization exception, not permission to put domain code into Core or
declare a second `forge::swarm::conversion::conversion` template.

Constructors accept the already-obtained inputs needed by each result; builder
specializations may expose bounded composition and `finish() &&`. Do not force
every mapping into an inheritance hierarchy, add forwarding aliases, infer an
inverse automatically or move existing Variant/Raw conversion into Core. The
reference pattern is reused, but product migrations are outside this plan.

## 4. Forge API Prerequisite

The original prerequisite addressed a batch-shaped Forge API streaming surface:

- server streaming materializes `std::vector<Response>`;
- client streaming accepts `std::vector<Request>`;
- bidirectional streaming accepts and returns vectors;
- the generic transport client buffers all response frames until the terminal
  frame.

That surface is not suitable for unbounded transfer, flow control or simultaneous
peer messages. Swarm consumes the live API contract described below; it must not
reintroduce vector-based streaming or build a second streaming runtime.

### 4.1 Public stream primitives

The API Core family gains typed live streams equivalent in role to:

```cpp
template <typename T>
class stream_reader;

template <typename T>
class stream_writer;

template <typename Incoming, typename Outgoing>
class duplex_stream;
```

Required semantics:

- incremental item delivery without whole-call materialization;
- bounded queues and backpressure, meaning a fast sender waits for receiver
  capacity rather than growing memory without limit;
- simultaneous reads and writes for bidirectional methods;
- independent half-close of each direction;
- typed terminal error, deadline and cancellation propagation;
- one terminal transition and deterministic cleanup of pending operations;
- continuation on the caller executor;
- configurable per-item and per-stream limits;
- `FORGE_API` descriptor generation and remote proxies for all stream kinds.

Forge API wire v2 keeps the existing frame envelope and adds protocol-owned
`session_hello` and `stream_window` control frames. `session_hello` performs a
mandatory capability and limit handshake before the first request.
`stream_window` carries receiver-driven per-call item and byte credit so one
slow call cannot stall or exhaust a multiplexed API connection. Swarm does not
introduce a second raw framing stack.

Windows contain monotonic absolute `{max_items, max_bytes}` limits, not additive
credits. A repeated or smaller limit grants nothing. One `stream_item` consumes
one item and its encoded payload bytes; capacity is returned only after the
application reads or discards that item. The session also enforces a negotiated
aggregate buffered-byte limit across calls.

Wire v2 is a clean replacement for the batch-shaped `/forge/api/1` protocol.
P2P defaults to `/forge/api/2`; custom application protocol ids remain
application-owned but use the same mandatory v2 session handshake. Existing
frame-kind numeric values are preserved, while the new control kinds are
appended.

### 4.2 Transport capability rules

The API contract remains transport-neutral, but a binding must advertise and
enforce the stream kinds it can implement:

| Binding | Required support |
| --- | --- |
| P2P/QUIC stream | unary, server, client and bidirectional streaming |
| WebSocket | unary and all streaming forms |
| HTTP/1.1 | unary, server streaming and client streaming |

An HTTP/1.1 binding maps server streams to chunked responses and client streams
to streamed request bodies. It must reject bidirectional methods at binding and
again before client I/O, and must never emulate them by collecting an unbounded
vector. Full peer exchange over web transport uses WebSocket unless a later
HTTP/2 binding provides equivalent duplex semantics.

## 5. P2P Discovery Prerequisite

The official `plugins.net.p2p.node.dht_api` already exposes profile-aware DHT
operations, where DHT means a distributed hash table, including:

```cpp
auto availability = co_await dht.provide(profile, key, options);
auto providers = co_await dht.find_providers(profile, key, options);
```

Reuse this slice with its existing bounded query options. Do not add a second
Swarm-owned DHT client or provider-renewal loop. The Swarm adapter must prove
cancellation, deadlines and registration lifetime through that public surface.

The historical source audit also found broader official-plugin integration gaps around
secure persistent startup, Identify, Peer Exchange, DHT/Rendezvous lifecycle,
topology maintenance, AutoNAT and relay candidate discovery. They are tracked as
the prerequisite production backlog in
[`forge-p2p-production-hardening-v1.md`](forge-p2p-production-hardening-v1.md).
Swarm must not treat the presence of low-level protocol tests as proof that this
host lifecycle is delivered.

The resolver plugin keeps its existing responsibility: it resolves and opens a
typed API on an already known peer. It does not become content discovery.

The normal connection flow is:

```text
DHT find providers
  -> peer id
  -> node API remote<swarm peer API>(peer, known protocol)
     or optional resolver for API catalog lookup
  -> Forge API bidirectional stream
```

The provider key is domain-separated from other DHT records and derived from
the swarm realm plus swarm identity. A realm separates discovery keys; it is
not authorization. A runtime-owned sharing operation retains the availability
registration supplied by the P2P adapter for exactly as long as verified
content is available and product policy permits advertising it. The adapter
uses the existing move-only `provider_registration`, without a second renewal
mechanism. The P2P node owns initial publication, bounded jittered republish
and withdrawal on registration release or shutdown. Remote provider records
expire naturally; restart requires the product to confirm availability and
create a fresh registration.

No public raw `open_protocol_stream()` escape hatch is added for Swarm.

## 6. Content Descriptor And Identity

The canonical descriptor in `swarm.core` describes an immutable single-file or
multi-file distribution, not one anonymous byte array. Protocol DTOs reuse this
value rather than defining a second descriptor layout:

```cpp
struct file_entry {
   std::vector<std::string> path;
   std::uint64_t size;
   forge::crypto::digest::sha256 root;
   std::vector<forge::crypto::digest::sha256> pieces;
};

struct descriptor {
   descriptor_version version;
   std::uint32_t piece_size;
   std::vector<file_entry> files;
};
```

PR1 freezes `descriptor_version::v1 = 1` as an explicitly serialized uint32,
`piece_size` as uint32 and file sizes as uint64. Counts use minimal Raw uint32
varints. Each file owns its ordered piece layer; files no larger than one piece
omit that layer. This replaces the provisional global `piece_hash_layer`
sketch with unambiguous file-local ownership. The
[Core guide](../../libraries/swarm/core/README.md) specifies exact bytes and
limits, and [donor evidence](../donors/forge-swarm-core-v1.md) records acceptance.

Accepted descriptor rules:

- SHA-256 is the v1 digest algorithm;
- the Merkle base block is 16 KiB;
- piece size is a power of two and at least 16 KiB;
- every non-empty file begins at a piece boundary, so one piece never spans two
  files;
- each non-empty file has a Merkle root over its bytes;
- piece layers allow a received piece to be verified before full completion;
- `swarm_id` is derived from canonical descriptor identity bytes;
- local path mappings and user-selected display names do not change the
  descriptor;
- a product binding, signature, price, origin, chain id or retention policy is
  never part of the neutral descriptor.

Path components are validated before filesystem use. Absolute paths, empty
intermediate components, `.`, `..`, separators embedded in a component, NUL,
duplicate normalized paths and file/directory collisions are rejected.
Platform adapters additionally reject names that cannot be represented safely
on their target filesystem.

Canonical logical paths preserve valid UTF-8 bytes without normalization or
case folding and are ordered component-wise by unsigned bytes. Exact duplicate
and file/directory prefix paths are invalid. Case or Unicode equivalence and
other target-filesystem aliases require adapter rejection rather than changing
the descriptor identity.

The v1 descriptor is Forge Raw, not a claim of `.torrent` wire compatibility.
The file/piece layout deliberately follows BitTorrent v2 closely enough for a
future explicit import/export bridge. Text magnet representation is an outer
boundary and may use `forge_multiformats`; it is not required by swarm
mechanics.

## 7. Filesystem Storage

### 7.1 Store contract

`forge_swarm_core` defines the runtime-facing storage contract with these
capabilities:

- return a consistent availability snapshot;
- read a bounded range from a verified piece;
- begin and write a bounded piece range;
- atomically mark a fully verified piece available;
- discard incomplete or corrupted data;
- load and save resume state;
- acquire a lease, meaning a temporary hold that prevents deletion while a
  transfer reads or writes the content;
- report local appearance or removal of pieces.

The contract is filesystem-neutral so Spine snapshot directories and model
caches can provide adapters without copying their payload into a second store.

### 7.2 File-backed Swarm store

`forge::swarm::storage::file_store`, shipped by `forge_swarm_storage`, implements
the contract using ordinary files:

- an existing complete file or directory can be verified and seeded directly;
- a single-file download writes a hidden sibling staging file;
- a multi-file download writes a hidden sibling staging directory;
- staging is on the same filesystem as the final destination;
- payload files are sparse or preallocated where supported and receive data at
  their final logical offsets;
- standalone file mode uses a small atomically replaced sidecar for descriptor
  identity, verified pieces and unfinished block state; ObjectDB-backed mode
  uses the selected metadata checkpoint adapter instead, not a second authority;
- a piece is advertised only after its bytes and Merkle proof are verified;
- completion syncs files, publishes with atomic rename and syncs the parent
  directory;
- multi-file publication renames one staging root, not individual visible
  files;
- an unexpected existing destination produces a typed conflict; overwrite,
  merge or user rename policy belongs to the product;
- cross-filesystem publication is rejected rather than silently weakening the
  atomicity contract.

The file store uses caller-owned bounded `forge::asio::compute::executor`
instances for blocking filesystem and hashing work. CPU and disk execution use
separate pools supplied by application composition, so a slow disk cannot
occupy all hash workers. This reuses Forge Asio rather than introducing a Swarm
thread pool; no pool is created per file, peer or distribution. Section 8.4
defines execution ownership.

RocksDB, MDBX and DB Blob are not payload backends for the file store. The
ObjectDB catalog/checkpoint adapter is a planned supported composition, not a
future excuse to replace payload files with database blobs. Runtime consumers
can still use a standalone file store or a product-supplied adapter.

### 7.3 Indexed metadata in Swarm DB

`swarm.db` describes only the local metadata that needs Object Store indexing:

- content/catalog records keyed by canonical swarm identity, with descriptor
  metadata and a local storage locator;
- download/sharing job metadata needed for restart, with state, priority and
  checkpoint generation;
- bounded resume checkpoints for verified and unfinished pieces;
- optional retention/expiry metadata supplied by the owning product, not a
  library decision about how long to retain content.

Freeze exact records and fields in the DB PR after enumerating real queries.
Index coverage includes identity lookup, bounded state/priority selection and
expiry traversal where that field is present. Use Object Store's primary,
ordered and composite descriptors, pagination and transactions. Do not write
custom physical keys, a second index engine or full-catalog snapshot scans.
Do not persist one object per received network block merely to use an index.

These model modules contain declarations, field metadata and index descriptors,
not live DB handles, registration side effects or runtime loops. The storage
adapter uses externally supplied Object Store services. Named-store lifecycle
belongs to `plugins.db.store`; application/plugin composition registers the
schema before starting the Swarm runtime. No direct RocksDB/MDBX ownership is
introduced into Swarm or its plugin.

The runtime's bounded bitmaps, counters and outstanding-request queues remain
algorithm state, not another authoritative indexed catalog. Product payment
records and budgets do not become Swarm DB models.

### 7.4 Checkpoint authority and crash ordering

Each content operation selects one checkpoint authority: sidecar or ObjectDB.
Do not independently update both verified-state copies. The DB catalog may
refer to a sidecar-managed store, but must then treat availability as a derived
view and consult that store before serving, not as a second resume authority.

Payload filesystem operations and a metadata DB transaction are not one atomic
transaction. Verified durable checkpoints require data verification and the
necessary file sync before committing the checkpoint; final publication also
requires rename and parent-directory sync. A crash between those steps may
leave recoverable bytes without metadata, never justify advertising missing
bytes as complete. Reopen reconciles generation, location and actual files,
rehashing when their integrity cannot be established.

Checkpoint writes may batch a bounded amount of verified progress after file
sync; an `fsync()` per network block is not required. Live verified availability
and the last durable restart checkpoint are distinct states. A lagging
checkpoint may require rechecking data after restart, but must not claim bytes
that were never durably written. A failed sync or checkpoint commit is an
explicit storage failure, not successful durable completion.

Metadata transactions atomically update their related records and indexes.
Schema mismatch, failed commit, stale checkpoints and replaced/missing files
have typed outcomes and explicit recovery tests. Neither a DB row nor a remote
DTO is proof that payload still exists and is verified.

## 8. Swarm Mechanics And Runtime

### 8.1 Deterministic mechanics

A piece is the unit of availability and selection; a block is the bounded unit
of transfer. A piece may contain multiple 16 KiB blocks. Receiving one block
does not establish full-piece availability. A block can be independently
verified only with an authenticated expected hash/proof; otherwise verification
waits for enough data to check the piece against its authenticated layer.

`forge_swarm_core` owns deterministic mechanisms independent of sockets and
application lifecycle:

- peer availability and verified local availability bitfields;
- rarest-first selection;
- sequential and explicit piece priorities;
- bounded request pipelining;
- endgame duplicate requests and cancellation of losing requests;
- choke, unchoke and optimistic unchoke state;
- throughput, latency, timeout and integrity-failure observations;
- peer scoring inputs without product bans or economic meaning;
- transfer/session state machines;
- have, verified and unfinished resume state;
- deterministic scheduling decisions suitable for simulation tests.

Rarest-first, sequential and explicit-priority behavior are deliberate picker
policies, not competing schedulers. Bound the number and bytes of unfinished
pieces and prefer completing partial pieces when that bound is approached,
following libtorrent's picker. Time and randomness are explicit inputs for
reproducible tests. Choke/unchoke controls serving capacity; reciprocal upload
is not mandatory authorization for paid providers or snapshot distribution.

Endgame duplication is bounded and uses the same admission, resource and quota
path as ordinary requests. Commit a verified winner once and cancel redundant
requests. Late responses crossing cancellation are expected, not automatically
peer misconduct. Corruption retries may choose another source, but a failed
aggregate piece check must not arbitrarily blame one of several contributors.

The core does not open sockets, query DHT, own timers, spawn tasks, choose disk
paths or emit product receipts.

### 8.2 Reusable active runtime

`forge_swarm_runtime` composes those mechanisms into an operational engine
that can be hosted with or without the optional Swarm plugin. It owns:

- peer sessions and incremental metadata/data exchange over Forge.API;
- bounded request scheduling, retries, timers and verification work;
- content-scoped sharing and download operations with explicit cancellation
  and an awaitable close/drain operation;
- provider lookup triggers and availability-registration lifetime through
  supplied capabilities, not DHT maintenance or network topology;
- admission checks and reservation of product-granted byte allowances;
- global/per-peer resource limits and transport facts for diagnostics;
- shutdown that stops admission, cancels and joins work, closes calls, releases
  registrations and policy reservations, and flushes resume state.

It uses caller-owned Forge Asio execution and existing Forge API backpressure.
It does not create hidden thread pools, a second P2P node, wire framing or a
payment service. Deterministic mechanisms remain separately testable without
the active engine. Closing a sharing/download operation must not implicitly
delete product-owned files.

### 8.3 Product admission and quota enforcement

Admission means permission for a specific requester to perform a specific
operation on specific content. A quota reservation is an allowance held before
work is accepted, so concurrent requests cannot spend the same budget twice.
These are neutral runtime contracts, not balance or payment DTOs.

- Every remotely accessible metadata and content path uses the same admission
  boundary before disclosing protected data. Free serving is an explicit
  product policy, not a fallback when a paid policy is missing or fails.
- Requester identity comes from the binding's trusted invocation context. For
  P2P this is the authenticated peer, not client-provided metadata. Identity,
  a content hash or a discovery realm alone never grants permission.
- The product supplies a content/requester-scoped permission and quota policy.
  It may obtain those from a separate paid-access API; generic Swarm messages
  do not acquire prices, balances, chain transactions or payment receipts.
- Reserve allowance before accepting outbound data into a queue. Account for
  concurrent and in-flight requests across sessions sharing the same product
  budget; per-session counters alone are insufficient.
- Missing, denied, expired, exhausted or revoked permission prevents new data
  admission. Revocation cancels/drains bounded accepted work; bytes already
  handed to a transport cannot be recalled. The allowed in-flight bound and
  terminal accounting must be explicit and tested.
- Cancellation, timeout, store failure and disconnect finalize each reservation
  exactly once. Do not treat transport acceptance as confirmed useful delivery
  or automatically refund bytes whose delivery is unknown.
- Product policy calls must be bounded/cancellable where asynchronous and must
  not run under runtime state locks. Policy failure must not trigger an
  unrestricted endpoint, origin fallback or a second transfer path.
- Forge reports distinct requested, queued, sent/received and verified facts.
  Product policy defines billable units, receipts and settlement; these facts
  alone are not proof of remote receipt or protection against traffic fraud.

The same runtime must pass tests with an explicit free policy and a synthetic
quota-controlled policy. Production payment integration stays downstream.

### 8.4 Execution and parallel ownership

Swarm decides **what** to transfer and from whom; Forge Asio executes admitted
work. The picker is content policy, not a replacement for `task::scheduler`.
Scheduler priorities are numeric; Swarm owns their meaning and must not add
content-specific priorities to Asio.

| Work | Existing mechanism | Ownership |
| --- | --- | --- |
| Peer reads/writes and metadata exchange | Forge.API live-stream awaitables | Runtime-owned calls with independent read/write progress |
| Delayed retries and priority admission | `forge::asio::task::scheduler` | Caller-owned scheduler, runtime-owned task handles |
| SHA-256 and Merkle verification | `forge::asio::compute::executor` | Bounded caller-supplied CPU pool |
| Blocking file reads/writes/sync | Separate `compute::executor` | Bounded caller-supplied disk pool |
| Short transfer-state transitions | Boost.Asio strand | Per-distribution state owner, not an OS thread per distribution |
| Exclusive async operation where necessary | `forge::asio::gate` | Narrow operation ownership through a move-only ticket |
| Waiting for changed state | `forge::asio::notification` | Epoch-based, cancellable wait without polling or lost wakeups |

The current scheduler runs synchronous `.work` on its underlying Asio runtime;
`max_blocking_tasks` is an admission limit, not a separate worker pool. Do not
place hashing, blocking filesystem calls or `fsync()` directly in such work.
An orchestration coroutine explicitly awaits compute execution and propagates
its stop token. See the [Asio execution contract](../../libraries/asio/README.md)
and [scheduler implementation](../../libraries/asio/task.cpp).

Different distributions and peer calls progress concurrently. Within one
distribution, only short updates to availability, request ownership, picker
state and completion generation are serialized. No state mutex is held while
awaiting network, policy, CPU or disk work. A strand alone does not make an
operation spanning suspension atomic: reserve state before suspension, then
check ownership/generation before applying its result. A late result must not
revive a canceled request or publish data for a replaced operation.

For example, peers A and B can deliver different pieces while peer C receives
an already verified piece, CPU workers check earlier data and disk workers
write independent ranges. Conflicting writes and file close/publication are
coordinated by the store. Endgame duplicates do not become concurrent writers
to the same committed range.

Keep execution queues shallow and bounded. Do not create a task for every
block of the whole file in advance, or occupy all awaitable scheduler slots
with parents waiting for children that need those same slots. Numeric priority
does not preempt a running hash or syscall, and the compute pool is FIFO;
content fairness and bounded work sizes remain Swarm responsibilities.

### 8.5 Transfer pipeline and resource bounds

```text
product-selected content identity / descriptor trust
  -> bounded provider discovery -> authenticated peer call -> admission
  -> validated metadata and peer inventory -> picker -> bounded requests
  -> receive / verify / write staging ranges -> verified store availability
  -> HAVE and serving of verified pieces

verified written progress -> file sync -> durable resume checkpoint
complete file set -> file sync -> atomic publication -> parent-directory sync
```

Verification and staging writes may overlap, but unverified bytes never become
available for serving. Content hashes establish integrity against the selected
descriptor, not product authorization or payment. DHT providers and remote
inventories are hints until verified through the relevant boundaries.

Bound active peers/calls, outstanding requests, metadata, received and outbound
buffers, unfinished pieces, verification jobs and disk work. Enforce both item
counts and byte limits globally and per operation/peer, including waiting
submissions and endgame duplicates. Account for buffer ownership across queue
handoffs until the final user releases it; task count alone is not a memory
bound. Existing Forge API flow control remains responsible for its wire queues.

Application reads must not turn bounded API queues into an unbounded Swarm
queue. Capacity admission follows the data through verification/storage, and
slow storage eventually reduces new requests and receive progress. Reads and
writes must remain independently runnable; cancellation and terminal handling
must not depend on an exhausted data-work budget. `gate` is exclusive FIFO
admission, not a weighted byte-budget primitive.

Object Store indexes serve catalog and restart queries, not every block
selection. Active bitmaps/counters/request queues remain bounded algorithm
state. No second indexed catalog, per-block DB object or network queue is
introduced to bypass the existing Forge components.

### 8.6 Cancellation and joined shutdown

Each sharing/download operation owns its tasks, calls and storage holds.
Cancellation propagates to queued work, stream calls, waits and compute stop
tokens. Running CPU work cooperates; an already entered filesystem syscall
may return only later. Do not claim hard cancellation of that syscall, destroy
its buffer or close its file while it still uses them.

Shutdown follows this ownership order:

1. Close admission and stop creating requests, retries and new task submissions.
2. Release owned provider registrations and stop advertising service; already
   published remote DHT records may remain until expiry.
3. Cancel calls, timers and waits; await all owned workers and outstanding
   storage operations, including late completions and quota finalization.
4. Save a valid final checkpoint where storage permits, reporting failures
   rather than marking unsaved progress durable; release storage holds.
5. Return control before application composition stops shared executors, P2P
   or persistence. Swarm never shuts down those shared dependencies itself.

One operation's cancel/close must not stop another distribution or cancel an
unrelated caller's task on a shared scheduler. Shutdown acceptance measures
actual drain and retained resources, not just an early cancellation response.

## 9. Transport-Neutral Swarm API

`forge_swarm_protocol` owns the serializable request/response and message
records. `forge_swarm_api` owns the shared client/server interfaces over those
records. Both are consumable without Swarm DB, a storage driver or a plugin.
The library runtime
implements the server and uses the generated proxy as a client. The optional
plugin only mounts that implementation in the application's API publication.

The expected remote surface has two logical operations:

```cpp
metadata(swarm_id) -> server_stream<metadata_chunk>
exchange(duplex_stream<peer_message, peer_message>)
```

Metadata exchange supports hash-only, magnet-like bootstrap without requiring
the descriptor to be obtained from a central service. Metadata is chunked and
bounded, and the complete canonical bytes must match the requested swarm id.

Peer messages cover:

- handshake, realm and descriptor identity;
- inventory/bitfield and `HAVE`;
- interest, choke and unchoke;
- piece/hash request, data, reject and cancel;
- typed terminal errors.

These are explicit protocol records, not serialized ObjectDB models. Runtime
observations such as a peer's inventory are kept distinct from locally verified
storage facts; a DTO-to-model mapping cannot promote untrusted claims.

Piece data items are bounded blocks, initially no larger than 16 KiB. Large
pieces are never serialized as one unbounded DTO.

## 10. Swarm Node Plugin

`forge_plugins_swarm_node` is an optional thin application adapter. It:

- installs local control and remote peer APIs;
- publishes the peer API through `plugins.net.p2p.node`;
- supplies adapters over the official DHT and typed remote API capabilities;
  resolver catalog lookup is optional when the protocol is already known;
- maps thematic configuration and existing application execution resources
  into runtime options, without another scheduler or network loop;
- composes file storage and the supplied named Object Store through the library
  storage adapter when catalog persistence is selected; it does not implement
  its own catalog, schema codec or DB-to-protocol mapper;
- accepts product-selected stores and admission/quota policies, with explicit
  dependency lifetime and no implicit allow-all configuration;
- exposes local control and diagnostics using the runtime's own types/state;
- closes publication admission and awaits runtime cleanup before releasing
  P2P, policy, storage or executor dependencies during reverse shutdown.

The plugin does not:

- accept a user download directory as framework policy;
- decide whether a descriptor is trusted;
- allocate product quotas or own retention, pin or seed-duration policy;
- call a product origin automatically;
- understand payments, receipts, erasure coding, snapshots or model ids;
- store payload bytes in a framework database;
- require all peers to use P2P when another binding supports the API method
  kinds.

It must contain no independent picker, transfer-session engine, retry timers,
quota ledger or provider-maintenance loop. A product may embed the library
runtime directly, but when the official P2P plugin owns the shared node it
must still use that plugin's safe capabilities, not bypass its ownership.

For P2P, discovery is DHT-based. For configured HTTP/WebSocket endpoints, peer
location is supplied by the host product; DHT is not forced onto unrelated
transports.

## 11. Product Ownership And Swarmd

The distinction is explicit:

- **Forge feature:** a descriptor denotes actual files; a generic adapter can
  download, resume, verify, publish and seed those files.
- **Product feature:** the user chooses source/destination, resolves name
  conflicts, imports a file, opens a magnet, controls quota/retention and sees
  progress through a CLI or UI.

Therefore a file distribution is not deferred to `swarmd`. Safe file-tree and
filesystem-store mechanics belong to Forge. A user-facing downloaded file in a
chosen directory is a `swarmd` workflow.

Two supported product compositions use the same runtime and optional plugin:

| Product role | Admission and quotas | Product-owned concerns |
| --- | --- | --- |
| Paid provider | Permission from a paid entitlement or funded budget; reserve before sending | Prices, payer, durable accounting, receipts and settlement |
| Free distribution daemon | Explicit free-serving permission with owner-selected resource limits | Files, trusted sources, paths, retention and seeding duration |

Free does not mean unlimited or public: access policy is independent of price.
Paid serving does not make an immutable public object exclusive; a recipient
may seed the same public bytes for free. The paid provider supplies its own
delivery/availability service. Private content requires separate product
authorization regardless of the charging model.

Neither composition copies runtime orchestration into a product. The concrete
downstream mapping is recorded as provenance in section 12.6, not in Forge
public types or configuration vocabulary.

## 12. Donor Traceability

### 12.1 BitTorrent specifications

Reviewed:

- BEP 3, peer protocol, pipelining, request/piece/cancel, choke and endgame:
  <https://www.bittorrent.org/beps/bep_0003.html>;
- BEP 9, peer metadata exchange and magnet bootstrap:
  <https://www.bittorrent.org/beps/bep_0009.html>;
- BEP 52, file tree, per-file alignment, SHA-256 Merkle trees and piece layers:
  <https://www.bittorrent.org/beps/bep_0052.html>.

Accepted:

- symmetric bidirectional peer exchange;
- 16 KiB bounded request/data blocks;
- per-file piece alignment and Merkle roots;
- verified `HAVE` publication;
- metadata-by-hash bootstrap;
- rarest-first, pipelining and endgame cancellation.

Rejected:

- bencoding as the native Forge serialization;
- tracker dependence;
- protocol-specific sockets outside Forge API/P2P;
- assuming BitTorrent v1 SHA-1 identity.

### 12.2 libtorrent

Reviewed local donor commit `b4e9e2471e7a`:

- `include/libtorrent/file_storage.hpp`;
- `include/libtorrent/part_file.hpp`;
- `include/libtorrent/piece_picker.hpp`;
- `include/libtorrent/add_torrent_params.hpp`;
- `src/piece_picker.cpp`;
- `src/merkle_tree.cpp`;
- `src/mmap_storage.cpp`.

Accepted:

- explicit file-to-piece mapping;
- ordinary-file save paths and local rename mapping;
- separate unfinished, have and verified state;
- part/staging state for incomplete content;
- availability-ranked picker and bounded endgame behavior;
- limiting unfinished-piece count/bytes and preferring partial completion
  under pressure, as implemented in `piece_picker::pick_pieces`;
- explicit request ownership rather than a task for every future block.

Rejected:

- copying the libtorrent session, socket and disk-thread architecture;
- exposing donor storage types in Forge public API;
- one process-global disk subsystem.

Forge already owns Asio execution, P2P identity/transports and typed APIs.

### 12.3 Syncthing

Reviewed local donor commit `1919c89de488`:

- `lib/model/sharedpullerstate.go`;
- `lib/model/folder_sendrecv.go`;
- `lib/fs/tempname.go`;
- `lib/fs/basicfs.go`.

Accepted:

- hidden temporary files;
- sparse/block-oriented writes;
- block verification and retry from another source before accepting content;
- coordination of positional writes with file close;
- rename from staging to final path;
- resuming through separate state instead of a visible partial final file.

Rejected:

- mutable folder synchronization semantics;
- conflict-copy and device-version-vector policy;
- product filesystem watching in the Forge library;
- treating a failed file sync as successful durable completion.

The inspected `sharedpullerstate.go::lockedWriterAt.SyncClose` attempts sync but
logs and ignores its failure. Forge's durable checkpoint/publication contract
is deliberately stricter: preserve useful staging data, report the failure and
do not advance the durable checkpoint. Donor use is mechanism-level, not a
claim that every donor durability policy is adopted unchanged.

### 12.4 libp2p DHT and Kubo

Reviewed:

- `go-libp2p-kad-dht` commit `d40b14b5b2cc`, especially `routing.go`,
  `amino/defaults.go` and provider storage;
- Kubo commit `e931b379e7dc`, provider composition, Bitswap integration and
  blockstore/filestore boundaries.

Accepted:

- expiring provider records;
- periodic reprovide before record expiry;
- bounded asynchronous provider lookup;
- separation of peer discovery from content transfer.

Rejected:

- making an IPFS-style blockstore the only payload representation;
- importing Kubo DAG, repository or GC policy;
- coupling Forge swarm identity to CID semantics in v1.

### 12.5 gRPC and Boost.Asio

Reviewed:

- gRPC flow-control guide:
  <https://grpc.io/docs/guides/flow-control/>;
- Boost.Asio `experimental::channel` and `basic_concurrent_channel`:
  <https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/experimental__channel.html>.

Accepted:

- receiver-capacity-driven flow control;
- writes that suspend when bounded capacity is exhausted;
- independent stream directions;
- close/cancel semantics and executor-associated completion.

The implementation must avoid the documented duplex deadlock pattern by
allowing reads and writes to progress independently.

Existing Forge execution is the implementation substrate, not a new donor
runtime to port. The reviewed `forge.asio.task`, `compute`, `gate` and
`notification` contracts map to section 8.4. In particular, synchronous
scheduler work does not isolate blocking I/O; compute has bounded pending and
waiting admission with cooperative cancellation. Swarm supplies policy and
operation ownership without duplicating these primitives.

### 12.6 Downstream decision record

The maintainer decision of 2026-10-16 maps paid serving to Storlane `providerd`
and free distribution to `swarmd`. Both consume the same `forge::swarm` engine,
optionally through `forge_plugins_swarm_node`. This records the use case, not a
claim that either integration has been implemented.

Storlane owns payment admission, shared budgets and settlement. Forge owns
enforcement of supplied permissions/allowances and neutral transfer facts.
An ordinary provider holding erasure fragments is not automatically a Swarm
seed: a product bridge must supply verified canonical pieces when needed.
Erasure reconstruction, paid receipts and automatic origin fallback are not
introduced into Forge by this plan.

The first separate downstream pilot remains finalized Spine snapshots. Free
and quota-controlled adapters are both required in Forge's own fixtures; the
pilot does not replace them or prove production payment correctness.

### 12.7 Spine structural reference

Inspected local Spine commit `644e699b5ca2a8ed73d0f62680e79b239b65b954`:

- `libraries/chain/db/include/spine/chain/db/signed_block.cppm`: persisted model
  composition and Forge Object Store index descriptors;
- `libraries/chain/protocol` and `libraries/chain/api`: separate network values
  and remote-capable contracts;
- `libraries/chain/conversion/include/spine/chain/conversion/builder.cppm`,
  its paired implementation and README: explicit builders over already-read
  records, identity/completeness checks and no owned DB handle or hidden reads.
- `libraries/conversion/include/spine/conversion/builder.cppm`: its common
  primary template is only `template <class Result> class builder;`. Forge keeps
  the equivalent customization declaration in existing Core, rather than
  copying this one-declaration standalone library.

The current reference is `chain.conversion`, not the historical
`chain.db.protocol_builder` name. Adopt its ownership boundary, not a copied
generic builder framework, blockchain fields or an assumed universal reverse
mapping. Spine code remains a reference, never a Forge dependency. Swarm keeps
its already agreed reusable library runtime even though Spine's product
lifecycle has different ownership rules.

## 13. PR Delivery Plan

This revision plans eight sequential Forge PRs and a separate downstream pilot.
It splits the former plugin-runtime block into a reusable library PR and a
thin-plugin PR and adds a focused indexed-metadata PR. These are planned
deliverables, not completed work; this document update authorizes no
implementation or downstream migration.

| PR | Suggested branch | Deliverable |
| --- | --- | --- |
| 1 | `forge-swarm-descriptor-v1` | Core conversion declaration; canonical descriptors, identities and Merkle validation |
| 2 | `forge-swarm-file-store-v1` | Storage contract, ordinary files and crash-safe resume |
| 3 | `forge-swarm-db-v1` | Object Store metadata models, indexes and catalog/checkpoint adapter |
| 4 | `forge-swarm-transfer-v1` | Deterministic picker and transfer mechanisms |
| 5 | `forge-swarm-peer-api-v1` | Protocol DTOs, peer API and domain conversion specializations |
| 6 | `forge-swarm-runtime-v1` | Reusable active runtime and product policy enforcement |
| 7 | `forge-swarm-node-plugin-v1` | Thin official application adapter |
| 8 | `forge-swarm-acceptance-v1` | Integrated multi-peer, hostile-input and recovery evidence |

### 13.1 Existing substrate and entry gate

Before implementation, record the current Forge/P2P base SHA and inspect the
actual public APIs. Live streaming, authenticated invocation context,
publication ownership and official DHT capabilities are reuse prerequisites,
not new Swarm implementations. Existing contracts must not be copied or
weakened to make the plan fit.

Verify `find provider -> typed remote API -> duplex exchange` through the
official P2P plugin before completing PR7. Resolver is optional API catalog
discovery on a known peer, not another content-discovery mechanism. Keep the
P2P Stage 6/7 development gates and independent Stage 8 production gate from
the linked production roadmap; low-level codecs alone do not satisfy them.

### PR1: Descriptor, identity and proofs

- **Owner/API:** `forge_swarm_core`; descriptor/file-entry/swarm-id values and pure
  validation, identity and piece-proof operations. Freeze exact Raw field
  widths and limits in this PR; no runtime or plugin dependency.
- **Shared prerequisite:** add only the `forge.core.conversion` primary template
  declaration to existing `forge_core`. Prove an installed Core consumer can
  declare and use its own explicit specialization without linking any Swarm,
  DB, reflection or API target. Domain implementations follow in PR5.
- **Reuse/donor:** Forge Raw, reflection and SHA-256; BEP 52 and libtorrent's
  file/piece and Merkle rules. Do not substitute Chain Merkle semantics or
  modify a Chain wire contract for Swarm.
- **Acceptance:** hardcoded descriptor/id/proof fixtures, independent donor
  vectors, empty/non-power-of-two file cases, malformed proofs, arithmetic
  overflow, metadata bounds and unsafe path rejection. Install/package consumer
  imports only the modules it needs. Forge Raw is not BitTorrent wire parity.

### PR2: File storage and resume

- **Owner/API:** `swarm.core` storage/checkpoint contracts and
  `forge_swarm_storage` file-backed implementation; verified-range reads,
  staged writes, verified-piece commit, checkpoint/reopen and final publication.
  Exact operation signatures follow the ownership in section 7. This PR's
  standalone sidecar remains one supported checkpoint backend, not a future
  second source of truth beside the DB adapter.
- **Reuse/donor:** caller-owned Forge Asio compute executor; libtorrent file
  mapping and partial state, Syncthing temporary-file/rename patterns. Preserve
  Forge's explicit durability contract rather than silently accepting sync
  failures from a donor path.
- **Acceptance:** ordinary single/multi-file output, sparse/random writes,
  crash points before/after piece and final commit, concurrent reads, corrupted
  sidecars, symlink/path races, destination conflicts and cross-filesystem
  rejection. No payload DB or hidden disk thread pool.

PR2's concrete contracts are `forge.swarm.core.store` and
`forge.swarm.core.checkpoint`, implemented by
`forge.swarm.storage.file_store` (`swarm_storage` component). A store binds one
validated descriptor/destination and separate caller-owned disk/CPU executors.
`begin(piece_id)` grants exclusive writer ownership; complete-block positional
writes, authenticated commit and discard are distinct operations. Reads verify
the actual piece buffer before returning a bounded range. Snapshot/change
notification, leases, explicit checkpoint/publication and awaited close expose
their own ownership rather than a hidden runtime.

Default bounds are 16 unfinished pieces, 8 active I/O operations and 1 MiB
per write/range request. A versioned checksummed Raw restart cache holds
identity, generation, verified bitmap and unfinished block hints. Reopen
rehashes payload independently. An optional `checkpoint_store` selects external
metadata instead of the sidecar; PR3 supplies ObjectDB, not a second authority.
Native POSIX publication is an atomic no-replace rename of one same-filesystem
root. Unsupported atomic semantics fail explicitly. Parent credentials and
root ownership remain trusted local administration boundaries; process-crash
tests do not constitute physical power-loss or production proof.

### PR3: Indexed metadata and persistence adapter

- **Owner/API:** `forge_swarm_db` declares metadata models and index descriptors;
  `forge_swarm_storage` supplies the reusable catalog/checkpoint adapter over
  externally owned Object Store services. Core contracts keep the runtime
  independent of concrete DB models and drivers.
- **Reuse/reference:** Forge Object Store's object/index DSL, transactions,
  snapshots and pagination; the Spine DB/protocol boundary documented in
  section 12.7. No local object/index framework or hand-written storage codec.
- **Acceptance:** identity uniqueness, bounded indexed state/priority/expiry
  queries, commit/rollback, reopen, schema mismatch and persistence failures;
  memory/MDBX/RocksDB parity through Forge drivers. Prove no full-history scan,
  no payload-in-DB storage and safe crash ordering between file durability and
  metadata commits. A stale row must not make missing bytes available.
- **Authority:** test sidecar-backed and DB-backed checkpoint modes separately;
  one operation has exactly one checkpoint authority. Schema/model registration
  occurs in composition before startup, never through model import side effects.

PR3 freezes metadata schema v1 in `forge.swarm.db.content` and
`forge.swarm.db.schema`: a version singleton and one content object with
canonical identity/descriptor, local opaque locator, job state/priority, optional
expiry (Unix seconds; `INT64_MAX` means no expiry) and optional PR2 resume state.
Identity and locator are unique; ordered state/descending-priority and expiry
indexes serve bounded cursor queries. Related fields and indexes change in one
Object Store transaction. There are no payload or per-network-block objects.

`forge.swarm.storage.catalog` adapts an externally owned Object Store. Composition
explicitly registers models, supplies separate CPU and database executors, and
retains DB lifecycle ownership. The DB executor is necessary because a RocksDB
awaitable does not itself offload blocking work. Catalog requires single-writer,
monotonic-ID Object Store policy and bounds in-flight work and page sizes.
Checkpoint handles bind one immutable object ID, identity and locator; erase or
replacement cannot revive an old handle. Saves validate monotonic generations,
commit then flush the same driver; identical retries repeat the durability fence.
Checkpoint loads restore that fence after ambiguous prior commit errors. Existing
file verification and sync ordering remain PR2's authority. Expiry selection and
metadata erase do not silently delete payload or decide product retention policy.

### PR4: Deterministic transfer mechanisms

- **Owner/API:** `forge_swarm_core`; availability, picker and transfer state with
  explicit time inputs, piece priorities, bounded pipelining, endgame and
  timeout/cancel transitions.
- **Reuse/donor:** BEP 3 and libtorrent picker/choke patterns; reuse existing
  Forge numeric scheduling primitives where execution is needed later. Do not
  enforce reciprocal upload as a mandatory admission policy for all products.
- **Acceptance:** reproducible simulations, rarest-first diversity, duplicate
  response handling, request ownership, endgame cancellation and bounded
  state under long sequences. No sockets, timers or plugin lifecycle in these
  deterministic mechanisms.

### PR5: Protocol DTOs and peer API

- **Owner/API:** `forge_swarm_protocol` for network DTOs;
  `forge_swarm_api` for bounded metadata server stream and duplex peer-exchange
  interfaces. Concrete runtime service implementation follows in PR6; no empty
  placeholder service. Values are reused from Core, not redefined in API or DB.
- **Reuse/donor:** Forge.API live streams, Raw, trusted invocation and existing
  authenticated-peer context; BEP 9 metadata bootstrap and BEP 3/52 exchange
  semantics, not a copied BitTorrent framing implementation.
- **Acceptance:** metadata hash verification, malformed/oversized messages,
  simultaneous read/write, half-close, per-call cancellation and context
  spoofing tests. Define the identity/admission handoff for PR6 without putting
  payment fields into generic wire DTOs. A content id or realm is not access
  authorization; unsupported transport method kinds fail explicitly.
- **Conversion:** `swarm.conversion` supplies explicit specializations of
  `forge::core::conversion<Result>` for actual metadata responses and validated
  import paths. It imports `forge.core.conversion`; no new generic conversion
  library or domain-local primary template is created. Test private-field omission,
  identity/completeness rejection and rejection of remote local-state claims.
  Builders never own DB I/O or commit; a blind DTO-to-persisted-record save is
  forbidden. Installed protocol/API consumers must not link Swarm DB/storage.

### PR6: Library runtime and policy enforcement

- **Owner/API:** `forge_swarm_runtime` in `forge::swarm::runtime`; active runtime,
  sharing/download handles, bounded status/facts and neutral admission/quota
  contracts. Proposed module vocabulary is `engine`, `admission` and `quota`;
  finalize names and exact pairs against real entities, not arbitrary slices.
- **Composition:** implement the server and generated-client orchestration
  once; accept storage, execution, trusted context and connection/discovery
  capabilities. Reuse Forge API call cancellation, task ownership and compute
  execution; no library dependency on Forge.App or plugin implementations.
- **Policy contract:** enforce section 8.3 on every data path, including metadata,
  resumed transfers, duplicate/endgame requests and reconnects. Free serving
  requires explicit permission. Paid entitlement negotiation and accounting
  persistence remain product-owned and use a separate product API if needed.
- **Acceptance:** run without loading the Swarm plugin; exercise both explicit
  free serving and synthetic funded-quota adapters. Prove concurrent shared
  budget reservation, exhaustion, expiry/revocation, cancellation and exactly-once
  reservation finalization, including partial/unknown-delivery failures. Prove
  joined shutdown and no hidden unlimited or origin fallback path.

### PR7: Thin Swarm node plugin

- **Owner/API:** `forge_plugins_swarm_node`; `plugin::descriptor()`, typed local
  control, thematic config and diagnostics. Expose library operation handles
  and values without parallel DTO/state registries.
- **Composition:** official P2P capabilities supply discovery/publication and
  remote calls; Forge.App supplies lifecycle and execution dependencies;
  products supply stores and admission/quota policies. ObjectDB catalog mode
  obtains its named store through `plugins.db.store` and composes the PR3
  library adapter. Standalone file mode has no mandatory DB Store service.
  The plugin delegates sessions, timers, picking and limits to the PR6 runtime.
- **Acceptance:** prove programmatic-runtime/plugin parity, configured free and
  quota-controlled service, missing-policy fail-closed behavior, publication
  replacement, no premature serving, reverse shutdown and dependency lifetime.
  Include DB-backed restart parity and indexed catalog queries without scans.
  The same paid policy must not be bypassable through metadata, a second route
  or an alternate binding. No copied business or network orchestration.

### PR8: Integrated acceptance

- **Evidence:** at least three real peers, provider discovery by an independent
  node, partial downloader-to-seed promotion, mixed explicit free and synthetic
  quota-controlled peers, ordinary final files and content integrity.
- **Failure coverage:** corruption, restart, interrupted writes, slow readers,
  disconnect/reconnect, exhausted/revoked shared budget, queue pressure and
  cancellation at every blocked stage. Record resource bounds and shutdown
  completion rather than treating an exit code alone as proof.
- **Transport:** exercise native QUIC, TCP Noise/TLS with Yamux and the supported
  private TCP profile through official APIs. Go/Rust live evidence proves the
  underlying libp2p transport/relay composition, not interoperability with the
  custom Forge Swarm application protocol. Do not claim IPFS/Bitswap or
  BitTorrent wire support from donor-derived algorithms.
- **Delivery:** record fixed refs, donor paths, commands, raw results and honest
  PASS/FAIL/NOT_RUN gates. Synthetic quota tests do not prove a real payment
  protocol. Broader P2P Stage 8 production proof remains independent.

### Shared delivery gates and downstream pilot

Each implementation PR starts from the current `origin/dev`, follows both
repository skills, includes focused unit/integration and installed-package
tests, `test_forge_structure` and `git diff --check`, and receives independent
exact-head review. Builds are coordinator-owned in one tree with `-j4`; do not
run the full CI matrix. Remove only confirmed-inactive task build outputs after
acceptance and preserve reports, logs and checksums. Do not modify neighboring
worktrees or invoke a second runtime just to bypass a failing integration.

The separate downstream pilot registers finalized snapshot directories and
compares bootstrap latency/origin load with the existing product path. Its
product mapping is in section 12.6. Paid-provider integration, durable payment
accounting and a user-facing free daemon are separate downstream work; Forge
fixtures cover their neutral policy boundary, not their business acceptance.

Joint multi-process Swarm/P2P tests supplement rather than replace P2P raw-node,
official-plugin and Go/Rust evidence. Production requires the applicable P2P
Stage 8 gates and Swarm's own gates below to pass independently. Stage 9
WebSocket support does not block native Swarm deployment and must not be
inferred from native readiness.

## 14. Required Validation

The feature is not production-ready after a loopback transfer. Required gates
include:

- live stream ordering, half-close, cancellation, deadline and bounded-memory
  tests on every supported binding;
- provider TTL, reprovide, stale provider and resolver failure tests;
- descriptor Raw fixtures and path traversal/collision rejection;
- single-file and multi-file actual-output tests;
- crash/reopen tests around piece commit and final rename;
- resume with verified and partial blocks;
- concurrent readers while downloading and seeding;
- corrupted metadata, piece and Merkle proof rejection;
- rarest-first diversity and endgame cancellation tests;
- bounded partial-piece pressure, explicit-priority/sequential modes and
  reproducible time/random inputs;
- cancel/response and duplicate-winner races, stale completion generations and
  integrity-failure attribution with multiple contributing peers;
- peer flood, oversized request, slow reader and bounded queue tests;
- slow-disk/CPU saturation with network and cancellation progress; global and
  per-peer byte accounting across queue handoffs, not only task counts;
- no scheduler parent/child admission deadlock, lost state-change wakeup or
  cross-distribution cancellation on shared executors;
- bounded checkpoint batching, sync-failure rejection and joined shutdown with
  in-flight file work; no file close or buffer release before the last user;
- at least three real peers with downloader-to-seed promotion;
- indexed Object Store metadata queries, transaction/reopen/backend parity and
  no second catalog/index engine;
- protocol-only/API-only installed consumers independent of Swarm DB/storage;
- explicit DB-to-protocol mapping, no private metadata exposure or automatic
  trust promotion on protocol-to-DB input;
- installed Core-only customization and installed domain-specialization consumers,
  without reverse dependencies or duplicate primary templates;
- independent DB schema/wire fixtures and single-authority checkpoint recovery
  across filesystem/DB crash boundaries;
- library-runtime/plugin behavior parity with no duplicated orchestration;
- explicit free-serving and quota-controlled policies using the same runtime;
- fail-closed admission for metadata and data, authenticated-context spoofing,
  shared-budget races, in-flight bounds, revocation and exactly-once reservation
  finalization on cancellation/failure;
- distinct transport/verification facts without claiming payment proof;
- package consumers and `test_forge_structure` for every new target;
- no import from the Swarm library into product or plugin implementation
  namespaces, and no reverse dependency from API Core or network substrates
  onto Swarm; storage adapters must not introduce DB/Chain ownership into
  deterministic Swarm mechanics.

## 15. Explicit Non-Goals For v1

- mutable distributed files;
- erasure-coded swarm completion;
- paid bandwidth receipts or marketplace;
- implementation of product authorization/payment policy and private-key
  distribution; neutral admission/quota enforcement is required by section 8.3;
- automatic product origin fallback;
- DB-backed user payload;
- tracker compatibility;
- native `.torrent` import/export;
- a Forge-owned daemon;
- replacement of chain synchronization or product storage protocols.

## 16. Open Decisions For Focused Plans

The following are intentionally finalized in their owning implementation block:

- exact stream method signatures and cancellation token shape;
- exact runtime capability adapters over the existing profile-aware P2P APIs;
- canonical Raw descriptor field widths and maximum metadata size;
- default piece size policy for snapshot, model and user-file workloads;
- file-store sidecar name and platform durability matrix;
- exact indexed metadata schema, query coverage and checkpoint-authority
  configuration, plus the concrete result/input pairs and composition methods
  implemented by domain conversion specializations;
- choke policy profiles for reciprocal public swarm versus validator snapshot
  distribution;
- exact admission/grant/reservation signatures, byte-counting units, bounded
  revocation behavior and terminal reporting of unknown delivery;
- plugin local control API and metrics names.

These details may not weaken the boundaries, actual-file guarantee, bounded
resource behavior or donor-derived correctness rules established above.
