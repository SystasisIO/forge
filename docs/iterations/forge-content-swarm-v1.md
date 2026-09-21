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

1. `forge_swarm` owns deterministic descriptor, Merkle, piece, picker,
   transfer and resume mechanics, the storage contract and its file-backed
   implementation.
2. `forge_swarm_api` owns the transport-neutral peer `FORGE_API`
   contract and its wire DTOs.
3. `forge::swarm::file_store` implements the storage contract within
   `forge_swarm`. It writes ordinary files and directories; a separate generic
   content-storage library is not introduced without an independent use case.
4. `forge_plugins_swarm_node` binds those libraries to Forge application
   lifecycle, API publication, P2P discovery, peer sessions, limits and
   diagnostics.
5. Host products own descriptor trust, source and destination selection,
   authorization, quota, retention, seeding duration and origin fallback.

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
  CMakeLists.txt
  README.md
  include/forge/swarm/
    file_store.cppm
  details/
  file_store.cpp

plugins/swarm/node/
  CMakeLists.txt
  README.md
  include/forge/plugins/swarm/node/
  details/
```

Planned targets and package components:

| Target | Component | Namespace |
| --- | --- | --- |
| `forge_swarm` | `swarm` | `forge::swarm` |
| `forge_swarm_api` | `swarm_api` | `forge::swarm` |
| `forge_plugins_swarm_node` | `plugins_swarm_node` | `forge::plugins::swarm::node` |

Library modules use the `forge.swarm.*` prefix; the file store module is
`forge.swarm.file_store`. The plugin module prefix and contract id are both
`forge.plugins.swarm.node`. Its role is ownership of the Swarm runtime, not
ownership of a second network node. The `forge::plugins::swarm` grouping
namespace contains no public declarations.

`forge_swarm_api` is a focused target declared in the same physical
`swarm` library, as Crypto asymmetric values are separated from the heavy
algorithm target. It must not force API dependencies on deterministic swarm
mechanics. Each source/module has one target owner. Domain API declarations
remain in `forge::swarm`; they do not create an `api` child namespace or a new
transport binding. Non-template implementations use exact public/private
pairs, and aspect units split only coherent implementations.

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

The low-level P2P node already provides DHT operations, where DHT means a
distributed hash table:

```cpp
auto availability = co_await async_provide(profile, dht::key);
async_find_providers(dht::key);
```

They are not exposed through the safe local `plugins.p2p.node` API. Add a
focused provider-discovery slice that exposes only typed provide and provider
lookup operations with bounded results, cancellation and deadlines.

The source audit also found broader official-plugin integration gaps around
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
  -> resolver.remote<swarm peer API>(peer)
  -> Forge API bidirectional stream
```

The provider key is domain-separated from other DHT records and derived from
the swarm realm plus swarm identity. The content plugin retains the move-only
`provider_registration` for exactly as long as local content remains
available. The P2P node owns initial publication, bounded jittered republish
and withdrawal on registration release or shutdown. Remote provider records
expire naturally; restart requires the product to confirm availability and
create a fresh registration.

No public raw `open_protocol_stream()` escape hatch is added for Swarm.

## 6. Content Descriptor And Identity

The descriptor describes an immutable single-file or multi-file distribution,
not one anonymous byte array:

```cpp
struct file_entry {
   std::vector<std::string> path;
   std::uint64_t size;
   forge::crypto::digest::sha256 root;
};

struct descriptor {
   descriptor_version version;
   std::uint32_t piece_size;
   std::vector<file_entry> files;
   piece_hash_layer pieces;
};
```

The final exact field types are fixed with Raw fixtures before publication.

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

The v1 descriptor is Forge Raw, not a claim of `.torrent` wire compatibility.
The file/piece layout deliberately follows BitTorrent v2 closely enough for a
future explicit import/export bridge. Text magnet representation is an outer
boundary and may use `forge_multiformats`; it is not required by swarm
mechanics.

## 7. Filesystem Storage

### 7.1 Store contract

`forge_swarm` defines a storage contract with these capabilities:

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

`forge::swarm::file_store`, shipped by `forge_swarm`, implements the contract
using ordinary files:

- an existing complete file or directory can be verified and seeded directly;
- a single-file download writes a hidden sibling staging file;
- a multi-file download writes a hidden sibling staging directory;
- staging is on the same filesystem as the final destination;
- payload files are sparse or preallocated where supported and receive data at
  their final logical offsets;
- a small atomically replaced sidecar stores only descriptor identity,
  verified pieces and unfinished block state;
- a piece is advertised only after its bytes and Merkle proof are verified;
- completion syncs files, publishes with atomic rename and syncs the parent
  directory;
- multi-file publication renames one staging root, not individual visible
  files;
- an unexpected existing destination produces a typed conflict; overwrite,
  merge or user rename policy belongs to the product;
- cross-filesystem publication is rejected rather than silently weakening the
  atomicity contract.

The file store uses a caller-owned bounded `forge::asio::compute::executor` for
blocking filesystem and hashing work. It does not create hidden threads or own
an application runtime.

RocksDB, MDBX and DB Blob are not payload backends for the generic file store.
A product may persist catalog or resume metadata in a DB, but completed content
remains directly accessible as normal files.

## 8. Swarm Mechanics

`forge_swarm` owns deterministic mechanisms independent of sockets and
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

The core does not open sockets, query DHT, own timers, spawn tasks, choose disk
paths or emit product receipts.

## 9. Transport-Neutral Swarm API

`forge_swarm_api` owns the shared client/server contract. The plugin
implements the server and uses the generated proxy as a client.

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

