# forge::plugins::log::otlp

`forge::plugins::log::otlp` connects existing `forge_log` loggers to the
`forge_otlp` HTTP/JSON log exporter. It does not add a new logging API: code
continues to use `ilog`, `wlog`, `elog`, `dlog` for the default logger and
`forge_ilog(logger, ...)` style macros for named logger routes.

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

registry.register_plugin(forge::plugins::log::otlp::descriptor());
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
startup through the local `forge.plugins.crypto.secrets` API. Its configured
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
ilog("node started");
```

Named logger:

```cpp
static auto network_log = forge::logger::get("network");

forge_ilog(network_log, "peer connected ${peer}", ("peer", peer_id));
```

The plugin attaches one shared OTLP sink to every configured `loggers[]` route
with `export: true`. Logger routing is additive: parent console appenders and
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

## Boundaries

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
- `test_forge_package_plugins_log_otlp`
