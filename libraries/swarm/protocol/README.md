# forge_swarm_protocol

Bounded immutable-metadata exchange and peer-message values for Swarm. Use this
leaf to validate network items before passing them to Core mechanisms or an
external runtime. It owns no connection, authenticated peer, request ledger,
timer, executor, file, database, API service or product admission policy.

Public source API and this explicitly versioned native wire contract are
**Preview**. An incompatible wire change still requires an explicit version
and upgrade/reset decision; Preview does not permit silent reinterpretation.
These values are not BitTorrent or IPFS wire framing.

## Target And Modules

Target `forge_swarm_protocol`, installed `Forge::forge_swarm_protocol`, package
component `swarm_protocol`. Direct dependencies: `forge_swarm_core`, `forge_raw`,
`forge_reflect`, `forge_exceptions` and `Boost::headers`. No DB, storage, API,
Asio, App, plugin or concrete transport dependency is introduced.

| Module | Real Ownership |
| --- | --- |
| `forge.swarm.protocol.metadata` | Metadata/exchange requests, public metadata, chunks, realm validation and bounded chunk codec |
| `forge.swarm.protocol.metadata_reader` | Validated descriptor encoded once and emitted in ordered chunks |
| `forge.swarm.protocol.metadata_assembler` | Bounded ordered assembly and final exact canonical identity validation |
| `forge.swarm.protocol.peer_message` | Peer values, frozen variant, stateless/descriptor validation and bounded message codec |
| `forge.swarm.protocol.exceptions` | Typed protocol failures over Forge Exceptions |

There is no aggregate facade or terminal-error message. Terminal API failures
use the existing API error payload at the consuming API boundary. The `reject`
item is a request-level protocol outcome, not an alternative error envelope.

## Metadata Transfer

`metadata{swarm, manifest}` contains only immutable public metadata. No local
ObjectDB ID, locator, job state, priority, expiry or checkpoint is included.
Neither its identity nor a realm authorizes serving, establishes trusted peer
identity or proves local file availability. A descriptor may disclose logical
filenames: do not log it or the realm blindly.

```cpp
#include <utility>
import forge.swarm.protocol.metadata_reader;
import forge.swarm.protocol.metadata_assembler;

namespace protocol = forge::swarm::protocol;
namespace core = forge::swarm::core;

protocol::metadata exchange_in_memory(const protocol::metadata& metadata) {
   protocol::metadata_reader reader{metadata};
   protocol::metadata_assembler assembler{metadata.swarm};
   while (auto chunk = reader.next()) {
      const auto wire = protocol::encode_metadata_chunk(*chunk);
      assembler.append(protocol::decode_metadata_chunk(wire));
   }
   return std::move(assembler).finish();
}
```

The metadata stream consists of canonical Core **descriptor bytes**, not Raw
of the outer `metadata` record. The expected swarm identity is supplied to the
assembler from the request. `metadata_reader` validates the descriptor and
hashes its one encoded buffer against the supplied identity. `next()` returns
`optional<metadata_chunk>` and advances only after constructing that chunk.

Each `metadata_chunk{total,offset,data}` has `total` in 1..16777216, an aligned
16384-byte offset strictly below total, and exactly `min(16384,total-offset)`
data bytes. Only the last chunk may be short. `append` requires contiguous
ordered chunks and a stable total; duplicates, gaps and reordered chunks fail.
It reserves at most total bytes on the first append and copies each chunk once.
It does not decode, re-encode or rehash the accumulated prefix on every append.
`received_size()` reports byte progress; `complete()` reports framing completion,
not valid metadata or verified payload. `finish() &&` requires completeness,
decodes with Core's bounded canonical decoder, hashes the exact received bytes
and compares the expected identity. Successful finish is one-shot; an incomplete
finish leaves the assembler usable. A failed complete assembly must be discarded,
not published as partial metadata.

Reader and assembler each retain at most 16 MiB of encoded data. A returned
chunk owns at most 16 KiB. Finishing also owns a decoded descriptor, including
its strings/vectors and allocator overhead; these are not an exact total-memory
budget. Total encode/hash/decode work is linear in metadata plus Core's Merkle
and descriptor validation work. Callers must bound concurrent assemblies and
offload this synchronous work through their own bounded compute execution.
This leaf creates no hidden worker or asynchronous lifecycle.

## Peer V1 Wire

Raw serializes described fields in declaration order. Scalars use the native
Core-supported little-endian layout, with no C++ padding. `swarm_id`, SHA-256,
`piece_id` and piece proofs reuse Core's existing Raw layout. Strings and vectors
use minimal Raw unsigned uint32 varint counts. A variant is its minimal varint
index followed by its selected described value, with no extra framing.

