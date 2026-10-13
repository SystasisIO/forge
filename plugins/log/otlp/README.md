# forge::plugins::log::otlp

## Lifecycle Migration

Public lifecycle entry points are final framework wrappers; implementations use
protected `on_*` hooks with unchanged arguments. See the
[shared migration contract](../../README.md#lifecycle-migration). No compatibility aliases
or plugin-local schedulers are provided.

Exporter construction belongs to `on_after_initialize`; the base `startup()`
ensures that barrier for direct consumers too. Shared stop closes management API
admission, but logger sinks remain attached until reverse shutdown reaches OTLP.
Admitted call counts, sink detach, crash-capture ownership and awaited exporter
cleanup remain backend responsibilities.

`forge::plugins::log::otlp` connects existing `forge_log` loggers to the
`forge_otlp` HTTP/JSON log exporter. It does not add a new logging API: code
uses `tlog/dlog/ilog/wlog/elog` with named logger routes or a one-message
default-logger shortcut.

## When To Use

- A `forge_app` daemon needs configurable OTLP log export through the plugin
  lifecycle.
- Operators should control logger routes, queue sizes, retry windows and
  crash-spool resend through config.
- Product code should keep using `forge_log` APIs while export is wired once by
  infrastructure.

## When Not To Use

- Do not use this plugin for metrics or traces. It is log-export wiring only.
- Do not use it to define product logger names or alert policy.
- Do not parse environment variables or discover collectors here; config
  sources are application-shell-owned.

## Identity And Package

```cmake
find_package(Forge REQUIRED COMPONENTS plugins_log_otlp)
target_link_libraries(app PRIVATE Forge::forge_plugins_log_otlp)
```

```cpp
import forge.plugins.log.otlp.plugin;

registry.register_plugin(forge::plugins::log::otlp::plugin::descriptor());
```

Runtime identity:

- Plugin id: `forge.plugins.log.otlp`
- Main API id: `forge.plugins.log.otlp`
- Config section: `plugins.log.otlp`
- Target/component: `forge_plugins_log_otlp` / `plugins_log_otlp`
- Public modules:
  - `forge.plugins.log.otlp.plugin`
  - `forge.plugins.log.otlp.api`
  - `forge.plugins.log.otlp.types`
  - `forge.plugins.log.otlp.exceptions`

## Dependencies

- `forge_app`
- `forge_api_core`
- `forge_log`
- `forge_otlp`
- `forge_config_core`
- `forge_schema`
- `forge_plugins_crypto_secrets` only when a configured header uses
  `secret-id` and `purpose`.

## Configuration

Configuration is schema-driven through `BOOST_DESCRIBE_STRUCT`,
`forge::schema::rules<T>` and `forge::config::core::decode<T>()`.

```yaml
plugins:
  log:
    otlp:
      enabled: true
      export-enabled: true
      endpoint: "http://localhost:4318"
      logs-path: "/v1/logs"
      protocol: "http-json"

      loggers:
        - name: "default"
          enabled: true
          level: "info"
          export: true
        - name: "network"
          enabled: true
          level: "debug"
          export: true

      resource:
        attributes:
          - key: "service.name"
            value: "forge-node"

      queue:
        max-records: 8192
        max-bytes: 8388608
        overflow: "drop-new"

      batch:
        max-records: 512
        max-bytes: 524288
        flush-interval-ms: 5000

      retry:
        max-attempts: 3
        base-delay-ms: 100
        max-delay-ms: 5000

      request-timeout-ms: 30000
      shutdown-timeout-ms: 5000

      headers:
        - name: "Authorization"
          secret-id: "otlp/grafana-cloud"
          purpose: "otlp.logs.authorization"

      crash-spool:
        enabled: false
        directory: "./crash-spool"
        resend-on-startup: true
```

`enabled` is the application-owned plugin selection flag. When it is `false`,
the OTLP plugin is not instantiated. `export-enabled: false` keeps the plugin
loaded and applies configured logger routes without creating an exporter,
attaching an OTLP sink or starting network work. Named routes retain the
default console parent.

### Header Sources

Each header has exactly one source:

- compatibility/local-test literal: `value`;
- production secret reference: `secret-id` and `purpose` together.

Supplying neither source, both sources, or only one half of a secret reference
is an `invalid_config` error. A secret reference is resolved once during plugin
`after_initialize()` through the local `forge.plugins.crypto.secrets` API. Its configured
purpose must be allowed by that plugin. Missing Secrets support, an unknown
secret, or denied purpose stops OTLP startup with a typed error that names only
the header, secret id and purpose; secret bytes are never placed in diagnostics,
effective configuration or logs.

For production, keep vendor tokens out of YAML and configure Crypto Secrets
with the `otlp.logs.authorization` purpose. Literal values remain supported for
existing deployments and isolated local tests, but should not carry cloud
credentials.

## Examples

### Logging

Default logger:

```cpp
#include <forge/log/macros.hpp>
import forge.log.logger;

ilog("node started");
```

Named logger:

```cpp
#include <forge/log/macros.hpp>
import forge.log.logger;

ilog("network", "peer connected ${peer}", ("peer", peer_id));
```

The plugin attaches one shared OTLP sink to every configured `loggers[]` route
with `export: true`. Logger routing is additive: parent console sinks and
the OTLP sink both receive the same structured record, preserving its source
logger name. If the same shared sink is reachable through multiple route levels,
it is invoked once. Logger names are user-defined; the plugin does not hardcode
product domains.

### Management API

The plugin exposes a local-only management API:

```cpp
auto logs = context.apis().get<forge::plugins::log::otlp::api>(
   {.id = {"forge.plugins.log.otlp"}, .major = 1});

co_await logs->flush();
auto metrics = co_await logs->metrics();
```

`flush()` waits for queued OTLP log records to be exported. `metrics()` returns
queue/export counters from the underlying `forge_otlp` exporter.

Both management methods may execute on other executors concurrently with reverse
shutdown. Admission happens when their coroutine executes, under a short lock;
constructing an awaitable is not admission. An admitted call pins its exporter
until its coroutine completes or is cancelled. The plugin counts these calls and
joins them with `forge.asio.notification`, after core export shutdown, before
releasing its runtime owner. No lock covers an await or exporter I/O. An API
handle, or an unstarted management awaitable, may outlive plugin/runtime teardown;
executing it then raises the existing `exporter_unavailable` error.

`request_stop()` leaves sink export available but closes new management
admission through the shared lifecycle. Accepted calls finish or cancel safely;
cancellation of a flush waiter does not stop the shared sink drain.
Startup failure/export-disabled mode also reports `exporter_unavailable`.
Plugin lifecycle phases remain application-owner serialized; this contract does
not introduce concurrent configure/startup/restart as supported lifecycle use.

The exporter timeout bounds export work, not a caller executor that has stopped
making progress. Plugin shutdown deliberately waits for admitted caller
continuations to release their exporter owners. Keep those executors running
until the join finishes; an unstarted awaitable never causes that wait.

## Boundaries

### Application Lifecycle Order

Register OTLP before the plugins whose initialization/shutdown logs must be
exported, for example OTLP, Secrets, DB Store, then P2P. The existing application
registry preserves independent root registration order and honors dependencies.
OTLP attaches its sink in `after_initialize()`, after every plugin's
`initialize()`. This covers the subsequent DB Store open in `after_initialize()`
and P2P startup. Header resolution, optional crash capture and crash resend also
occur in that phase. `startup()` is idempotent and does not attach a second sink.

Shutdown runs in reverse order: producers finish before OTLP flush/shutdown.
Finishing producers includes joining their in-progress synchronous logging calls.
Detaching a sink prevents future route snapshots from acquiring it, but does not
cancel a delivery that already captured the sink. Such a delivery must finish
before the application's runtime is destroyed. Logging through detached routes
can continue; the requirement concerns already captured OTLP deliveries.
OTLP `request_stop()` does not close log admission. Application-added dependency
edges must not require an explicitly disabled OTLP plugin. DB/P2P library and
canonical plugin descriptors deliberately do not depend on telemetry export.

This is not capture of all process startup: configuration, earlier initialize
events and the application shell's final stopped event remain local unless the
application installs an exporter outside the plugin lifecycle. There is no second
bootstrap queue. An unavailable collector does not block DB/P2P operations;
bounded queue/retry/shutdown may drop records, reported by exporter metrics.
Keep a local sink when those best-effort export guarantees are insufficient.

- Logs only in this plugin; metrics and traces are future separate plugins.
- `forge_log` owns logger names, macros and sink dispatch.
- `forge_otlp` owns OTLP JSON mapping, batching, retry and crash-spool mechanics.
- The plugin owns only config/lifecycle wiring.
- No OpenTelemetry SDK globals, environment auto-discovery, new logging macros,
  direct `send(record)` API, auth policy or product logger names are introduced.

## Security And Common Mistakes

- Do not log secrets in logger names, resource attributes or structured fields.
- Use `secret-id` plus `purpose` for cloud authorization headers. Header source
  validation runs before HTTP requests are built.
- `export-enabled: false` creates no exporter and starts no network work, but
  still configures logger routes and their console-parent routing.
- Flush during shutdown when callers require best-effort delivery of queued
  records.

## Tests

- `test_forge_plugins_log_otlp`
- Gated concurrency tests cover retained APIs after stop/teardown, delayed
  unstarted calls, cancellation, multi-executor metrics/flush, and a blocked
  external caller continuation that shutdown must join.
- `test_forge_package_plugins_log_otlp`
