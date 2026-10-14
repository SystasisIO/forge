# Forge Log

`forge_log` provides structured logging with bounded asynchronous configured
output. Use
`tlog/dlog/ilog/wlog/elog` for application events: logger names, message
templates, structured fields and callsite metadata reach the same record/sink
path. Use direct `log_record` construction when an event needs explicit
timestamps, thread metadata, component, exception chain or stacktrace.

Macros prepare records synchronously, without changing their interface.
Configured console, text-file and JSONL output crosses a bounded queue onto a
dedicated Forge Asio compute worker. Direct sink objects remain synchronous;
`forge.log.async_sink` adapts a custom blocking sink with explicit worker
ownership. No HTTP/P2P I/O worker performs configured output. Optional OTLP delivery belongs to
[Forge OTLP](../otlp/README.md) and its
[plugin](../../plugins/log/otlp/README.md).

## Target and modules

Target: `forge_log`; installed target: `Forge::forge_log`; package component:
`log`.

- `forge.log.logger`: named loggers, routing and macro implementation hooks;
  re-exports the record types.
- `forge.log.record`: levels, structured fields/records, console/file/JSONL
  sinks, thread names and FORGE-owned stacktrace snapshots.
- `forge.log.logger_config`: built-in sink configuration and named logger policy.
- `forge.log.exceptions`: typed configuration and file errors.
- `forge.log.async_sink`: bounded FIFO adapter, delivery counters, flush fences
  and deadline-aware shutdown waits on an externally owned compute executor.
- `forge/log/macros.hpp`: macro-only header; modules cannot export macros.

Public dependencies are Core, Exceptions, Reflect, Variant/Schema, Asio and Boost
headers. Chrono, Boost.DLL and the optional stacktrace backend are private.
Boost.DLL supplies the default process name for thread metadata.

## Ordinary events

```cpp
#include <forge/log/macros.hpp>
import forge.log.logger;

ilog("network", "Peer ${peer} connected", ("peer", peer_id)("port", port));
dlog("network", "Queue depth ${depth}", ("depth", expensive_queue_depth()));
ilog("process ready"); // one argument selects the default logger

auto log = forge::logger::get("network");
wlog(log, "Peer unavailable", ("peer", peer_id));
```

The named form accepts a logger name (`const char*`, `std::string`,
`std::string_view`) or a logger object. With fields, a logger is always
explicit: `ilog("default", "Ready ${port}", ("port", port))`.
Two arguments always mean logger plus message/record, so two strings are
never classified by their contents.

The logger expression is evaluated once. Its level filter runs before the
message, record expression or field values are evaluated. A macro captures the
user callsite, including file, line and function. `FORGE_DISABLE_LOGGING`
removes every level and every argument expression at preprocessing time.
Macros are best-effort: exceptions from route resolution, message/field
preparation or delivery do not escape into the instrumented operation. The
emergency path increments `log_failure_count()`, never exception text or an
emergency stderr write that could block the caller.
Do not put required application side effects in logging arguments. Direct APIs
retain ordinary eager argument evaluation and may throw during preparation.

Levels retain their order: `all` (the `tlog` level), `debug`, `info`,
`warn`, `error`, `off`. An originating logger filters its own event.
Parent routing is additive and does not refilter that event. A sink shared by
the child and its parents receives it once; cycle detection bounds routing.

### Concurrent routes and handles

Logger copies share route state. Setters for enabled/level/name/parent, getters,
sink attachment/removal and registry operations use the same routing mutex.
A delivered record captures its originating filter, name and complete additive
sink route under that mutex. Formatting and sink callbacks run after unlocking.
Concurrent setters may therefore affect later records, not a delivery already
captured. Separate setter calls are not one atomic configuration transaction;
macro filtering remains a lazy preliminary check before the delivery check.

Handles use the standard C++23 `shared_ptr` atomic load/store/exchange free
functions, supported by the current libc++ toolchain. Copying, assigning and
reading the same live logger handle can run concurrently. Each operation retains
one implementation snapshot; changing a handle does not retarget existing
copies. Move construction empties its source; move assignment preserves the
existing swap behavior. A two-handle move assignment is not an atomic transaction
for concurrent observers. Object destruction still requires the caller to end
concurrent access to that object.

`default_logger()` keeps its mutable reference API. `configure_logging(...)`
replaces that handle atomically while publishing the registry. Ordinary
one-argument macros resolve `"default"` by name each time, not through the mutable
reference. Assigning the reference directly does not update the registry; use
`logger::update("default", route)` to update both. Retained logger copies keep
their previous routes after configuration replacement.

