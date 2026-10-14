# Nonblocking Log Delivery

Status: implementation complete; focused acceptance recorded below. Merge
requires clean independent reviews of the exact delivery head.

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

## Focused Acceptance (2026-10-14)

Native macOS arm64, LLVM 22.1.8, CMake 3.31.6/Ninja, OpenSSL 3.6.4.
Builds use `-j4`; no full CI, Linux or sanitizer result is claimed here.

- Log, static teardown, configured delivery, private configuration lifecycle,
  OTLP, HTTP observations and structure suites passed.
- App and the OTLP plugin passed. Asio passed 20 consecutive full-suite runs
  after replacing two pre-existing submit-versus-start test races with explicit
  callback-entry/release barriers; Asio production code did not change.
- Log, configured delivery and configuration lifecycle each passed 20 repeats.
- Installed generic, log-only and OTLP-plugin package consumers passed. The
  log-only consumer requests only `log`, disables OpenSSL discovery, and awaits
  the public delivery contract on a Boost.Asio executor.
- A real unread stderr pipe did not prevent HTTP responses. Releasing that pipe
  allowed explicit drain; failed output was counted separately from delivery.
- Deterministic private-owner barriers prove shutdown waits for admitted
  preparation and rejects publication after stop. The FIFO fixture adds real
  blocked-open coverage without claiming scheduler ordering as its proof.
- `libraries/log/include/forge/log/macros.hpp` is byte-identical to the base.
  `git diff --check` passed.

Acceptance logs are retained under
`.cache/acceptance/log-nonblocking-20261014/`; disposable build outputs are not
part of the evidence. Review fixes cover configuration/stop atomicity and
exception-safe retired-owner pruning. Explicit shutdown is still required for
drain; destructor fallback cannot terminate a non-cooperative callback and may
discard pending work. Direct synchronous sinks and OTLP retain their own owners.
