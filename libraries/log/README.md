# forge_log

`forge_log` — синхронный C++23 logging core для библиотек и программ, которым
нужны дешёвая проверка уровня, structured fields, source location, thread
identity, JSONL/text sinks, redaction и диагностический stacktrace без
зависимости на runtime/event loop.

Библиотека сохраняет старый `log_message`/appender слой как compatibility
поверхность, но новый код должен использовать `log_record`, `log_field`,
`logger::info/error(...)` и sinks. Логгер не является audit/security boundary:
секреты нужно помечать как secret до записи.

## When To Use

- Нужны синхронные console/file/JSONL logs без отдельного logging daemon.
- Нужно записывать structured context: component, fields, exception chain,
  source location, timestamp, thread id/name.
- Нужно автоматически добавлять stacktrace на error/fatal-like путях, но не
  платить за него на `debug`/`info`.
- Нужно направить `forge_exceptions` capture path в logging sink без зависимости
  `forge_exceptions -> forge_log`.

## When Not To Use

- Не используйте `forge_log` как durable audit trail или source of truth.
- Не добавляйте сюда async queue/background runtime: это будущий runtime adapter,
  а не обязанность core logger.
- Не пишите secrets обычными полями. Используйте `log_secret(...)`.
- Не держите application-specific trace schema в FORGE. Приложение может использовать
  `forge_log` как sink, но schema принадлежит продукту.

## Public Modules

- `forge.log.record` — `log_record`, `log_field`, sinks, stacktrace snapshot.
- `forge.log.logger` — logger hierarchy, level checks, v2 logging API.
- `forge.log.log_message` — retained message formatter.
- `forge.log.appender`, `forge.log.console_appender`, `forge.log.logger_config` —
  retained appender compatibility.

Macro-only header:

- `forge/log/macros.hpp` — retained convenience macros and modern `forge_log(...)`.

Target: `forge_log`.

Dependencies: `forge_chrono`, `forge_core`, `forge_reflect`, `forge_variant`, Boost headers,
private Boost.DLL and optional private Boost.Stacktrace fallback. Public API
does not expose `std::stacktrace` or `boost::stacktrace`.

## Hierarchy Routing

A child logger routes to every appender and structured sink attached to itself
and its parents. A shared appender or sink is delivered once, even when it is
attached at several levels. The source logger name is preserved: a record
emitted by `chain.producer` remains `chain.producer` when a parent route adds a
console or OTLP destination.

The retained appender API only accepts `log_message`. When new structured code
is routed to an appender, Forge bridges the complete redacted text record into
that compatibility surface rather than dropping fields. Conversely, a legacy
`log_message` is converted once for structured sinks, so a logger can fan out
to console appenders and OTLP at the same time.

## Stacktrace Backend

Backend order:

1. use `std::stacktrace` when the toolchain exposes `<stacktrace>` and the
   feature macro;
2. otherwise use private `Boost::stacktrace_basic` when available;
3. otherwise return `stacktrace_unavailable`.

Consumers always see only `forge::stacktrace_snapshot`. Missing stacktrace support
is a degraded diagnostic mode, not a build failure for consumers that do not
need stack traces.

## Examples

### Attach Sinks

```cpp
#include <memory>

import forge.log.logger;
import forge.log.record;

auto log = forge::logger{"service"};
log.set_log_level(forge::log_level::debug);
log.add_sink(std::make_shared<forge::console_sink>());
log.add_sink(std::make_shared<forge::jsonl_sink>("service.jsonl"));
```

### Write Structured Logs

```cpp
import forge.log.logger;
import forge.log.record;

log.info(
   "listener started",
   {
      forge::log_ctx("component", "http"),
      forge::log_ctx("bind", "127.0.0.1:8080"),
   });

log.error(
   "login failed",
   {
      forge::log_ctx("user", "alice"),
      forge::log_secret("access-token", token),
   });
```

`log_secret(...)` stores `<redacted>` in text and JSONL output. Do not put
tokens, private keys or passphrases into the plain message string.

### Avoid Building Disabled Records

Use the `forge_log(...)` macro when a field is expensive to compute. The provider
is evaluated only after `logger.is_enabled(level)`.