Registry creation/update reads route state directly while holding the routing
mutex; it must not call the public locking getters/setters from inside that
lock. Retired handles and sinks are released after unlocking, including on
parent replacement, registry update and configuration replacement. Configured
asynchronous adapters never join their worker from those paths.

## Diagnostic fields and redaction

Each named value passes through
`forge::variant_schema::encode_diagnostic(value)` before interpolation or
stringification. Log does not implement a serializer:

- Schema fields use their canonical names and `.secret()` metadata.
- Nested Schema objects, containers, Describe and compatible PFR aggregates
  are traversed recursively.
- Known secret types are redacted through the shared neutral
  `diagnostic_is_secret(value)` customization.
- Existing non-secret scalar and custom conversions remain available.
- Opaque unsupported values become `<unsupported>`; diagnostic failures
  become `<diagnostic-error>`; secret values become `<redacted>`.
- Ordinary strings have no secret-name or secret-content heuristic.

Both `${field}` interpolation and the delivered field string use the safe
diagnostic representation. A failure never retries raw serialization.
Wire, persisted, JSON/YAML and Raw encoders keep their normal behavior;
diagnostic records are not replacements for those formats.

Treat plain messages, exception-chain text and manually constructed field
strings as public diagnostic text. For explicitly secret scalar values in an
advanced record, use `log_secret(key, value)`; it never serializes the value.
Ordinary field helpers `log_ctx` remain available for dynamic/advanced records,
but ordinary macro pairs do not require them.

## Advanced records and direct APIs

```cpp
#include <forge/log/macros.hpp>
import forge.log.logger;

auto route = forge::logger::get("worker");
auto record = forge::log_record{
   .component = "task",
   .message = "Task ${id} failed",
   .fields = {forge::log_ctx("id", task_id), forge::log_secret("credential", credential)},
   .timestamp = observed_at,
   .thread_id = observed_thread,
   .thread_name = "worker",
   .location = captured_location,
   .stacktrace = forge::capture_stacktrace(),
   .exception_chain = safe_exception_chain,
};
elog(route, record);
```

A full-record macro sets the macro's level and the selected logger route.
Explicit component, timestamp, thread metadata, exception chain and stacktrace
are preserved. An explicit location is preserved; a missing location is filled
from the macro callsite. An empty timestamp/thread remains empty, matching
direct `logger.log(record)` semantics.

The direct `logger.log(record)`, `logger.log(level, message, fields, location)`
and `debug/info/warn/error` methods use the same filtering and sink route.
Direct field and message expressions follow normal eager C++ evaluation.
Message-based calls supply the current timestamp/thread and capture a
stacktrace for errors. Full records keep their supplied metadata.

## Sinks and configuration

```cpp
import forge.log.logger;
import forge.log.logger_config;
import forge.variant.value;

auto config = forge::logging_config{};
config.sinks.push_back({
   .name = "events",
   .type = "jsonl",
   .args = forge::mutable_variant_object{}("path", "events.jsonl")("append", true),
});
auto route = forge::logger_config{"default"};
route.level = forge::log_level::info;
route.sinks = {"events"};
config.loggers.push_back(route);
if (!forge::configure_logging(config)) {
   // Reject startup or report the invalid document through the host's policy.
}
```

Built-in types are `console`, `file` and `jsonl`. File sinks require a
non-empty `path`; `append` defaults to true. Console `stream` accepts
`std_error`, `std_out` or `by_level` (stdout for ordinary events, stderr for
warnings/errors). The default configuration uses the stderr console route at
info level. A direct `console_sink(bool)` preserves the existing by-level or
stdout behavior; `console_sink(console_stream)` selects an explicit stream.

Every configured sink defaults to `asynchronous=true`, `max_records=8192`, and
`max_bytes=8*1024*1024`. Limits include the record inside the output callback;
the byte estimate includes owned strings, fields, trace frames and container
storage, not allocator bookkeeping or process-wide memory. Saturation rejects
the new record immediately and counts it. `asynchronous=false` is an explicit
synchronous option for controlled CLI/test callers, not a safe server default.
`logging_delivery_metrics()` aggregates configured generations, including
completed retired sinks. Direct sinks and OTLP have their own counters.

The process-scoped configured worker retains at most 64 asynchronous sinks
across current and still-used retired configurations. Idle owner-only adapters
are reclaimed during configuration. Capacity is reserved before opening files;
exhaustion rejects the new configuration and preserves the active routes.
One blocked configured sink can delay delivery to others, but not producers;
use separately owned dedicated executors when output isolation is required.

### Flush and ownership

