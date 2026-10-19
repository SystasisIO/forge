# forge_swarm_core

Core describes immutable single-file and multi-file content and verifies pieces
without storage, network or runtime dependencies. Use it before admitting
untrusted metadata or publishing a content identity. It does not authorize a
descriptor, open a local path, schedule work, or implement BitTorrent/IPFS wire
formats. Public source API: **Preview**. The v1 wire contract below is explicit;
Preview does not permit silently changing persisted bytes.

Target: `forge_swarm_core` (`Forge::forge_swarm_core` when installed).
Direct dependencies: `forge_core`, `forge_crypto_digest`, `forge_raw`, `forge_exceptions` and
`Boost::headers`. Chain Merkle is intentionally not reused: its tree shape and
padding are different.

## Public Modules

| Module | Ownership |
| --- | --- |
| `forge.swarm.core.descriptor` | File entries, descriptor version, independent limits, validation and exact bounded encoding/decoding |
| `forge.swarm.core.merkle` | SHA-256 block/piece trees, streaming file hasher and piece proofs |
| `forge.swarm.core.identity` | `swarm_id` and validated identity calculation |
| `forge.swarm.core.checkpoint` | Bounded versioned resume values and checksummed Raw encoding |
| `forge.swarm.core.store` | Filesystem-neutral asynchronous store/checkpoint contracts, writer ownership and leases |
| `forge.swarm.core.transfer` | Peer availability, piece/block picker, bounded requests and explicit verification transitions |
| `forge.swarm.core.peer_observation` | Connection IDs, explicit chrono time and cumulative observation values |
| `forge.swarm.core.choker` | Bounded serving slots, optimistic exploration and seed rotation |
| `forge.swarm.core.exceptions` | Typed errors over existing Forge exception machinery |

There is no `forge.swarm` or `forge.swarm.core` facade module. Values have no
pimpl or hidden runtime resources. All indices in Merkle APIs are file-local;
there is no ambiguous global piece index.

## Storage Contracts

`store` names Boost.Asio awaitables but acquires no Forge Asio dependency and
does not execute I/O. `store::writer` owns one unfinished piece until commit,
discard or destruction. Blocks may arrive in any order at aligned offsets;
unverified progress never establishes availability. `store::lease` is a
move-only hold which a concrete store drains before closing its resources.
Closing is explicit; destroying a value never implicitly deletes product files.

`checkpoint_store` is the single selected durable metadata authority. A
successful `async_save()` must atomically persist the whole checkpoint and
report failures. The file implementation's sidecar is used only when no
external adapter is supplied. Swarm DB models and the Swarm Storage catalog
adapter provide the externally owned Object Store integration.
An external adapter binds its content/locator and atomically rejects stale or
nonmonotonic checkpoint generations; a successful return is not a queued write.

Checkpoint v1 is canonical Raw `pair<resume_state, sha256>`. The digest covers
the canonical Raw state (not the digest itself). It detects accidental metadata
corruption, not hostile modification: content must still be verified after
reopen. The state contains uint32 version, content identity, uint64 generation,
published flag, little-bit-first verified bitmap and ordered unfinished records
with exact block bitmaps. Bits beyond the declared geometry must be zero;
verified and unfinished entries cannot overlap. At most 64 unfinished pieces
and 1 MiB encoded metadata are accepted. A published checkpoint is complete.
Unknown versions, noncanonical counts, wrong identity/checksum/geometry and
excess allocation are typed `invalid_checkpoint` errors. The decoder uses
bounded Forge Raw rather than a separate codec.

This restart-cache layout is **Preview** independently of the immutable
descriptor v1 format. A future incompatible cache revision requires an explicit
version/reset path; there is no silent legacy decoder.

## Deterministic Transfer

Use `transfer` for single-owner decisions driven by a runtime, not as a network
engine. It owns no payload, store, thread, timer or executor. A caller serializes
its calls, retains returned request records, maps each opaque `peer_id` to one
connection, and applies cancellation records to that connection. IDs are never
reused within an instance. Request IDs and verification generations are scoped
to that instance: do not deliver an old instance's callbacks to a new instance.
The constructor's explicit seed provides reproducible diversity, not entropy
or a security capability. Different clients should supply different seeds.

