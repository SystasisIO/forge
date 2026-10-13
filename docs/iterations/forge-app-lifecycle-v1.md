# App Lifecycle Ownership

## Scope

Maintainer-approved clean source break in Forge.App and official Forge plugins.
Downstream applications are not modified by this change. No release version,
network protocol, database layout or persisted product state changes.

## Ownership

- App owns common lifecycle transitions, operation serialization, stop admission,
  generation checks and read-only observation.
- Concrete plugins own resource publication, cancellation and joined cleanup.
  Resource-specific states, such as database model registration and open stores,
  are not interchangeable with the plugin lifecycle.
- A failed operation is recorded independently of the current phase. Failure does
  not prove that resources have been released.
- Stop invalidates pending startup publication. Checking a generation and installing
  a resource must use the same synchronization boundary.
- Shared services and typed APIs remain the application composition mechanism.

## Observation

Diagnostics reads the canonical lifecycle objects. It does not maintain a second
mutable copy of their states. Snapshot observation is asynchronous and may coalesce
changes; it is not a lossless audit stream.

Synchronous lifecycle signals and the unused generic event subscription queues
are removed. Process SIGINT/SIGTERM handling remains in the runner. Diagnostic
history remains a bounded journal, with both entry-count and record-size limits.
Lifecycle logs use Forge logging macros and the configured logging sinks; App
does not own an OTLP exporter.

## Application Logging

Application and plugin lifecycle boundaries use the existing Forge logging macros
and named loggers. Donor evidence comes from the BitShares-derived App layer and
Spring/EOSIO application runners, not a new logging runtime.

- INFO marks successful startup/readiness and completed shutdown; DEBUG carries
  preparation and intermediate lifecycle work.
- ERROR marks failed lifecycle operations or incomplete cleanup. Ordinary stop and
  cancellation are not reported as application failures.
- Records contain operation, lifecycle phase and stable error classification.
  Configuration values, keys, tokens, payloads and arbitrary exception text are
  excluded. Diagnostic history remains bounded and is not an alternative log sink.
- Logging is best effort and runs outside lifecycle locks. Logger or sink failures
  cannot change operation completion, resource release or shutdown ordering.
- Existing configured sinks, including OTLP, consume the same records. App neither
  constructs nor owns an exporter, and cannot guarantee export before logging has
  been configured or after its sink has stopped.

Capture-sink tests cover lifecycle boundaries, repeated shutdown, redaction and
throwing sinks in addition to the lifecycle correctness gates below.

## Host Context

Plugin settings continue through typed component configuration. Common host facts
use an immutable application environment shared by daemon, shell and plugin context.
Relative data paths require an explicit base directory; they never silently use
the process working directory. The base is normalized once before use.

## Delivery Gates

- Direct and shell-driven plugin lifecycle tests.
- Stop during initialization/startup, repeated and concurrent shutdown, delayed
  completion, cleanup failure and retry, late observation and stale API admission.
- Data-path resolution independent of subsequent working-directory changes.
- Official plugin and package consumers, structure checks and focused sanitizers.
- Independent review and correction on the final source revision before dev merge.
- Only task-owned, inactive builds are removed after delivery.

This document is the implementation contract. The scoped results and remaining
evidence boundaries are recorded in the
[acceptance receipt](forge-app-lifecycle-v1-acceptance.md).
