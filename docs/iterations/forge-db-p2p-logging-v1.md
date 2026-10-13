# DB And P2P Operational Logging

Status: focused implementation acceptance passed; no whole-stack production claim.
Base: dev `03d4c43b`. Scope: operational logs, not protocol/storage changes.

## Ownership

Libraries emit structured events through existing Forge macros. `forge_log`
owns filtering, redaction and sink delivery. The optional `forge_otlp` library
and `plugins.log.otlp` own export; DB/P2P must not depend on them. Applications
choose routes, levels, destinations and lifecycle ordering. No extra polling
service, detached task, logging framework or network runtime is introduced.

The native node owns network state transitions. The node plugin logs only its
dependency/configuration/API lifecycle, not copies of native events. DB drivers
own engine errors; Object/Blob/Revision own their semantic operations; DB Store
owns configured named-store lifecycle. Failures are logged once where handled,
not at every rethrow. Programmatic library users retain the same instrumentation.

## Event Policy

| Domain | Ordinary INFO | Opt-in DEBUG/TRACE | Warning/error |
| --- | --- | --- | --- |
| P2P | ready, stopped, reachability/degradation changes | connection, Identify, discovery, maintenance, relay/DCUtR, mesh outcomes | persistent loss of service, resource pressure, internal failure |
| P2P plugin | configured service ready/stopped | dependency/API composition | invalid setup or failed lifecycle |
| DB driver/store | open/close and backend/durability | explicit flush and detailed lifecycle | I/O, corruption, failed flush/commit or capacity |
| Object/Blob/Revision | rare administrative lifecycle | transaction completion, revision changes, pruning | incompatible schema or integrity failure |

Constant routes use `forge.db.*`, `forge.net.p2p` with focused subroutes, and
`forge.plugins.net.p2p.node`. Structured `event` identifiers are stable. Fields
are limited to safe operation identity, counts, duration, state and error codes.
No payload, DB value/key bytes, PSK, private key, token, whole config or unchecked
exception/vendor text is emitted. Peer identity is not fabricated before
authentication. Remote addresses/topic identifiers are not routine INFO fields.

Success is emitted after the real transition. A committed transaction is not
called durable unless the selected backend mode/flush actually guarantees it.
Reads, writes, packets, heartbeats and routine failed probes do not create INFO
records. Ordinary cancellation, not-found and expected conflicts are not errors.
Repeated hostile/retry failures use bounded reporting or existing coalesced state
transitions; logging must not create an unbounded per-peer/cardinality map.

## Native Diagnostics

Disable RocksDB's independent diagnostic LOG/LOG.old output, never its WAL or
recovery files. Use supported native hooks, not vendor patches. Preserve useful
error/corruption/background status without raw content dumps. MDBX logging is
process-global: one coherent adapter owns its callback and does not invent a
per-store attribution. Assertions, integrity checks and fatal termination remain
unchanged. Native callbacks must not re-enter DB code or arbitrary sinks under
engine locks. Native diagnostic buffering, if necessary, must be bounded and
owned, with explicit drain/drop semantics and no new worker runtime.

MDBX fatal callbacks are the emergency exception: they attempt a fixed redacted
stderr write because abort may prevent a later drain. This write can block on a
full pipe; it is not a hard real-time guarantee. Independent system assertions
retain their native behavior. Ordinary callbacks only update fixed counters.

## Safety And Export

Macro route/message/field/delivery failures are best-effort and cannot change a
DB/network result. Emergency reporting is fixed-text, allocation-free and bounded.
Recursive delivery to an already active sink is suppressed. Direct logging APIs
retain ordinary eager evaluation. Synchronous console/file sinks remain
synchronous; log after releasing owner locks and never add network awaits.

OTLP is optional and logs-only. Collector downtime must not stop DB/P2P work;
queues/retries/shutdown remain bounded and dropped exports remain observable.
Early initialization and final shutdown events need explicit lifecycle proof;
attaching an exporter only during startup does not capture earlier DB open.
Local diagnostics remain available without export. Products may alternatively
ship JSONL/console through an external collector. Telemetry is not a durable
business/audit ledger and must not be stored in the DB it is diagnosing.