```cpp
#include <forge/log/macros.hpp>

import forge.log.logger;
import forge.log.record;

forge_log(
   log,
   forge::log_level::debug,
   "scheduler snapshot",
   forge::log_field_provider{[&] {
      return forge::log_ctx("queue-depth", expensive_queue_depth());
   }});
```

For cheap fields, direct `logger.info(...)`/`logger.error(...)` is clearer.

### Route Exception Capture Into Logger

`forge_exceptions` owns the capture helpers, but it does not depend on `forge_log`.
A program wires them together explicitly at the edge.

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;
import forge.app.exceptions;
import forge.app.application;
import forge.app.events;
import forge.app.diagnostics;
import forge.app.signals;
import forge.app.plugin_context;
import forge.app.plugin;
import forge.app.plugin_registry;
import forge.app.application_shell;
import forge.app.application_builder;
import forge.app.runner;
import forge.app.daemon;
import forge.log.logger;
import forge.log.record;

forge::exceptions::set_log_sink([&](std::string_view chain) {
   log.error(
      "operation failed",
      {
         forge::log_ctx("exception-chain", chain),
         forge::log_secret("request-token", token),
      });
});

try {
   run_operation();
} FORGE_CAPTURE_AND_LOG(
   "operation failed",
   forge::exceptions::ctx("phase", "startup"),
   forge::exceptions::secret("request-token", token))
```

Use `FORGE_CAPTURE_AND_LOG` only for explicit cleanup/best-effort paths. If the
operation must fail the caller, use `FORGE_CAPTURE_AND_RETHROW` or
`FORGE_CAPTURE_LOG_AND_RETHROW`.

### Log Runtime Failures Without Turning Logs Into Recovery

```cpp
#include <forge/exceptions/macros.hpp>

import forge.exceptions;
import forge.log.logger;
import forge.log.record;

boost::asio::awaitable<void> start_with_logging(forge::app::application_shell& app) {
   try {
      co_await app.startup();
   } catch (const std::exception& error) {
      log.error(
         "startup failed",
         {
            forge::log_ctx("exception-chain", forge::exceptions::format_exception_chain(error)),
            forge::log_secret("bootstrap-token", token),
         });
      app.request_stop();
      co_await app.shutdown();
      throw;
   }
}
```

The log call records context; it does not make the application healthy. Application
code still owns rollback, shutdown and the returned exit status.

### Format A Record Without A Sink

```cpp
import forge.log.record;

auto record = forge::log_record{
   .level = forge::log_level::warn,
   .logger = "probe",
   .component = "readiness",
   .message = "endpoint slow",
   .fields = {forge::log_ctx("latency-ms", 250)},
};

auto line = forge::format_text_log_record(record);
auto json = forge::format_json_log_record(record);
```

This is useful for tests and adapters that need deterministic formatting.

## Security Notes

- Redaction is explicit: `log_secret(...)` is safe; plain `log_ctx(...)` is not.
- JSONL output is a diagnostic stream, not signed audit data.
- Error logs may include stack traces. Avoid adding raw user payloads or secrets
  to error messages.
- Sinks are synchronous. If a file sink points to slow storage, the caller pays
  that cost.

## Runtime Risks And Anti-Patterns

- Do not log raw serialized payloads or private keys to “debug signatures”.
  Log safe IDs, hashes or redacted config paths instead.
- Do not allocate expensive fields before checking the log level. Use
  `forge_log(...)` with `log_field_provider` for expensive diagnostics.
- Do not install a slow network filesystem path as a synchronous file sink on a
  hot request path. Route hot-path telemetry through an application-owned trace layer
  or a bounded adapter.
- Do not hide errors by logging and continuing unless the code path is explicitly
  best-effort cleanup.

## Typical Mistakes

- Calling `format_stacktrace()` on hot debug paths.
- Creating `log_field_provider` and then calling `make_log_fields(...)`
  directly before checking the log level.
- Treating exception logging as recovery.
- Reintroducing old lower-level logging macros in new examples. Prefer
  `logger.info(...)`, `logger.error(...)` or `forge_log(...)`.

## Tests

`test_forge_log` covers cheap level filtering, console/file/JSONL-style
formatting, secret redaction, stacktrace fallback, and exception-chain routing.