```cpp
import forge.log.logger_config;

// After all producers and application shutdown logs, while an await executor lives:
const bool drained = co_await forge::async_shutdown_logging(std::chrono::seconds{5});
// false means delivery is still pending, NOT that its callback was terminated.
```

`async_flush_logging(timeout)` captures fences for the currently retained
adapters at call time. It waits only for those accepted records, including
failed delivery attempts, not for a moving empty queue or downstream durability.
`async_shutdown_logging(timeout)` first closes admission in every generation,
then waits for their fences and shuts down the dedicated compute owner.
Repeated/concurrent waits are supported. A timeout/cancel only ends that wait;
retry after the output recovers. New configurations after shutdown are rejected;
late records are dropped rather than synchronously replayed. Shutdown is owned
by the process, not an individual `application_shell` sharing global log routes.
Explicitly synchronous sinks (`asynchronous=false`) and directly attached sinks
are outside this asynchronous delivery lifecycle; their caller owns completion.

Custom adapters take a `forge::asio::compute::executor` from a dedicated pool.
Close/drain the adapter before stopping that pool; keep it alive until every
callback returns. `async_sink` destruction closes admission without joining.
The configured owner's process-exit fallback joins workers but may discard
pending work. Neither a deadline nor a destructor can safely kill an arbitrary
non-returning C++ callback: such a callback can still prevent final worker join.
No detached thread or forced cancellation hides that limitation.

Async output callbacks must not log back into async sinks: those reentrant
records are counted and suppressed to avoid cross-worker feedback loops.
Do not wrap the OTLP sink again; its existing bounded queue already isolates
network waits. HTTP logging uses this same infrastructure, not a second queue.

Sink/logger names must be non-empty and unique, including the default logger.
All names, references, types and arguments are validated before any sink opens
a file. Structural validation failures keep the previous routes and files.
Opening resources can still fail after validation; `append=false` intentionally
truncates a file when the sink is created. Application composition controls when
configuration changes are permitted.

Programmatic custom sinks derive from `forge::sink` and attach with
`logger.add_sink(...)`; no appender factory or bridge exists. Sink callbacks
and final sink destruction run outside the routing lock, so they can log or
change routes reentrantly. Removing a sink excludes it from subsequent route
snapshots, but does not cancel an already captured delivery: that snapshot keeps
the sink alive until delivery completes. Custom sinks must synchronize their
own mutable state when multiple logging threads call them. File/JSONL sinks lock their own output; console
output is synchronized across all console sinks for a complete record.
Direct sink failures are contained and counted by the emergency counter.
A sink is not invoked recursively while it is already delivering on that
thread, even through a different route. Other sinks still receive the nested
event. Sink callbacks must remain short; this guard is not an asynchronous
queue and does not make synchronous file/console output non-blocking.

Thread names are released at normal thread exit. Late static-destructor logs
use the `shutdown` fallback instead of accessing destroyed thread-local state.
`test_forge_log_shutdown` exercises the real macro path after TLS teardown and
checks that throwing sinks increment the counter without fallback I/O.

## Tests and migration

`test_forge_log` covers macro forms, laziness, single logger evaluation,
callsite metadata, all levels and compile-disabled logging; nested Schema
redaction and safe diagnostics; record/direct parity; parent deduplication;
console/file/JSONL parity; concurrent console writes; configuration validation
before file creation; and exception routing.

[`concurrency_tests.cpp`](../../tests/log/concurrency_tests.cpp) adds fixed-size
barrier/loop regressions for setters versus shared-route logging, name/parent
changes and cycle deduplication, concurrent shared-handle replacement,
configuration versus the mutable default reference and named resolution,
default-macro registry semantics, in-flight sink removal, reentrant callbacks
and last-owner destruction during route retirement. These tests use no sleeps
or timing assumptions. The test process needs a finite timeout to turn a
deadlock regression into a bounded failure; a race-detector run remains useful
in addition to ordinary functional tests.

OTLP and plugin tests cover delivery through the existing exporter.
Async tests cover blocked callbacks, concurrent producers, full/oversized
queues, fixed fences, timeout/retry, cancellation and reentrancy. The configured
delivery subprocess leaves a real stderr pipe unread while exercising HTTP,
then resumes the reader and verifies drain and shutdown. It also checks that
retirement exhaustion cannot truncate a new sink file.
The installed package consumer compiles macro calls from installed modules and
headers. See the [migration note](../../docs/releases/unreleased-logging-macros.md)
for the approved source/configuration break.

Common mistakes are eagerly formatting a secret before passing it to a macro,
using the default shortcut with fields, attaching the same event to separate
logger routes, and treating synchronous file writes as asynchronous work.