```cpp
#include <array>
#include <chrono>
import forge.swarm.core.transfer;

namespace core = forge::swarm::core;

void choose(const core::descriptor& descriptor) {
   core::transfer state{descriptor, {}, 41U};
   // Build an exact little-bit-first descriptor-sized bitmap from peer state.
   const auto pieces = core::validate_descriptor(descriptor).total_pieces;
   auto bitmap = std::vector<std::uint8_t>((pieces + 7U) / 8U, 0U);
   if (pieces != 0U) bitmap[0] = 1U;
   const auto peer = state.add_peer(bitmap);
   const auto now = core::transfer_time{std::chrono::milliseconds{1000}};
   static_cast<void>(state.set_choked(peer, false, now));
   const auto requests = state.pick(peer, now, 8U);
   // The external runtime now sends requests, retains payload in its writer,
   // and calls receive(peer, request.id, exact_length, explicit_arrival_time).
   // receive_result.ready identifies a complete UNVERIFIED piece.
   // After actual hash verification, call finish(token, verification_passed).
}
```

`selection::rarest_first` orders by descending explicit priority, then ascending
peer availability, partial-piece preference at equal rarity, and a specified
SplitMix64 seed rank (ordinal breaks remaining ties). Priority zero filters
future picks. Sequential selection uses descriptor file/piece order within each
priority. Unlike libtorrent's weighted priority/rarity buckets, explicit Forge
priority always wins over rarity, including endgame. Under count/byte pressure
(at least three quarters of either unfinished limit), existing partial pieces
win over new pieces within the same priority, even in sequential mode. A pick
uses a maintained ordered index with precomputed seed ranks, lazily filtered
by the requesting peer's bitmap. Only the at-most-64 partial-piece candidates
are sorted per batch and merged with that index. `receive` and `cancel` never
rebuild global order. Verified pieces are removed; discard re-admits them.
Availability updates scan bitmap bytes and reindex only XOR-changed pieces:
O(bitmap bytes + changed pieces * log P). Identical bitmaps are no-ops with no
allocation; sequential order does not depend on nonzero availability counts.
Existing index nodes are rekeyed without allocation. Newly available nodes are
prepared before any counts change, preserving failure atomicity. Priority
changes similarly rekey one node, and policy changes reorder existing nodes
without allocating. Initial/dense availability admission can cost O(P log P);
it is not hidden in each pick. Sparse peer filtering can still scan O(P) index
entries in the worst case, but there is no per-pick full candidate sort. Global
endgame coverage uses an explicitly maintained wanted count and bounded partial
metadata, not a descriptor-wide scan. These mechanisms do not establish network
runtime throughput; initial inventory and steady-state picking are separate
measurement surfaces.

Request count and bytes have independent global and per-peer bounds. Partial
pieces reserve their full actual length before their first request and retain
that reservation through receive-before-verification. Thus completed but
unverified pieces cannot escape the working-set bound. Payload remains entirely
external; the runtime must honor these reservations with its own writer memory
and disk quotas. Hard config ceilings: 4096 peers, 65536 requests, 1 GiB request
bytes, 64 unfinished pieces and 1 GiB unfinished bytes; per-peer caps cannot
exceed global caps. Peer bitmaps use at most 32 KiB each (128 MiB at the peer
ceiling); partial metadata has at most 64 * 1024 block records. The maintained
index has at most 262144 nodes and one precomputed rank per descriptor piece;
an availability update may temporarily stage that many new nodes. Per-pick
candidate views contain at most 64 partial indices, not all descriptor pieces.
Node allocation size and per-node overhead depend on the STL/container ABI;
these bounds do not imply an exact index byte total.
These are ceilings, not defaults or an overall process-memory budget:
object/container overhead, descriptor,
returned snapshots and caller-retained records require additional memory.

Every request terminates once by receive, cancel, timeout, discard, choke,
disconnect or stop. Endgame requires every wanted missing block to be already
requested and a bounded tail (at most `endgame_blocks`); it adds at most one
busy-block duplicate per `pick`, never to the same peer, subject to the same
count/byte path plus replica/global-duplicate caps. Priority precedes replica
count and seeded diversity. The first retained block is a provisional winner:
`receive` cancels its siblings but does not mark availability. A failed aggregate
piece check discards that attempt and permits retry. Only `finish(token,true)`
marks a fully received piece verified. A duplicated or stale result is ignored;
discard invalidates generations and retracts even previously verified local
availability. Failed mixed-source or unknown-resume-provenance pieces increment
only unattributed integrity failures; one peer is blamed only when all blocks
are known to come from that peer. No ban, payment or product policy is implied.

