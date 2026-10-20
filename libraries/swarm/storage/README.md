# forge_swarm_storage

Preview native POSIX file storage for immutable Swarm content. Target
`forge_swarm_storage`, package component `swarm_storage`, module
`forge.swarm.storage.file_store`, namespace `forge::swarm::storage`.
Native macOS and Linux use ordinary files; Windows storage is not claimed.
Public dependencies are `forge_swarm_core`, `forge_asio` and `forge_exceptions`.
This leaf has no dependency on Net, API, App, plugins, a DB driver or payload
blobs. It uses Core's canonical descriptor/Merkle/checkpoint contracts.

## Composition

Supply **separate**, bounded, caller-owned `forge::asio::compute::executor`
instances for disk and CPU. The store creates no worker pool, event loop or
background task. The application owns executor lifetime through store drain.
All blocking filesystem work runs on the disk pool. Descriptor validation,
hashing and checkpoint validation/encoding run on the CPU pool.

```cpp
import forge.swarm.storage.file_store;

auto files = co_await forge::swarm::storage::file_store::async_open({
   .manifest = validated_descriptor,
   .destination = selected_destination,
   .disk = disk_pool.get_executor(),
   .cpu = cpu_pool.get_executor(),
});
auto writer = files->begin({.file = 0, .index = 0});
for (auto& block : incoming_piece_blocks) {
   co_await writer->async_write(block.offset, std::move(block.bytes));
}
co_await writer->async_commit();
// Repeat for all pieces, then publish the complete file set.
co_await files->async_publish();
writer.reset();
co_await files->async_close();
```

`create` refuses an existing destination, staging object or checkpoint.
`resume` loads selected metadata and rehashes actual pieces before announcing
availability; a crash-created staging object without metadata can be salvaged.
An unexplained existing final destination is a conflict, not an implicit seed.
`seed` verifies an existing complete file/directory in place without copying
payload. Parent metadata/lock storage must be writable even when payload is
opened read-only. Reopening missing/truncated/replaced files fails explicitly.
Corrupt published content cannot be silently repaired or overwritten.

The destination is a file for one descriptor entry and a directory for multiple
entries. Its parent must already exist. In single-file mode the caller-selected
filename is the output name; the descriptor's logical path still belongs to
identity. Multi-file mode preserves every component under one root.

## Writes And Availability

`begin(piece_id)` returns one exclusive writer. Different pieces can progress
concurrently. Each write owns its vector, is at most `max_io_size`, starts at a
16 KiB block boundary and ends at a block boundary or the exact file tail.
No unverified data is readable through `async_read`. Commit rereads and hashes
the complete piece; mismatch clears its unfinished bitmap and reports an
integrity error. Abandoned writers retain recovery hints but cannot apply late
results to a replacement generation. Discard never deletes product files.

Verified reads accept a bounded range within one piece. Because ordinary files
can be changed externally, the returned piece buffer is rehashed before a
range is served. A timestamp or an earlier different buffer is not proof.
This intentionally costs one complete piece read/hash, never whole-file
materialization. Damage invalidates its availability and produces a typed error.

Defaults: 16 unfinished pieces, 8 active I/O operations, 1 MiB write/range
request bound. Hard limits: 64 unfinished pieces, 32 active operations, 1 MiB
request and Core's 16 MiB piece size. Work exceeding admission returns
`storage_busy`; the higher-level bounded runtime decides retry/fairness.
At most `max_parallel_io * max_piece_size` piece buffers may be in flight,
plus bounded descriptor/checkpoint/index state. No per-file pool or unbounded
queue is allocated. Checkpoint/publication admission, including queued
maintenance requests, is also bounded by `max_parallel_io`. File descriptors are opened by bounded anchored traversals,
not retained for every file. `snapshot()` is consistent; `async_wait_change()`
uses Forge notification epochs, not polling, and reports `storage_closed` at
shutdown instead of returning the same terminal snapshot in a busy loop.

## Durability And Recovery

