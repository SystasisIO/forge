# Unreleased: unified logging macros

This is a maintainer-approved pre-stabilization source/configuration break.
It does not change OTLP transport, persisted layouts or normal Variant,
JSON/YAML and Raw serialization. No release/version/tag is implied.

## Source migration

Include `<forge/log/macros.hpp>` and import `forge.log.logger`.

| Previous source | Current source |
|---|---|
| `forge_ilog(log, message, ("key", value))` | `ilog(log, message, ("key", value))` |
| `ilog(message, ("key", value))` | `ilog("default", message, ("key", value))` |
| `logger::get("category").info(message, make_log_fields(log_ctx("key", value)))` | `ilog("category", message, ("key", value))` |
| `forge_log(log, log_level::debug, message, ...)` | `dlog(log, message, ("key", value))` |
| `FORGE_LOG_MESSAGE(...)` / `log_message` | `log_record`, passed to `ilog(log, record)` or `log.log(record)` |
| `appender::log(log_message)` | `sink::log(const log_record&)` |
| `logger.add_appender(value)` | `logger.add_sink(value)` |

The same conversion applies to all five levels. The one-argument
`ilog(message)` shortcut remains. Two arguments always select logger plus
message/record. Expensive values belong directly in macro pairs, so the level
filter runs first; `log_field_provider` and `make_log_fields` are removed.

Imports of `forge.log.log_message`, `forge.log.appender` and
`forge.log.console_appender` become `forge.log.record` or
`forge.log.logger`. `log_level` and thread-name helpers live in
`forge.log.record`. `forge_*log`, generic `forge_log`,
`FORGE_LOG_CONTEXT`, `FORGE_LOG_MESSAGE`, old dump helpers and appender
factory registration are removed, with no aliases.

Direct record/sink APIs and the modern logger convenience methods remain.
Full-record macros choose the level and route while preserving explicit
metadata. A missing source location is filled from the macro callsite.

## Configuration migration

| Previous field/type | Current field/type |
|---|---|
| `logging_config.appenders` | `logging_config.sinks` |
| `logger_config.appenders` | `logger_config.sinks` |
| `appender_config` | `sink_config` |
| `log_config::register_appender<T>(...)` | Derive `sink` and attach its instance with `add_sink` |

Keep configured names and logger references. Built-in sink types are
`console`, `file` and `jsonl`. Console `args.stream` accepts
`std_error`, `std_out` and `by_level`; old color/flush appender options
are removed. File/JSONL `args` use `path` and optional `append`.
The unused `includes` and unsupported file-parsing overload are removed;
the application supplies the decoded `logging_config` document.

The default config now contains only the used stderr console sink.
Config validation rejects unknown sink references/types/streams and duplicate
sink/logger names before opening any file. Existing named parent routing,
origin-level filtering and shared-sink deduplication continue.

## Diagnostic values

Ordinary macro values now use `variant_schema::encode_diagnostic` before
interpolation. Schema secret metadata, nested objects/containers and known
secret types are redacted. Unknown types use `<unsupported>`; failed
diagnostics use `<diagnostic-error>` without a raw fallback.

`log_ctx` and `log_secret` remain advanced record-field helpers. Ordinary
strings are public values; their names/content do not trigger secret guessing.
Do not eagerly format credentials or raw key material into messages.