New peers start remotely choked; `set_choked` controls downloading FROM them,
not the local serving slots selected by `choker`. Changing availability or
priority affects future picks, not existing owned requests. `cancel`, `discard`,
`stop`, choke/disconnect results and `expire` make cancellation explicit. The
runtime calls `expire(now)` to release elapsed requests proactively; no hidden
timer exists. An arrival exactly at its deadline times out even if `expire`
has not run. Arrival/cancel order otherwise determines the winner. Accepted time
must be nonnegative and monotonic (equal times allow deterministic event races).
Rejected input and failed allocation do not advance time or leak issued work;
failed batches may burn IDs but roll back all their active requests/reservations.
Counters saturate rather than overflow. Deadline overflow is a typed error.

`progress()` reuses checkpoint bitmaps and geometry but claims no durability,
publication or generation advancement. Constructor resume input must come from
trusted storage after revalidation, never directly from a remote peer. It has
no known contributor provenance. `stop()` rejects future work and verification
callbacks, releases requests and empty reservations, but preserves received
block evidence and its partial reservations for a subsequent checkpoint.
The runtime still owns write draining, verification, data sync, durable
checkpoint sequencing and publication; a Core bit is not proof of any of them.

## Serving Slots

`choker{config, seed}.select(candidates, now, seeding)` takes a complete inventory
of unique nonzero connection IDs and returns the complete `unchoked`/`choked`
sets, not a delta. Interested peers alone compete; absent peers are immediately
pruned. Callers own actual choke messages, serving and upload accounting.
Each candidate supplies cumulative `peer_observation` and successfully served
payload bytes; counters must not regress on the same live connection.

The caller schedules selection rounds with strictly increasing nonnegative
times. There is no implicit round timer or fixed regular-round interval.
Useful throughput is the difference in verified bytes since the preceding
round; all candidates share that round duration, so comparing byte deltas
compares rates without overflow-prone multiplication. Latency uses mean response
milliseconds from round deltas. Integrity and timeout deltas are tie inputs,
not admission gates. A seed instead ranks current unchoked upload throughput,
deprioritizes peers only after both served-byte quota and minimum unchoke time
are met, then favors peers waiting longest. Download reciprocity is not required
for serving or authorization. This is not the donor's anti-leech/economic mode.

Total `slots` includes the optional single optimistic slot. Its selected peer
is retained until `optimistic_interval` unless no longer eligible or promoted
to a regular slot; rotation prefers unexplored then least-recently explored
peers, with explicit seeded ties. Defaults are 30 seconds optimistic rotation,
60 seconds minimum seed rotation and 1 MiB seed quota. Regular selections occur
on every caller round. Zero slots choke everyone. State and temporary round
views are bounded by 4096 candidates, independently of Transfer's limits.
Failed selection leaves prior samples, serving state and time unchanged.

Simulation coverage lives in `tests/swarm_core/transfer_tests.cpp`: seeded
selection, pressure, priorities, ownership, event orderings, endgame, retries,
retained unverified bounds, checkpoint evidence, malformed input and long-run
peer churn. Separate allocation and donor oracles are acceptance gates; source
implementation and these tests do not establish a working network runtime.

## Frozen V1 Raw Layout

The native wire is Forge Raw on little-endian, 8-bit-byte hosts. Other host
endianness is deliberately rejected at compilation rather than silently
producing a different identity. No C++ object padding is serialized.
Boost.Describe lists fields at the bottom of each owning interface.

| Value | Ordered fields and exact representation |
| --- | --- |
| `descriptor` | `version`, `piece_size`, `files` |
| `version` | `descriptor_version::v1 = 1`; **4-byte uint32**, explicitly shared `enum_wire_type`, not the native described-enum int64 default |
| `piece_size` | 4-byte uint32, bytes per piece |
| `files` | Minimal Raw unsigned uint32 varint count, then `file_entry` values |
| `file_entry` | `path`, `size`, `root`, `pieces` |
| `path` | Minimal varint component count; each component is minimal varint byte length then UTF-8 bytes |
| `size` | 8-byte uint64, exact file length |
| `root` | Exactly 32 digest bytes in SHA-256 output order, no length prefix |
| `pieces` | Minimal varint hash count followed by 32-byte hashes in increasing file-local piece order |
| `swarm_id` | `value`: exactly 32 digest bytes |
| `piece_proof` | `siblings`: minimal varint count followed by 32-byte hashes, bottom-up; orientation is implied by the index |

