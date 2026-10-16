# forge_db_core

`forge_db_core` is the shared low-level record driver layer. It owns the public
record-oriented contract used by higher-level stores such as DB Object and DB
Blob.

## Stability

The savepoint, participant and record-lock source APIs are **Preview** in Forge
8.x. They may receive documented source-incompatible refinements in a MINOR
release. This status does not relax backend data or transaction correctness.

## Scope

- `forge::db::core::record_key`, `record_range`, `record_entry`, `record_page`,
  `cursor`, `page_request` and `family`.
- `forge::db::core::session`: backend-owned async record session.
- `forge::db::core::driver`: opens write transactions and read snapshots and
  exposes an optional durable checkpoint boundary.
- `forge::db::core::transaction`: move-only commit/rollback boundary with
  savepoints, optional record locks and participant hooks.
- `forge::db::core::snapshot`: stable read-only view.

`forge_db_core` does not know about objects, blobs, RocksDB, plugins, paths, WAL
policy or product schemas.

## Driver Contract

A backend implements `forge::db::core::driver` by opening sessions with honest
capabilities:

- `snapshot_reads=false, writes=true`: write transaction session.
- `snapshot_reads=true, writes=false`: read-only stable snapshot.
- `snapshot_reads=true, writes=true`: universal session.
- `snapshot_reads=false, writes=false`: invalid and rejected.

`transaction` owns commit/rollback and participant cleanup hooks. Higher-level
libraries can join the same transaction and share one backend commit boundary.

Drivers also expose an awaited, fail-fast `async_close()` boundary. Its first
call permanently rejects new transactions and snapshots. If sessions are still
opening or alive, close reports typed `driver_busy` without invalidating them;
the owner releases those handles and retries. A successful close is idempotent,
and every new open after close has started reports typed `driver_closed`.
Backend destructors remain no-throw fallbacks, not substitutes for this awaited
ordering boundary.

Backend-specific public operations that do not pass through `begin_transaction()`
or `begin_read()`, such as a driver's flush operation, must hold the protected
`driver::admit_operation()` result until backend access finishes. Once a close
request starts, new admissions report `driver_closed`; an admitted operation makes
close report `driver_busy` until its admission is released.

`create_checkpoint(path)` asks the backend for a self-contained durable copy at
one committed point. Unsupported drivers report typed `unsupported_operation`;
Core does not emulate a checkpoint by scanning records or expose backend files.

## Snapshot Ownership

Snapshots opened by `driver::begin_read()` carry the identity of that driver.
Higher-level stores use `snapshot::belongs_to(driver)` to reject accidental
joins across physical databases. The compatibility constructor taking a raw
`session` remains available, but creates an origin-less snapshot that cannot be
joined to DB Object or DB Blob.

Copies share ownership of one backend snapshot. The backend session and native
snapshot remain alive until the last copy is destroyed. Snapshot-capable
sessions must therefore support concurrent read operations through copies of
the same Core snapshot; write sessions receive no new concurrency requirement.

Driver-opened write transactions also expose `transaction::belongs_to(driver)`.
Moves preserve that origin; manually wrapped sessions have unknown origin and
return false. Origin is identity only: callers must separately check `active()`.
Existing normal joins are unchanged. Fresh provisioning APIs may require a known
origin before attaching participants or writing records. This additive source API
and private transaction state require consumers to rebuild; no record format changes.

## Savepoints And Participants

Savepoints are transient LIFO boundaries inside one active transaction:

```cpp
auto tx = co_await driver->begin_transaction();

co_await tx.put(records, key_a, value_a);
const auto point = co_await tx.create_savepoint();
co_await tx.put(records, key_b, value_b);

co_await tx.rollback_to_savepoint(point);
co_await tx.commit();
```

Only the top savepoint can be rolled back or released, and either operation
consumes it. Rolling back a savepoint removes its suffix of changes while the
outer transaction remains active. Releasing it keeps the changes and removes
only the boundary. Outer commit and rollback close every remaining frame.

Backends advertise savepoint and record-lock support through `capabilities`.
Unsupported operations, stale or non-top IDs, ID overflow and operations on a
rollback-only transaction fail with typed DB Core exceptions.

Higher-level DB libraries attach `transaction_participant` implementations to
keep their in-memory state aligned with native savepoints and final
commit/rollback. Participants are retained by the Core transaction, prepare
before commit and classify record mutations as reversible, excluded or
forbidden. `forbidden` always rejects the mutation, including for a standalone
non-capturing participant. `forbidden_when_captured` rejects only while at
least one participant captures mutations, allowing storage layers to protect
operations that cannot be represented in a captured history. This contract is
implementer-facing; savepoints themselves do not create durable revisions.
Higher-level operation guards can use `transaction::captures_mutations()`
without depending on a participant name or concrete revision implementation.

`transaction::before_commit()` registers an awaited one-shot projection hook.
Hooks run while the Core transaction is still active, after the caller has
finished its savepoint work and before participant prepare begins. They may use
the normal transaction read/write API, which lets a higher-level participant
materialize derived records inside the same physical commit. Hooks may not
commit, roll back, attach another participant or register another pre-commit
hook reentrantly. A hook failure makes the transaction rollback-only; a backend
commit failure after successful prepare still permits an explicit rollback or
a retry of the prepared backend commit.

Before-, after-commit and after-rollback hooks must all be registered while the
transaction is active and before the before-commit phase starts. Registration
on a default, closed or already-finalizing transaction is rejected rather than
silently retaining a callback that can no longer run.

Participants that own a physical record layout declare their
`exclusive_families()`. Core rejects overlapping claims before the first
mutation or savepoint, while observer participants can keep the default empty
claim and coexist with storage layers.

Participants that require transaction-wide record-lock ordering declare
`prewrite_locks()`. Before the first `get_for_update()`, mutation or native
savepoint, Core collects every claim, sorts and deduplicates them by
`(family, key)`, and acquires them in that canonical order. A participant with
prewrite claims may not be attached after this preparation boundary; a
claimless observer or storage layer may still attach until the first mutation
or savepoint because it cannot change lock order. Claims must remain stable
for the participant's lifetime. A partial preparation failure makes the
transaction rollback-only; prepared locks remain owned by the backend
transaction through savepoints until final commit, rollback or dropped cleanup.

## Families

`forge::db::core::family` is a logical record space inside one driver. RocksDB maps it
to column families. In-memory drivers can map it to separate ordered maps.
