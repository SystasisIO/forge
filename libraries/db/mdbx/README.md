# forge_db_mdbx

`forge_db_mdbx` implements `forge::db::core::driver` over the vendored
libmdbx `v0.14.2` release. It is intended for ordered state stores with one
serialized writer, many concurrent snapshot readers and strict transactional
savepoint requirements.

## Package

- Target: `forge_db_mdbx`
- Package component: `db_mdbx`
- Modules: `forge.db.mdbx.driver`, `forge.db.mdbx.exceptions`,
  `forge.db.mdbx.diagnostics`
- Dependencies: `forge_asio`, `forge_db_core`, `forge_exceptions`; libmdbx is
  private and vendored
- Build option: `FORGE_ENABLE_MDBX=ON`

## Logging Diagnostics

The private `forge_log` dependency uses `forge.db.mdbx`. INFO covers successful
environment open/explicit close and checkpoint creation. Native errors and
DEBUG transaction outcome counts are buffered, not emitted by CRUD, savepoint,
commit/rollback or low-level error helpers. Normal contention/not-found results
are DEBUG. No payload, key, path or native message text is sent to Forge sinks.
The DEBUG `environment.flush_request_completed` event means the flush request
returned normally, not that a non-blocking poll made data durable. In particular,
`async_flush(false)` may return normally when MDBX reports `MDBX_BUSY`; the
event's `sync` field identifies the requested mode without claiming a flush.

```cpp
import forge.db.mdbx.diagnostics;

forge::db::mdbx::diagnostics::claim_process();
// Outside native transactions, owner mutexes, writer gates and affine callbacks:
forge::db::mdbx::diagnostics::drain();
```

`claim_process()` permanently claims libmdbx's process-global debug callback;
the first Forge environment open also claims it once. It preserves the native
log level and debug flags. `installed()` reports that this claim occurred, not
that an unrelated caller has refrained from replacing it. Embedders must agree
on one owner and must not replace or restore callbacks while Forge MDBX is in
use. The native API cannot return a previous callback, so no speculative
restoration or per-store callback switching is performed.

Callbacks only record fixed atomic severity counters/source line numbers,
without formatting vendor arguments or attributing a global callback to a
store. Delivery retains native WARN/ERROR/FATAL levels and uses `scope=process`.
Automatic drains run after open or after explicit flush/checkpoint/close has
returned from the affine lane and released the writer gate. Idle/destructor-only
backends may retain counters until a later safe drain; no timer or worker is
introduced. Typed error counters retain 32 distinct native codes per process;
excess codes remain visible through an overflow count and latest code.

Fatal callbacks additionally attempt one fixed, sanitized stderr notice through
a system write, preserving `errno` and invoking no Forge sink. This is the sole
emergency-output exception because native abort may prevent a later drain. The
notice is best effort; no panic hook, termination or vendor assertion path is
changed. In particular, a system assertion can still print its own fatal text;
the debug callback cannot suppress that separate native panic path.

## Usage

```cpp
import forge.asio.affine;
import forge.db.mdbx.driver;

auto driver = co_await forge::db::mdbx::driver::open(
   {
      .path = "./data/mdbx",
      .families = {"objectdb", "blobdb.data", "blobdb.refs"},
      .durability_mode = forge::db::mdbx::durability::durable_sync,
      .map = {.upper_size = 64ULL * 1024 * 1024 * 1024,
              .growth_step = 256ULL * 1024 * 1024},
   },
   {.max_pending_operations = 1024,
    .max_waiting_submissions = 1024,
    .thread_name = "db-mdbx"});

auto transaction = co_await driver->begin_transaction();
co_await transaction.put({"objectdb"}, key, value);
co_await transaction.commit();

co_await driver->async_close();
driver.reset();
```

The lane-options overload makes the driver create and retain a managed lane
through every native session. It is intended for runtime components such as DB
Store that own both resources. The executor overload remains available when the
application owns a longer-lived lane; in that form the lane owner must outlive
every driver and session.

Call `async_close()` and release active transactions/snapshots before an
explicit shutdown of a caller-owned lane. A busy close is fail-fast and does
not invalidate live sessions; the managed form keeps the environment and lane
alive until the last session is released.

## Execution Contract

- Environment open, write begin, CRUD, native savepoints, commit, rollback,
  flush and close run on the supplied affine executor.
- A FIFO `forge::asio::gate` admits one writer without blocking an ordinary
  Asio runtime worker on the MDBX writer mutex.
- Snapshot reads run through independent clones of one immutable anchor
  transaction. Clone creation is briefly serialized because libmdbx forbids
  simultaneous use of the anchor; reads and cursors themselves remain
  concurrent.
- Keys and values are copied at the Core boundary. Native handles and return
  codes are never exported.

## Durability

`durable_sync` is the default and acknowledges a commit only after MDBX's safe
durable commit path. `safe_nosync` keeps a valid older steady checkpoint but may
lose the unsteady committed tail after an operating-system crash. Use it only
when the application has a tested replay source.

`async_flush(true)` creates a forced steady checkpoint. `async_flush(false)`
performs a non-blocking sync poll. Both are serialized with the writer gate.

`create_checkpoint(path)` creates a compact, self-contained MDBX environment in
a new destination directory. It is serialized with the writer gate, never
overwrites an existing destination and removes a partial destination on failure.

## Geometry And Snapshots

Production deployments should configure an explicit upper map size and a
realistic growth step. Reaching the configured map bound, exhausting readers,
incompatible environment geometry and I/O failures are reported as typed
`forge::db::mdbx::exceptions`.

Long-lived snapshots retain old MVCC pages and can grow the environment. Keep
snapshots operation-scoped or bounded to a known batch; they are not a
daemon-lifetime cache.

## Boundaries

This library does not own Object, Blob, Revision, ranked-index or plugin
policy. Those layers consume the neutral DB Core driver. `plugins.db.store`
provides typed configured MDBX ownership, while programmatic `add_store()`
continues to accept a caller-owned MDBX driver without taking over its lane or
close lifecycle.

Ordinary DB Blob records are supported, but MDBX v1 does not provide a
large-payload storage policy comparable to RocksDB Blob files. Values cross the
DB Core boundary as owned byte vectors and consume MDBX map space directly.
Deployments with large or unbounded payloads should keep the payload layer on a
backend designed for that workload until a separate policy defines thresholds,
streaming or external-value ownership, snapshot retention, garbage collection,
Revision interaction and crash recovery. This is not a correctness blocker for
bounded Object, Revision and ordinary Blob records.

MDBX v1 also keeps backend diagnostics private. It reports operational failures
through typed exceptions, but does not expose a stable public status surface for
map usage, reader slots, oldest-reader lag, retired pages, affine-lane queues or
close progress. A future diagnostics API must separate backend-neutral health
from MDBX-specific counters, avoid exposing native handles and paths, and prove
that observation does not interfere with writer or snapshot lifetimes. Operators
must size geometry and bound snapshot lifetimes from configuration until that
surface is delivered.

## Verification

`test_forge_db_mdbx` covers Core CRUD and scan boundaries, FIFO writer
admission, cancellation, native savepoints, snapshot clones, typed limits,
geometry, close/reopen, Object ranked indexes, shared Object/Blob snapshots and
Revision revert/prune. Its process-crash helper terminates without driver/lane
cleanup and verifies every durable acknowledgement or a valid `safe_nosync`
committed prefix after reopen.

When RocksDB is available, `benchmark_forge_db_backends` provides an
informational side-by-side run for point reads, scans, commits, savepoints,
ranked queries and concurrent snapshots. It has no performance threshold and is
not registered as a correctness test.