No optional layout, discriminator, display name, local root path, padding file,
policy, signature or origin is hidden in these records. `file_hashes{root,pieces}`
is a construction result, not a second descriptor layout.

`swarm_id = SHA256(Raw(descriptor))` after full canonical validation. Hashing
streams Raw directly into the existing SHA-256 encoder, not via a copied
metadata buffer. Immutable logical file paths and exact lengths are part of
the identity; local path mappings and presentation/policy are not. A bare file
root is **not** a unique file identity: padding can make different lengths share
a root, so file identity must also bind length.

### Independent Limits

All limits are hard v1 limits, not host-selected trust policy. Length sums use
checked subtraction before addition, including aggregate content, paths and
piece/hash counts. Files consume pieces independently; two short files do not
share one piece.

| Resource | Bound |
| --- | --- |
| Encoded metadata | 16 MiB |
| Files | 1 through 4096, including empty files |
| Path depth | 1 through 32 components |
| Component length | 1 through 255 bytes |
| Path length | 4096 bytes including conceptual `/` separators |
| Aggregate path length | 8 MiB, using the same separator accounting |
| File length | 1 TiB |
| Aggregate file length | 16 TiB |
| Piece size | Power of two, 16 KiB through 16 MiB |
| Pieces per file and across descriptor | 262144 (empty files consume zero) |
| Stored piece hashes across descriptor | 262144; short files store no layer |
| Proof siblings | Exactly `ceil(log2(piece_count))`, at most 18 |

Limits apply together; for example the piece-count cap may require a larger
piece size before a file reaches the file-length cap.

`decode_descriptor(span)` first rejects oversized frames, then makes a
nonallocating descriptor-specific structural pass using existing Raw scalar
and varint helpers. Each file/path/hash count and its minimum required bytes
are checked against the actual remaining frame before any vector/string
resize. This pass derives an exact cumulative Raw element budget; strings also
receive the 255-byte allocation cap. Generic reflected `unpack_exact` then owns
the actual decoding. Every varint is checked for minimality during preflight;
all other wire fields are fixed-width scalars, digest bytes or verbatim UTF-8
bytes. Version membership, exact frame consumption, semantic validation and a
Raw size check complete canonicality without a second encoder or buffer.
Nonminimal count varints and trailing bytes are rejected, not repaired.
Callers must keep the input stable for the duration of decoding.

`encode_descriptor` validates and counts through Raw before allocating the
exact bounded output. Calling generic `raw::unpack<descriptor>` or hashing
arbitrary Raw bytes bypasses this boundary and is not safe for untrusted
metadata. Likewise generic Raw decoding of `piece_proof` is not a bounded peer
protocol: its future framing owner must impose the 18-sibling cap before
allocation. The verifier itself checks the supplied span/vector length first.

## Paths

Components are valid UTF-8, checked through `forge.core.utf8` without making a
copy. Empty names, `.` and `..`, `/`, `\\`, NUL and C0/DEL controls are
rejected. Cyrillic and other valid non-ASCII filenames are supported. There is
no sanitization, case folding, Unicode normalization or repair.

Files must already be strictly sorted lexicographically by **components** and
unsigned component bytes, case-sensitive, with a shorter prefix first. Thus
`["a", "x"]` precedes `["a-"]`; joined slash-string sorting is incorrect.
Exact duplicate paths and file/directory prefix collisions are rejected.
Byte-distinct case variants and Unicode-normalization variants remain distinct
logical paths and distinct identities.

Platform adapters must still perform safe root-relative file operations,
prevent symlink/reparse-point escapes and reject target-specific aliases or
unrepresentable names. Case/Unicode equivalence, reserved device names,
drive-name semantics and trailing dot/space rules belong to those adapters,
not this immutable logical namespace. Descriptor validation cannot establish
filesystem containment or protect a later open from races. Path values may be
private; errors do not include raw path data and callers must not log it blindly.

## Merkle Contract

