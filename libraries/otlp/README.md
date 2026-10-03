# forge_otlp

`forge_otlp` is the opt-in OpenTelemetry Protocol adapter for FORGE logs. It
exports `forge_log` records to an OTLP/HTTP JSON collector and can resend
minimal crash-spool records on the next start.

## Responsibility

- `forge_log` owns log records, structured fields, redaction and synchronous sink
  mechanics.
- `forge_net_http` owns HTTP client mechanics.
- `forge_asio` owns runtime scheduling, timers and cancellation.
- `forge_otlp` owns OTLP log JSON mapping, bounded export queues, retry policy,
  explicit flush/shutdown, and crash-spool resend.

Telemetry export is opt-in. Installing `forge_log` does not automatically create
an exporter or contact a collector.

## When To Use

- Export `forge_log` records to an OTLP/HTTP JSON collector.
- Add a bounded asynchronous log sink with flush and shutdown semantics.
- Persist minimal crash evidence for next-start resend.
- Keep telemetry mapping in Forge while application alerting and retention
  policy remain above Forge.

## When Not To Use

- Do not use `forge_otlp` as a metrics or tracing SDK. This library currently
  exports logs and crash-spool records.
- Do not use it to define application logger names, privacy policy or routing
  policy.
- Do not import OpenTelemetry SDK globals or protobuf/gRPC dependencies through
  this adapter.
- Do not use it for unbounded fire-and-forget background export.

## Public Modules

- `forge.otlp.log_exporter` - OTLP/HTTP JSON log exporter and export metrics.
- `forge.otlp.log_sink` - non-blocking `forge::sink` adapter with bounded queue
  semantics.
- `forge.otlp.options` - resource, queue, batch and retry options.
- `forge.otlp.crash` - POSIX crash-spool install and next-start resend helpers.
- `forge.otlp.exceptions` - typed OTLP exception category.
- `forge.otlp` - aggregate module.

Target: `forge_otlp`.

Dependencies: `forge_exceptions`, `forge_log`, `forge_asio` and `forge_net_http`. The
library does not depend on `opentelemetry-cpp`, gRPC, protobuf, backend SDKs,
P2P, plugins or application code.

## Examples

### Export Logs

```cpp
#include <forge/log/macros.hpp>

import forge.asio.runtime;
import forge.log.logger;
import forge.otlp.options;
import forge.otlp.log_exporter;
import forge.otlp.log_sink;

auto runtime = forge::asio::runtime{};
auto exporter = std::make_shared<forge::otlp::log_exporter>(
   runtime,
   forge::otlp::log_exporter_options{
      .endpoint = "http://127.0.0.1:4318",
      .logs_path = "/v1/logs",
      .resource = {.attributes = {{"service.name", "forge-app"}}},
   });

auto log = forge::logger{"app"};
log.add_sink(std::make_shared<forge::otlp::log_sink>(exporter));
ilog(log, "startup complete");

co_await exporter->async_flush();
```

### Resend Crash Spool

```cpp
import forge.otlp.options;
import forge.otlp.log_exporter;
import forge.otlp.crash;

auto guard = forge::otlp::install_crash_capture(
   forge::otlp::crash_spool_options{.directory = "./crash-spool"});

auto result = co_await forge::otlp::async_resend_crashes(
   *exporter,
   forge::otlp::crash_spool_options{.directory = "./crash-spool"});
```

Crash capture writes only bounded, fixed-size records from signal-safe paths.
Human-readable JSON, HTTP export, symbolication and redaction run later during
normal process startup. By default `chain_after_capture` remains enabled, so
FORGE records evidence and then returns to the platform/default crash handling;
tests or supervised helpers may disable it to exit cleanly after capture.

## Security And Redaction

- Secret-like structured fields must be redacted before they enter OTLP payloads.
- Crash-spool records are intentionally minimal and bounded; do not write
  arbitrary log messages from signal handlers.
- Custom headers and endpoints are operator-controlled config and should not be
  copied from untrusted remote input.
- Queue overflow policy is explicit. Prefer dropping or blocking according to
  the application risk profile rather than growing queues without bounds.

## Common Mistakes

- Adding the exporter and forgetting `async_flush()` during graceful shutdown.
- Treating `enabled: false` as a partially configured exporter. It is a no-op.
- Logging tokens in resource attributes, logger names or custom headers.
- Adding metrics/traces to this log adapter instead of a focused future
  exporter.

## Tests

- `test_forge_otlp` covers OTLP JSON shape, batching, queue overflow, retry,
  shutdown, redaction and crash-spool resend.
- `test_forge_package_otlp_component` verifies
  `find_package(Forge CONFIG REQUIRED COMPONENTS otlp)` and `Forge::forge_otlp`.
- `test_forge_log`, `test_forge_http_websocket` and `test_forge_app` cover adjacent
  logging, HTTP and app behavior.

## Boundaries

- Logs and minimal crash evidence are in scope.
- Metrics and traces are future exporters with separate runtime semantics.
- Native minidumps, Crashpad, Breakpad, watchdogs, gRPC and protobuf are out of
  scope for this library version.
- Application authorization, privacy policy, alert routing and storage retention
  belong above FORGE.
