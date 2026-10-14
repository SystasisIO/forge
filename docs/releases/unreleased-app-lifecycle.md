# App Lifecycle Source Break

This maintainer-approved pre-stabilization change affects Forge.App and concrete
official plugin C++ implementations. It does not change wire contracts or stored
data. The release-train version is not changed in this feature branch.

## Consumer Changes

| Removed surface | Replacement |
| --- | --- |
| Overriding public plugin lifecycle methods | Override protected `on_*` hooks; callers still use `configure/provide/initialize/after_initialize/startup/request_stop/shutdown` |
| `plugin_state`, `application_state`, diagnostics-only `lifecycle_state` | `lifecycle_phase` and canonical `lifecycle_snapshot` |
| Mutable diagnostic state setters | Bind canonical observers in the owner; consumers read snapshots |
| Synchronous `forge.app.signals` lifecycle callbacks | `lifecycle_observer::snapshot/async_wait/async_wait_until` |
| Event subscriptions, filters and polling queues | Bounded `event_journal` for history; typed API for operations |
| `plugin_context::config/config_value`, `config_view` | Typed plugin component config and `context.environment()` for common host facts |
| `daemon_context.data_dir/profile` | `daemon_context.environment.data_dir()/profile()` |

The daemon places the same environment in `context.shell.environment`.
Derived shells forward `context.shell`; builder-based applications forward
`context.environment` through `application_builder::environment(...)`.
Relative plugin data paths use `context.environment().resolve_data_path(...)`.
An empty base rejects a relative path instead of choosing the current directory.

## Lifecycle Rules

The framework guards lifecycle entrypoints even when a test or embedded host calls
a plugin directly. Do not call protected hooks as a replacement for those methods.
Keep the shared lifecycle handle in private resource owners. Stop closes admission
and invalidates startup publication; a generation check and resource installation
must be one atomic admission operation, never a check followed by an await.

`configure`, `provide` and `initialize` accept their inputs once per plugin
instance; a repeated input-bearing call is rejected rather than silently ignoring
new input. Joined startup/after-initialize/shutdown calls share the actual
operation result. A stopped instance is terminal; create a new instance to restart.
Explicit live APIs, such as TLS context reload, remain distinct from lifecycle
configuration.

Cleanup remains the concrete owner's responsibility. Await `shutdown()` before
destroying resources. A failed cleanup leaves the owner stopping and retains its
dependencies for retry; it must not report stopped.

Diagnostics now embeds `lifecycle_snapshot` as `snapshot.lifecycle` and
`snapshot.plugins[i].lifecycle`. Its optional failure records an operation and
exception independently of the phase. A successfully stopped plugin can retain
the failure which caused shutdown. Snapshot observation can coalesce transitions;
it is not an audit-delivery guarantee.

There are no compatibility aliases or downstream source edits. Applications
update their own implementations using this mechanical mapping.
