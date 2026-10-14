# Nonblocking Log Delivery

Status: implementation and acceptance in progress.

## Contract

The public `tlog/dlog/ilog/wlog/elog` macro interface is unchanged, including
lazy argument evaluation, filtering, fields, redaction and callsite metadata.
Record preparation remains synchronous; nonblocking means no downstream I/O
or waiting for queue space in the producer, not lock-free or hard real-time.

`forge.log.async_sink` adapts a synchronous sink onto a dedicated, externally
owned Forge Asio compute executor. It owns a bounded FIFO and one coalesced
drain operation. Budgets include the record being delivered. Overflow drops
new records and exposes counters; it never falls back to synchronous output.
Downstream exceptions and recursive logging must not escape or form a loop.

Configured console/file/JSONL sinks use the adapter by default. Direct sink
objects remain synchronous for explicit callers and tests. OTLP already owns
a bounded asynchronous export queue and is not wrapped automatically.

## Ownership

One private process-scoped configured-sink owner reuses Forge Asio compute;
HTTP, P2P and individual application shells do not create logging workers.
Retired configurations have a finite retention limit. Capacity must be checked
before constructing sinks that can open or truncate files. Removal and route
replacement never join a sink callback on the emitting thread.

Flush observes a fixed acceptance fence. Shutdown closes admission, waits for
all retained generations and only then shuts down workers. Caller cancellation
or a deadline does not cancel another caller's wait or claim that a running
callback was terminated. No detached threads, unbounded posts or forced thread
termination are allowed. The process owner performs explicit shutdown after
all producers finish. Destructor fallback joins workers and may wait for a
non-cooperative callback; this limitation cannot be hidden by a timeout result.

## Evidence

Use deterministic blocked/throwing/recursive sinks, concurrent producers,
overflow and byte bounds, metadata ownership, fixed flush fences, timeout and
subsequent completion, reconfiguration, static teardown and HTTP progress.
Retain the existing macro, OTLP and App regressions and installed consumers.
Only focused tests and structure gates; no full CI. Two independent reviews
must be clean on the exact delivery head before merge to `dev`.

Donor semantics: OpenTelemetry Logs SDK recommends that `OnEmit` not block the emitting
thread; its batching processor bounds the queue and drops records on overflow.
This implementation reuses Forge runtime ownership rather than importing an
additional logging runtime or network exporter.

Reference: https://opentelemetry.io/docs/specs/otel/logs/sdk/