Standalone sidecars are hidden siblings keyed by content identity and a digest
of the destination basename. They contain Core's versioned, checksummed Raw
checkpoint, never payload. Supplying one `checkpoint_store` selects that
external authority instead; an existing sidecar is rejected rather than kept
as a second authoritative state. ObjectDB composition is provided by `catalog`.
Both persistence paths validate Core checkpoint invariants before saving. If
a published file is later corrupted, a failed read clears its availability;
checkpoint/close then report `invalid_checkpoint` without replacing the last
valid durable checkpoint or calling the external adapter with invalid state.
The exclusive checkpoint temporary uses one fixed slot per key, bounding crash
leftovers to one file. Retry validates its type, owner, permissions, device,
link count and size before removing it. Symlinks and hardlinks fail without
deletion. No directory-wide cache sweep is performed.

Normal writes may overlap. Checkpoint/publication reserve a short admission
barrier, await active work without a state mutex, then perform:

1. Sync payload files and nested directories, children before parents.
2. Persist the whole checkpoint atomically. Sidecar mode syncs an exclusive
   temporary, replaces its owned sidecar and syncs the parent. External mode
   syncs the staging parent **before** invoking the adapter's durable commit.
3. For final publication, rename the complete staging root with atomic
   no-overwrite semantics, then sync the destination parent and save the
   published checkpoint.

Single-file staging is a hidden sibling file; multi-file staging is one hidden
directory. Payload is sparse and written at its final offsets. There is no
copy/delete fallback, cross-device downgrade or individually exposed files.
Every required sync failure is an error. A failed directory sync after rename
does not mean rollback: the output may already be visible. Its actual location
is retained, and retry/reopen reconciles that state without deleting data.
Checkpoint generations are reserved before an attempt and never reused after
a failed or canceled commit. Live availability and acknowledged durable
checkpoint generation are distinct.

`request_stop()` closes admission; `async_close()` cancels/joins owned work,
waits explicit leases, flushes a final checkpoint and closes descriptors.
Release leases before awaiting close in the same flow. Close is an isolated,
awaited operation and does not change the caller's cancellation policy.
Cancellation requests cannot undo a completed syscall. Writes never publish
availability from a canceled result, and publication retains its real outcome.
Destruction requests stop and retains state until in-flight callbacks retire;
it is not a substitute for successful durable `async_close()`.

## Filesystem Trust Boundary

The caller chooses the destination. Operations pin its parent and traverse
untrusted names via `openat` with `O_NOFOLLOW`; type, device, exact size and
inode identities are checked. Symlinks, hardlinked payloads, aliasing logical
names and replaced objects fail closed. Directory/file creation uses exclusive
names. A process lock prevents cooperating stores from sharing one staging
location. Final no-overwrite rename is provided by Darwin `RENAME_EXCL` or
Linux `RENAME_NOREPLACE`; unsupported filesystems fail rather than emulate it
with a racy exists-check. No `EXDEV` copy fallback is present.

The parent and process credentials remain a trusted local administration
boundary. A hostile owner able to directly rewrite/reparent pinned inodes or
ignore advisory locks cannot be made into an atomic filesystem transaction by
path checks. This library is not a sandbox for a malicious local administrator.
Returned data is independently rehashed and restart metadata is never trusted
as proof of payload integrity. No raw path/content is included in error text.

## Object Store Catalog

`import forge.swarm.storage.catalog;` provides the indexed metadata adapter.
The `swarm_db` component owns its declarative models, not a live database.
Composition opens a named Object Store namespace, calls
`catalog::register_objects(objects)` before starting operations, then awaits
`catalog::async_open(objects, options)`. The Object Store must use single-writer
transactions and monotonic IDs. Schema mismatch reports `schema_mismatch`; no
automatic namespace deletion or implicit schema migration occurs.