| Index | Alternative | Ordered Fields |
| --- | --- | --- |
| 0 | `handshake` | uint16 version=1, swarm, realm, uint32 capabilities |
| 1 | `inventory` | vector<uint8> pieces |
| 2 | `have` | piece |
| 3 | `interest` | bool interested |
| 4 | `choke` | bool choked |
| 5 | `request` | uint64 id, piece, uint32 offset, uint32 length |
| 6 | `block` | uint64 id, piece, uint32 offset, vector<uint8> data |
| 7 | `cancel` | uint64 id |
| 8 | `reject` | uint64 id, uint8 rejection |
| 9 | `hash_request` | uint64 id, piece |
| 10 | `hash_response` | uint64 id, piece, hash, proof |

This alternative order is frozen for v1. `rejection` has shared Raw uint8 wire
width with explicit codes unavailable=1, choked=2, invalid_request=3, canceled=4.
Zero/unknown rejection codes and unknown alternatives are rejected. Handshake
versions other than 1 fail explicitly. Capabilities remain opaque uint32 facts:
they are not feature activation, trust, authorization or payment. Realms contain
0..128 printable ASCII bytes (0x20..0x7e). No peer identity is accepted from a DTO.

All request/cancel/reject/hash IDs are nonzero. Stateless `validate(message)`
checks ID, version, realm, inventory/proof caps and aligned block ranges within
the maximum 16-MiB piece geometry. Payloads contain 1..16384 bytes. It cannot
infer a short tail from a message alone and establishes no owned request.

`validate(message, manifest)` requires a descriptor already validated by Core
at admission. It reuses Core piece geometry to enforce exact complete blocks or
exact short tails. Inventories must contain exactly ceil(total_pieces/8) bytes,
little-bit-first in descriptor file order, with zero trailing bits. Hash responses
are checked against the selected file's expected root using Core verification,
including proof geometry/padding. A block is not locally verified merely because
a peer supplies it, an inventory bit or a hash response. The runtime must still
bind responses to live owned requests and perform actual retained-byte checking.

Geometry checks do not revalidate or hash the entire immutable descriptor on
every block. Piece lookup and inventory counts cost at most O(file count), while
proof/root checking costs O(log(piece count)). The handshake descriptor overload
also computes the canonical swarm identity once per call to bind that handshake.
Do not pass unvalidated mutable descriptors and mistake this geometry overload
for metadata admission. Session order, realm consistency across messages, late
responses, cancellation, quota and availability updates remain runtime/Core work.

## Bounded Codec Boundary

Use `encode_message` / `decode_message` for peer stream items and
`encode_metadata_chunk` / `decode_metadata_chunk` for metadata stream items.
Both decoders accept optional `forge::raw::unpack_limits`; they only tighten the
caller budgets, never replace tighter values with defaults. Peer container,
cumulative-container and first-container limits are clamped to 18 before Raw
allocates proof siblings. Byte fields are separate from element containers in
Raw: peer byte allocations are clamped to 32768 and metadata data to 16384,
also no larger than the input frame. Outer message frames are capped at 65536;
metadata chunk frames at 16395 (8 scalar bytes, at most 3 count bytes, 16384 data).
The exact allocation size and container overhead remain implementation-dependent.

The existing `raw::unpack_exact` owns primitive decoding and allocation checks.
After semantic validation, comparing a bounded Raw re-encode rejects nonminimal
counts/variant indices. Trailing bytes, bad bools, truncated/overflowing prefixes,
unknown alternatives and oversized allocations fail with typed protocol errors.
Canonical re-encoding temporarily owns another bounded item buffer, not a second
whole metadata buffer. No custom Raw primitives or transport framing are used.

Plain `raw::pack(DTO)` intentionally has the same bytes as the helper; plain
unbounded Raw decoding is **not** the untrusted network boundary. The consuming
transport must invoke these bounded helpers before exposing stream items, then
perform manifest/context/request-state validation. Protocol limits do not replace
parent API limits, caller authentication or process-wide concurrency admission.

Tests in `tests/swarm_protocol` cover hardcoded Raw fixtures for every alternative,
16-KiB boundaries and the valid 16-MiB metadata cap, forged prefixes, unknown and
noncanonical encodings, parent decode limits, bitmap tails, proof misbinding and
one-shot assembly. Tests/build/package acceptance are separate coordinator gates;
this leaf does not claim a working network runtime or interoperability protocol.
