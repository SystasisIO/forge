# Swarm Indexed Metadata Donor Mapping

Scope: PR3 metadata models and externally owned Object Store adapter. File
durability remains owned by [PR2](forge-swarm-storage-v1.md); neither DB rows nor
successful metadata reads prove payload integrity.

## Reviewed Sources

- Syncthing `1919c89de488d8e1088579f5b11f28aa375c5a8e`:
  `lib/model/folder_sendrecv.go` directory sync before index update;
  `internal/db/sqlite/folderdb_update.go` related metadata in one transaction.
- libtorrent `b4e9e2471e7abce2a58585a7d7fbc4e8d1edc47e`:
  `src/torrent.cpp` unfinished piece state, verification and resume saving;
  `src/storage_utils.cpp` seed checks. Presence/size-only confidence and a
  resume-save notification are not accepted as Forge durability proof.
- Forge `db.object` model/index DSL, transaction/snapshot and paging APIs;
  DB Core commit/after-commit participant semantics; MDBX affine execution and
  flush writer gate; RocksDB driver commit/flush; Asio compute and cancellation.
- Spine-style DB/domain boundary: model declarations and index descriptors do
  not own a live store, registration side effect, network DTO or runtime loop.

## Decisions

One content object atomically carries canonical identity/descriptor, a local
locator, job settings and optional bounded checkpoint. No object per received
block, payload-in-DB storage, hand-written physical key or second index engine.
Identity and locator are unique. State/descending-priority and expiry queries
use ordered composite indexes with bounded cursor pages, not catalog scans.
The locator index derives a fixed SHA-256 key with the existing Object Store
`const_mem_fun` extractor and Crypto digest. A raw long string exceeded MDBX's
4-KiB-page key limit in a retained failing regression. The full locator remains
in the row and binding; digest collisions reject admission rather than confusing
paths. No custom physical-key encoding or second persisted digest is added.

File store verifies and syncs actual bytes and directory entries first. Adapter
transaction checks immutable binding identity, object ID and locator, changes
only the current row's checkpoint, commits, then flushes that same store's
driver with `true`. An ACK follows the durable flush, not only commit.

An error after commit may leave the new row visible. It is not a rollback
guarantee. An identical-generation retry repeats the flush; a different value
at the same generation or an older generation is rejected. Loading a stored
checkpoint restores the flush fence before exposing it as restart state.
Erase/recreate gets another monotonic Object ID, so an old binding cannot
recreate or overwrite the replacement.

Models are explicitly registered by composition. Schema v1 mismatch is typed
and has no automatic destructive reset. The adapter neither opens nor closes
the externally owned DB. CPU work and DB work receive caller-owned executors;
RocksDB's awaitable signature does not imply offload, so its blocking work is
explicitly dispatched away from the caller's network/event-loop executor.

## Evidence Boundary

Shared behavioral fixtures cover memory and native MDBX/RocksDB. The memory
driver is a test fault model, not a durable backend or an alternative Swarm
storage implementation. Faults distinguish write/commit rollback from failed
post-commit durability; actual files are reconciled and rehashed on restart.
Focused results and exact-head reviews are recorded in the external acceptance
receipt, not inferred from this design note.

Native MDBX/RocksDB process-crash fixtures execute a separate helper process
and require exit 77 from an Object Store after-commit observer. The helper exits
before catalog flush, once for a staged checkpoint and once after published
metadata commit. Reopen verifies metadata, real payload and final publication.
Missing the exact boundary fails the test; no fork of an active threaded runtime
or substitution with the memory fault model is used.

Object Store performs its generic Raw decode before domain validation. Domain
size checks are not claimed as a preallocation sandbox for hostile DB files.
Local DB administrators and storage locator normalization remain composition
boundaries. No network interoperability, production soak or physical power-loss
proof is implied by this PR.