## Acceptance

- Capture actual DB/P2P transitions, check level/event/context and avoid duplicates.
- Prove disabled arguments remain lazy and logging exceptions do not escape.
- Prove sink recursion terminates and sink failure does not suppress other sinks.
- Open/reopen both DB backends without native diagnostic files; preserve commit,
  rollback, failure and durability behavior, including background diagnostics.
- Check redaction/no payload leakage and bounded repeated failure reporting.
- Exercise real macro records through OTLP with a local collector, early/late
  lifecycle, full queue, unavailable endpoint, cancellation and bounded shutdown.
- Run focused log/OTLP/DB/P2P/plugin/package tests, structure/inventory and diff
  checks. One coordinator build, -j4, no full CI. Independent exact-head reviews,
  fix loop, then reviewed merge dev and cleanup only task-owned inactive builds.

No version bump, release, Stage 8 or downstream product changes are in scope.

## Acceptance Receipt

Validated on 2026-10-13 against implementation head
`d400c4935802819f533d98e92e27ed44e9e89d93`. Production/CMake changes were frozen
at `39ca3d90455b57e5d49c631b1834ed096f662706`; subsequent implementation commits
only add missing explicit `<string>` includes to package consumers. This receipt
is a documentation-only follow-up. Base remains dev `03d4c43b`.

- Linux ARM64, Clang 22.1.8, Ninja, Debug `-O0 -g0`, assertions enabled, both
  RocksDB and MDBX enabled; coordinator-only builds with `-j4`.
- Final focused CTest run: 17/17 PASS. Targets: structure, log_shutdown, log,
  otlp, db_mdbx, db_object, db_blob, db_revision, db_rocksdb, p2p_lifecycle,
  p2p_pubsub_router, p2p_autorelay, p2p_path_management, p2p_inventory, plugins,
  plugins_db_store and rocksdb (all prefixed `test_forge_`).
- Separate external build-tree package consumers: 11/11 PASS. Components:
  db_mdbx, db_rocksdb, rocksdb, db_object, db_blob, db_revision, otlp, net_p2p,
  plugins_db_store, plugins_log_otlp and plugins_net_p2p_node. Each consumer was
  configured, compiled and executed; completed consumer builds were removed
  before continuing. Unchanged source closures reuse their earlier result in
  this sequential run, not a claim that every consumer was rebuilt per commit.
- macOS Homebrew LLVM: `contract-sdk-host` built `forge_db_object`; an external
  `db_object` package consumer compiled and ran. The SDK profile exports Log
  without acquiring App, OTLP or network dependencies.
- Real DB Store/OTLP application fixtures pass for both backend drivers with
  collector success and HTTP 503. Tests cover early open/final close events,
  no native RocksDB LOG files, redaction, commit/rollback/reopen, exporter
  rollback after initialization failure, crash-capture rollback, recursion,
  failing sinks and logging after main-thread TLS destruction.
- Huygens independently reviewed the full diff; Kuhn independently reviewed
  Log/OTLP, package/SDK changes and cross-layer boundaries. Both returned CLEAN
  at `d400c493`; coordinator owns test execution, not the reviewers.

Not run: full CI, a new Go/Rust interoperability matrix, production soak, full
install/relocation package matrix, or the omnibus `test_forge_quic_p2p` target.
The latter drags unrelated large chain/signer compilation; the existing focused
lifecycle, PubSub, AutoRelay and path-management suites were run instead. Linux
SDK configuration was blocked by the trimmed toolchain's missing
`libLLVMDemangle.a`; the complete native macOS SDK build passed.

Existing concurrency limits remain explicit: concurrent logger reconfiguration
and OTLP API calls overlapping explicit shutdown were not validated. Review
found pre-existing unsynchronized shared state in those paths at the base, not
a new concurrent path introduced by earlier exporter attachment. Normal ordered
application lifecycle is the tested contract here. Native diagnostic counters
can remain buffered until a safe drain, and synchronous local sinks remain
synchronous. Collector delivery is best-effort, not a durable audit ledger.
