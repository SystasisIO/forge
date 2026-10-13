# DB And P2P Operational Logging

Status: implementation; no production or acceptance claim until final validation.
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