`options.cpu` is the existing caller-owned compute executor. `options.database`
is a caller-owned Asio executor suitable for the selected DB driver. In
particular RocksDB commit/flush are synchronous inside awaitables: use an
executor isolated from the application's network/event-loop threads. The catalog
creates no pool and does not open or close the DB. Await all operations and drop
consumers before the owner closes executors or the driver.

```cpp
catalog::register_objects(objects);
auto metadata = co_await catalog::async_open(objects,
   {.cpu = cpu_executor, .database = database_executor});
auto entry = co_await metadata->async_create(manifest, normalized_local_path,
   {.state = forge::swarm::db::job_state::downloading});
auto resume = co_await metadata->async_checkpoint_store(entry.swarm, entry.locator);
auto files = co_await file_store::async_open(
   {.manifest = entry.manifest, .destination = entry.locator,
    .disk = disk_executor, .cpu = cpu_executor, .checkpoint = resume});
// Write/verify content, then checkpoint/publish and await files->async_close().
```

The locator is a bounded local opaque string, never a network path claim. Its
normalization and filesystem aliases belong to composition; file ownership and
safe traversal remain enforced by `file_store`. Job state is scheduling metadata,
not proof of verified payload. `async_update()` changes only job settings;
checkpoint saves cannot clobber a concurrent priority/expiry update. Erase removes
metadata only. An old binding cannot recreate erased/replaced content.

Default admission is eight outstanding operations (maximum 32), including work
waiting for the DB executor. Queries default to 16 rows and reject limits above
32; use the Object Store cursor in the next bounded query. These are live
per-page reads, not a catalog-wide snapshot across concurrent updates. Identity,
state/descending-priority and exclusive-expiry queries use Object Store indexes,
without enumerating unrelated history. `INT64_MAX` expiry means no expiry.

The v1 row and all its secondary indexes are updated atomically. A checkpoint ACK
requires commit followed by `objects.driver()->async_flush(true)`. A failure after
commit may leave the row visible; it is not a rollback guarantee. An identical
generation retry repeats flush; older or conflicting equal-generation values fail
with `stale_checkpoint`. Checkpoint load validates and restores the durability
fence before returning restart state. File bytes are still independently checked
by PR2, even when a row says the download was complete.

The DB is locally administered: generic Object Store Raw decoding precedes domain
validation. Post-decode descriptor/checkpoint bounds are not a preallocation
sandbox for malicious DB files. No remote input is persisted by implicit import.

Cancellation is joined, not a promise to interrupt an already running driver
syscall. Native Asio cancellation remains connected to cancellable dependency
waits; explicit stop tokens are checked at operation boundaries and CPU work.
Once commit starts, its commit/flush section is awaited without cancellation;
the caller then observes cancellation without a false success or abandoned
durability operation. Public calls reserve admission before DB dispatch and
may synchronously reject overload. Destroying an unstarted awaitable releases
its reservation; a started operation or checkpoint handle retains its own state
independently of the original catalog facade.

## Evidence

See [donor mapping](../../../docs/donors/forge-swarm-storage-v1.md) and focused
tests in `tests/swarm_storage`: real file output, partial resume, checksum and
payload corruption, source replacement, atomic conflicts, ownership,
cancellation and syscall failure/crash scenarios. Test executable interposition
changes no production syscall implementation. Process-crash tests do not claim
to simulate a physical disk losing acknowledged writes or full production soak.
The injected `EXDEV` and exact no-overwrite rename-race cases are registered
only on Darwin, whose libc rename entry point can be interposed. Linux uses
raw `renameat2`; those two injected cases are not proven there and are not
registered as passing substitutes. Native Linux acceptance remains separate.

```sh
cmake --build build/pr2 -j 4 --target \
  test_forge_swarm_core test_forge_swarm_storage test_forge_asio
ctest --test-dir build/pr2 --output-on-failure \
  -R '^test_forge_(swarm_core|swarm_storage|asio|structure|swarm_structure)$' \
  --timeout 180
cmake --build build/pr2 -j 4 --target test_forge_package_swarm_storage_component
git diff --check
```