Piece data items are bounded blocks, initially no larger than 16 KiB. Large
pieces are never serialized as one unbounded DTO.

## 10. Swarm Node Plugin

`forge_plugins_swarm_node` is the lifecycle-owned reference runtime. It:

- installs local control and remote peer APIs;
- publishes the peer API through `plugins.p2p.node`;
- announces and discovers providers through the DHT slice;
- opens typed remote APIs through `plugins.p2p.resolver`;
- owns active peer sessions, transfer orchestration and timers;
- applies global and per-peer connection, stream, request and bandwidth limits;
- runs picker decisions and verification work against a registered content
  store;
- retains provider registrations only while verified content is available;
  renewal and withdrawal mechanics remain owned by the P2P node;
- exposes progress, availability, peer and error diagnostics;
- emits transport facts such as requested, received and verified bytes;
- stops admission, cancels pending work, closes streams and flushes resume state
  during shutdown.

The plugin does not:

- accept a user download directory as framework policy;
- decide whether a descriptor is trusted;
- own quota, retention, pin or seed-duration policy;
- call Storlane origin automatically;
- understand payments, receipts, erasure coding, snapshots or model ids;
- store payload bytes in a framework database;
- require all peers to use P2P when another binding supports the API method
  kinds.

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

Examples:

- Spine registers a finalized snapshot directory through a read-only adapter.
- An inference product registers its model cache.
- `storlane-swarmd` composes `forge::swarm::file_store`, chooses user-visible
  paths and optionally binds the same descriptor to a durable Storlane origin.

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
- availability-ranked picker and endgame behavior.

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
- sync before final publication;
- rename from staging to final path;
- resuming through separate state instead of a visible partial final file.

Rejected:

- mutable folder synchronization semantics;
- conflict-copy and device-version-vector policy;
- product filesystem watching in the Forge library.

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

## 13. Implementation Order

### Block 1: API and P2P prerequisites

1. Replace vector streaming with live Forge API streams.
2. Add binding capability validation and full P2P/QUIC/WebSocket coverage.
3. Complete P2P Stage 6 and fix its contracts before parallel Swarm development
   starts alongside Stage 7. Earlier partial P2P delivery is insufficient.
4. During Stage 7, expose validated configuration and focused DHT provider
   discovery through `plugins.p2p.node`.
5. Prove `find provider -> resolver -> typed duplex API` through the official
   plugin before enabling Swarm network integration.

Blocks 2 and 3 and the transport-neutral peer contract may proceed alongside
Stage 7 after steps 1 through 3. Network integration in Block 4 depends on steps
4 and 5; no temporary raw-node path or product-owned discovery loop is allowed.
Parallel Swarm work must not delay P2P Stage 7 or Stage 8 delivery.

### Block 2: descriptor and file storage

1. Add canonical descriptor/file-tree values and fixtures.
2. Add Merkle and piece-layer validation.
3. Add the abstract content-store contract.
4. Add `forge::swarm::file_store` within `forge_swarm` and actual-file durability
   tests; do not create a separate content-storage target.

### Block 3: scheduling mechanics

1. Add availability and picker state.
2. Add rarest-first, priorities, bounded pipelining and endgame.
3. Add resume and deterministic transfer/session state.
4. Add randomized donor parity and long-run simulations.

### Block 4: plugin runtime

1. Add the peer `FORGE_API` contract.
2. Implement lifecycle, discovery, API publication and peer sessions.
3. Add limits, cancellation, diagnostics and clean shutdown.
4. Run real multi-peer, corruption, slow-peer, disconnect and resume tests.

### Block 5: downstream proof

1. Integrate finalized Spine snapshot directories.
2. Compare bootstrap latency and origin load against current range sync.
3. Only after that proof, integrate model caches and build `storlane-swarmd`.

Stage 8 includes joint multi-process Swarm/P2P tests of interrupted transfers,
recovery, slow peers, memory bounds and long-duration operation. They supplement
rather than replace P2P raw-node, official-plugin and Go/Rust evidence.
Production use requires the applicable P2P Stage 8 gates and Swarm's own gates
below to pass independently. Stage 9 WebSocket support does not block native
Swarm deployment and must not be inferred from native readiness.

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
- peer flood, oversized request, slow reader and bounded queue tests;
- at least three real peers with downloader-to-seed promotion;
- package consumers and `test_forge_structure` for every new target;
- no import from the Swarm library into product or plugin implementation
  namespaces, and no reverse dependency from API Core or network substrates
  onto Swarm; storage adapters must not introduce DB/Chain ownership into
  deterministic Swarm mechanics.

## 15. Explicit Non-Goals For v1

- mutable distributed files;
- erasure-coded swarm completion;
- paid bandwidth receipts or marketplace;
- product authorization and private-key distribution;
- automatic Storlane origin fallback;
- DB-backed user payload;
- tracker compatibility;
- native `.torrent` import/export;
- a Forge-owned daemon;
- replacement of chain synchronization or product storage protocols.

## 16. Open Decisions For Focused Plans

The following are intentionally finalized in their owning implementation block:

- exact stream method signatures and cancellation token shape;
- exact DHT discovery API options and result streaming;
- canonical Raw descriptor field widths and maximum metadata size;
- default piece size policy for snapshot, model and user-file workloads;
- file-store sidecar name and platform durability matrix;
- choke policy profiles for reciprocal public swarm versus validator snapshot
  distribution;
- plugin local control API and metrics names.

These details may not weaken the boundaries, actual-file guarantee, bounded
resource behavior or donor-derived correctness rules established above.
