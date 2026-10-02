# Forge Log

`forge_log` provides synchronous structured logging. Use
`tlog/dlog/ilog/wlog/elog` for application events: logger names, message
templates, structured fields and callsite metadata reach the same record/sink
path. Use direct `log_record` construction when an event needs explicit
timestamps, thread metadata, component, exception chain or stacktrace.

The core owns no executor, queue, transport or runtime. Console, text-file and
JSONL sinks run synchronously. Optional OTLP delivery belongs to
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
- `forge/log/macros.hpp`: macro-only header; modules cannot export macros.

Public dependencies are Core, Exceptions, Reflect, Variant/Schema and Boost
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

Levels retain their order: `all` (the `tlog` level), `debug`, `info`,
`warn`, `error`, `off`. An originating logger filters its own event.
Parent routing is additive and does not refilter that event. A sink shared by
the child and its parents receives it once; cycle detection bounds routing.

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

Sink/logger names must be non-empty and unique, including the default logger.
All names, references, types and arguments are validated before any sink opens
a file. Structural validation failures keep the previous routes and files.
Opening resources can still fail after validation; `append=false` intentionally
truncates a file when the sink is created. Application composition controls when
configuration changes are permitted.

Programmatic custom sinks derive from `forge::sink` and attach with
`logger.add_sink(...)`; no appender factory or bridge exists. Sink callbacks
run outside the routing lock. File/JSONL sinks lock their own output; console
output is synchronized across all console sinks for a complete record.
Sink failures are contained and reported to stderr.

## Tests and migration

`test_forge_log` covers macro forms, laziness, single logger evaluation,
callsite metadata, all levels and compile-disabled logging; nested Schema
redaction and safe diagnostics; record/direct parity; parent deduplication;
console/file/JSONL parity; concurrent console writes; configuration validation
before file creation; and exception routing.

OTLP and plugin tests cover delivery through the existing exporter.
The installed package consumer compiles macro calls from installed modules and
headers. See the [migration note](../../docs/releases/unreleased-logging-macros.md)
for the approved source/configuration break.

Common mistakes are eagerly formatting a secret before passing it to a macro,
using the default shortcut with fields, attaching the same event to separate
logger routes, and treating synchronous file writes as asynchronous work.