The algorithm follows [BEP 52](https://www.bittorrent.org/beps/bep_0052.html)
and libtorrent's `merkle_pad`/piece-layer pattern at donor commit
`b4e9e2471e7abce2a58585a7d7fbc4e8d1edc47e`, not torrent serialization.
Inspected donor files: `src/merkle.cpp`, `src/merkle_tree.cpp`. Donors are not
build dependencies; no donor types, session, disk worker or networking code
are copied into Core.

- Base leaves hash actual consecutive 16 KiB blocks. The final short block is
  hashed at its actual byte length, without byte padding.
- An absent block leaf is `Z0 = 32 zero bytes`, **not** SHA256(empty) or the hash
  of 16 KiB zero bytes. `Z(j+1) = SHA256(Zj || Zj)`.
- Every nonempty file has its natural complete binary tree, padded to
  `bit_ceil(ceil(size / block_size))` leaves, independently of other files.
- Empty file: size zero, zero root sentinel, empty piece layer and no pieces.
  An all-empty descriptor with at least one named file is valid.
- A file of at most one piece has an empty layer and its natural block-tree
  root, never an unnecessarily piece-sized tree.
- Larger files store exactly `ceil(size / piece_size)` piece hashes. The last
  piece is padded with absent block leaves up to `piece_size / block_size`.
- Reconstructing a file root from this layer pads absent **pieces** with
  `Z(log2(piece_size / block_size))`, not `Z0`. Pair ordering is always left/right.
- A proof has exactly the geometry-derived number of siblings. Wholly absent
  sibling subtrees must equal the correct `Zj`, even if a forged root would
  otherwise make the supplied path self-consistent.

`root_from_piece_layer` accepts larger files and canonical empty files. It
rejects nonempty short files: their omitted layer cannot reconstruct their
root. `make_piece_proof` accepts an empty layer for a short file and returns an
empty proof. Empty files have no valid proof index. Proof generation uses
O(piece count) hashing and O(log(piece count)) stack/output; verification uses
O(log(piece count)) hash operations, plus hashing the bounded piece bytes when
using `verify_piece`. Partial subtrees inside an opaque sibling hash cannot be
inspected without more data; the trusted root binds them.

`file_hasher` accepts complete blocks in order, followed by the exact last
short block. It retains at most 11 frontier hashes plus the bounded piece
layer, never file bytes. A rejected append or incomplete finish leaves the
state usable. Successful finish is one-shot; later append/finish throws.
`hash_file(span,piece_size)` is a zero-copy convenience path over this same
hasher; the streaming API does not require a complete in-memory file.

## Usage

```cpp
#include <cstdint>
#include <span>

import forge.swarm.core.descriptor;
import forge.swarm.core.identity;

namespace core = forge::swarm::core;

core::descriptor describe(std::span<const std::uint8_t> content) {
   constexpr auto piece_size = 256U * 1024U;
   auto hashes = core::hash_file(content, piece_size);
   auto result = core::descriptor{
      .version = core::descriptor_version::v1,
      .piece_size = piece_size,
      .files = {{.path = {"archive.bin"}, .size = content.size(),
                 .root = hashes.root, .pieces = hashes.pieces}},
   };
   static_cast<void>(core::validate_descriptor(result));
   return result;
}

auto verify_metadata(std::span<const std::uint8_t> untrusted_metadata) {
   auto descriptor = core::decode_descriptor(untrusted_metadata);
   return core::calculate_swarm_id(descriptor);
}
```

For a streaming file store, construct `file_hasher{known_size,piece_size}`,
read successive ranges of `min(block_size,remaining)` into a reusable caller
buffer, call `append_block(span)`, then `finish()`. Core owns no I/O or worker;
products must keep this synchronous hashing work off latency-sensitive I/O
executors using their existing bounded compute boundary.

## Errors And Acceptance

Malformed/truncated/nonminimal/trailing Raw input becomes
`exceptions::invalid_encoding`. Unsupported versions, unsafe paths, resource
limits, descriptor inconsistencies, invalid geometry and incomplete content
have separate typed exception aliases in the owning exceptions module.
Semantic errors discovered while decoding retain those semantic types;
preflight framing/count inconsistencies are encoding errors. A descriptor
with no files is `invalid_descriptor`; an oversized component is
`limit_exceeded`. Allocation/backend crypto failures are not swallowed.
Proof verification returns `false` for malformed geometry, wrong lengths,
wrong padding, wrong roots or changed content; backend crypto failures can
still throw. Hash-only verification assumes the caller hashed the correct
piece bytes; prefer `verify_piece` at an untrusted byte boundary.

Expected acceptance is owned by the repository's `tests/swarm_core` and
installed-component consumer: frozen Raw/id bytes, independent donor vectors,
all empty/short/non-power-of-two cases, malformed proofs, failed-append safety,
overflow and independent limit checks, path ordering/collisions and malicious
short frames with huge counts. Those gates are separate from implementing
this leaf. No builds or acceptance runs are implied by this README.
