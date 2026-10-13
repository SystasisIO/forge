# Runtime + App

Этот документ описывает связку `forge_asio` and `forge_app`. Локальные API
показаны в [libraries/asio/README.md](../../libraries/asio/README.md) and
[libraries/app/README.md](../../libraries/app/README.md); здесь фиксируется
сквозная архитектура runtime, scheduler and async application lifecycle.

## Задача

Сервисные программы обычно быстро скатываются в набор detached threads,
`std::async`, sleep/poll loops and глобальных очередей. Это плохо проверяется:
непонятно, кто владеет shutdown, где backpressure, кто отменяет delayed work и
почему startup failure оставил часть системы работающей.

FORGE baseline другой: runtime is explicit, async API is coroutine-first, queues
are bounded, shutdown is deterministic and diagnostics are part of the contract.

## Layering

```text
foreground daemon
  -> forge_app::run_daemon
  -> forge_codec_yaml / forge_config_env / forge_config_program_options
  -> forge_config_core::document
  -> forge_app::application_shell
  -> plugins configured through component_view
  -> forge_asio runtime / task scheduler / optional compute pool
```

`run_daemon(...)` is the high-level foreground-daemon entrypoint. It owns
standard source adapters, merge order, generated config, effective config
printing and signal policy. The daemon runner can independently disable YAML,
explicit `.env`, process env and app/plugin CLI sources. `run_application(...)`
remains the lower-level lifecycle runner for tests, embedded hosts and custom
product shells that already have a `config::core::document`.

`application_runtime` remains a lower-level escape hatch, while
`application_shell` is the preferred production owner of runtime, task
scheduler, optional compute pool, API registry, bounded event journal,
diagnostics, immutable application environment, plugin registry and plugin
context. The shell and its internal application runtime share one canonical
application lifecycle; each plugin owns its own canonical lifecycle.

## Runtime Ownership

- `forge::asio::runtime` owns `boost::asio::io_context` and worker threads.
- `forge::asio::task::scheduler` owns bounded pending queues, numeric priority,
  delayed tasks, cancellation handles and metrics.
- `forge::asio::compute::pool` optionally owns bounded synchronous CPU work on
  a joined `boost::asio::thread_pool`.
- Priority names are product/program policy. FORGE only orders numeric priority and
  FIFO within equal priority.
- Blocking work must cross an explicit boundary; it must not be hidden inside
  unrelated coroutine code.
- CPU-heavy synchronous work crosses the compute boundary rather than occupying
  runtime workers. The snapshot/concurrency direction is documented in
  [`compute-and-snapshots.md`](compute-and-snapshots.md).
- `forge::app::plugin_lifecycle` owns lifecycle transitions, operation admission,
  stop generations and completion notification. It does not own plugin resources,
  worker threads or a second scheduler.
- `lifecycle_observer` gives consumers read-only snapshots and asynchronous
  change waiting. Copying an observer does not grant stop or transition authority
  or a lifetime guarantee for the owner's concrete resources.

## Canonical Observation

The canonical phases are `created`, `configured`, `initializing`, `initialized`,
`starting`, `started`, `stopping` and `stopped`. Failure is a separate historical
`lifecycle_snapshot::failure`, with the operation, attempt and original
`std::exception_ptr`; it is not a parallel `failed` phase enum. Successful cleanup
does not erase the last failure.

`application_shell::lifecycle()`, `application_runtime::lifecycle()` and
`plugin::lifecycle()` return observers of these same shared objects.
`state()` is a convenience read of the canonical phase, not a second mutable
state table. Use `snapshot()` and `async_wait_until(observed_epoch, deadline)`
for observation; async notifications may coalesce changes and are not a lossless
transition stream. Destruction closes admission and wakes observers without
claiming that resource cleanup succeeded.

`diagnostics_store` binds those observers and projects them on each snapshot.
Only the host owner binds or clears the observer registry. Plugin consumers get
`const diagnostics_store*` from `plugin_context::diagnostics()`, while application
contexts expose a const diagnostics reference. Clear/rebind retires old observers
and failure exceptions outside the registry lock, since arbitrary exception
destructors may re-enter diagnostics. Neither diagnostics nor journal publication
can authorize a lifecycle transition. A diagnostics snapshot is not an atomic
transaction across all lifecycle objects and journal rings.

`forge.app.events` remains the module for `event_journal`, not an event bus.
`publish(...)`, `recent_events()`, `critical_events()` and `metrics()` provide
bounded diagnostic history. Entry-count and record-byte limits apply; oversized
records are dropped. Generic `event_subscription`, `event_filter`, `subscribe`
and `poll` are removed, and the journal invokes no consumer callbacks.
Lifecycle logging uses the configured Forge logging sinks independently of this
best-effort journal; neither logging nor history failure may veto cleanup.

`forge.app.signals` and the synchronous lifecycle signal bus are removed.
Process SIGINT/SIGTERM handling is a separate `run_application(...)` runner
concern, controlled by `run_options`; it is not exposed through plugin or shell
contexts.

## Host Environment

`forge.app.application_environment` owns immutable host facts: `data_dir()` and
`profile()`. `daemon_context::environment` is copied into
`daemon_context::shell.environment`; the shell passes that same value to
application and plugin contexts through their const `environment()` getters.
Builder-based daemon factories forward it with
`builder.environment(context.environment)` alongside the daemon runtime and
scheduler options.

A non-empty data-directory base is made absolute and lexically normalized at
environment creation. The daemon captures its initial working directory when
resolving a relative `daemon.data-dir`, so later factory or hook changes to the
process working directory do not move the base. `resolve_data_path(relative)`
resolves against that frozen base, and throws typed
`forge::app::exceptions::config_failed` when the base is absent. Absolute input
paths are returned unchanged. The environment neither creates files/directories
nor canonicalizes symlinks; it is not a filesystem sandbox. Explicit YAML and
`.env` source-path handling remains a separate daemon adapter concern.

Plugin settings still arrive through `configure(component_view)` and schema
decoding. The legacy `config_view` string map, `config()` and `config_value()`
host lookups are removed; typed component settings and common host facts must
not become parallel configuration stores.

## Application Lifecycle

The lifecycle order is fixed:

1. app and plugin config descriptors are collected;
2. schema defaults are merged with the input config document;
3. app hook receives `configure_context`;
4. plugins receive `configure(component_view)`;
5. app hook provides APIs;
6. plugins provide their declared local APIs;
7. plugins initialize in dependency order;
8. plugins complete `after_initialize()` in dependency order;
9. the application connects and publishes process-local services;
10. the application completes its read-only `after_initialize` hook;
11. plugins start;
12. `request_stop()` is issued synchronously;
13. plugins shut down in reverse order;
14. only after all plugin cleanup succeeds, task scheduler admission stops and queued work is canceled;
15. the optional compute pool drains while already-running scheduler continuations remain runnable;
16. connected services are destroyed;
17. the task scheduler asynchronously drains;
18. the host stops and joins the Asio runtime after successful cleanup.

All heavy lifecycle methods return `boost::asio::awaitable<void>`. `request_stop`
is intentionally synchronous/noexcept: it signals intent, it does not perform
cleanup.

Plugin lifecycle entrypoints are guarded and final. Derived plugins implement
only protected `on_configure`, `on_provide`, `on_initialize`,
`on_after_initialize`, `on_startup`, `on_request_stop` and `on_shutdown` hooks.
Private resource owners compose the shared `plugin_lifecycle` handle obtained
through `lifecycle_handle()`; they must not introduce duplicate plugin phases.

Derived shells implement `on_describe_config`, `on_register_plugins`,
`on_configure`, `on_provide`, `on_connect`, `on_after_initialize` and optionally
`on_run_foreground`. Public configure/initialize/startup/shutdown methods are
not extension points. `application_builder` installs callbacks into those same
hooks rather than defining another lifecycle.

Plugin configure/provide/initialize inputs are one-shot: repeated or concurrent
calls are rejected rather than silently ignoring a new view, provider or
context. Concurrent after-initialize/startup/shutdown callers join the accepted
attempt; already-successful calls run no hook again. Shell bootstrap may ensure
already-configured immutable inputs through the owner-only `begin_joined`
mechanism. This is not permission to accept new inputs after configuration.

## Daemon Integration Example

Most service `main(...)` functions should be thin:

```cpp
#include <memory>

import forge.app.daemon;

int main(int argc, char** argv) {
   return forge::app::run_daemon(
      [](const forge::app::daemon_context& context) {
         return std::make_unique<service_application>(context.shell);
      },
      argc,
      argv,
      forge::app::daemon_options{
         .name = "service",
         .display_name = "Service daemon",
         .default_data_dir_name = "service",
         .env_prefix = "SERVICE",
      });
}
```

The app still owns product composition. FORGE owns the generic daemon flow:

```text
collect descriptors -> defaults/YAML/.env/process-env/CLI merge -> configure -> startup
-> wait for stop -> request_stop -> shutdown
```

Daemon source precedence is intentionally fixed:

```text
schema defaults < daemon defaults < YAML < .env < process env < daemon CLI < app/plugin CLI
```

## Shell Integration Example

```cpp
#include <boost/asio/awaitable.hpp>

#include <utility>

import forge.app.application_shell;
import forge.app.plugin_registry;
import forge.config.core.component;
import forge.config.core.document;
import forge.plugins.net.http.server.plugin;

class service_application final : public forge::app::application_shell {
 public:
   explicit service_application(forge::app::application_shell_options options = {})
       : forge::app::application_shell{std::move(options)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::plugins::net::http::server::plugin::descriptor());
   }

   void on_describe_config(forge::config::core::component_registry& registry) const override {
      registry.add(forge::config::core::describe_component<service_config>("service"));
   }
};

boost::asio::awaitable<void> run_service(service_application& app, const forge::config::core::document& document) {
   co_await app.async_configure(document);
   co_await app.startup();
   app.request_stop();
   co_await app.shutdown();
}
```

Custom embedders may still own YAML/JSON/env/CLI adapters themselves and pass an
already-merged document to `run_application(...)`. Normal foreground daemons
should not duplicate this plumbing; use `run_daemon(...)`.

For smaller applications and tests, `application_builder` creates an
`application_shell` without introducing another lifecycle model:

```cpp
#include <boost/asio/awaitable.hpp>

#include <memory>
#include <utility>

import forge.app.application_builder;
import forge.app.application_environment;
import forge.app.application_shell;
import forge.app.connect_context;
import forge.asio.compute;
import forge.plugins.net.http.server.plugin;

auto builder = forge::app::application_builder{};
builder.name("service")
   .environment(forge::app::application_environment{"./service-data", "dev_local"})
   .compute(forge::asio::compute::pool::options{.worker_threads = 4})
   .config<service_config>("service", [&](const service_config& config) {
      configure_service(config);
   })
   .connect([](forge::app::connect_context& context)
               -> boost::asio::awaitable<void> {
      auto resolver = context.api<resolver_api>();
      context.publish(co_await make_client(*resolver));
   })
   .plugin(forge::plugins::net::http::server::plugin::descriptor());

std::unique_ptr<forge::app::application_shell> app = std::move(builder).build();
app->configure(document);
boost::asio::awaitable<void> start_then_stop(forge::app::application_shell& app) {
   co_await app.startup();
   app.request_stop();
   co_await app.shutdown();
}
```

In a daemon factory, use `.environment(context.environment)`,
`.runtime(context.shell.runtime)` and `.scheduler(context.shell.scheduler)`
instead of recreating those host options. Forward `context.shell.compute`
through `.compute(*context.shell.compute)` when it is present. The application
and plugin `environment()` getters then resolve data paths against the same
base and preserve the daemon profile.

Executable lifecycle coverage lives in `test_forge_app`. Consumer snippets stay in
this document and the library README so they cannot drift behind an unbuilt
example tree.

## Failure And Rollback

- Each accepted operation publishes its result in the canonical lifecycle
  before joiners wake. Stop closes admission and invalidates the setup generation;
  late setup success cannot publish resources after stop. Validate the generation
  and install resources through the same `try_admit(...)` boundary. Its synchronous
  installation body must not await, re-enter the lifecycle, invoke other
  virtual/user callbacks or tear down resources inside the lock.
- Configure/provide/initialize/connect/after-initialize/startup failure triggers
  stop and rollback. Cleanup covers partially configured or initialized owners,
  not just plugins that reached startup. Successful rollback preserves the
  original exception; failed rollback reports typed `shutdown_failed` with the
  cleanup exception nested. The canonical failure records the latest failed
  operation, including a failed cleanup attempt.
- Shutdown joins active setup and the synchronous stop hook before releasing
  resources. Accepted cleanup disables inherited cancellation so one canceled
  caller cannot abandon the shared cleanup attempt.
- A failed cleanup leaves `stopping` with the recorded failure and permits a
  later shutdown attempt. Already-stopped plugins are skipped on retry. Reverse
  shutdown stops at a failed dependent, retaining its dependencies until that
  dependent has been cleaned up successfully.
- On plugin cleanup failure, the shell does not stop the scheduler or compute
  pool or release connected services. The same owner must stay alive to retry;
  cleanup hooks may still need those facilities. Only after plugin cleanup
  succeeds may the shell stop scheduler admission, drain compute, release
  services and asynchronously drain the scheduler, in that order.
- Destruction is not async cleanup. Plugin/shell destructors invalidate owners
  and wake observers; hosts must finish explicit shutdown before destruction
  or decide an explicit process-level failure policy. A shutdown timeout is not
  evidence that resources have been released.

## Boundaries

- `forge_asio` imports no app/config/network/TUI code.
- `forge_app` may depend on source adapters at the high daemon-runner layer:
  `forge_codec_yaml`, `forge_config_env` and `forge_config_program_options`. Plugins and app hooks still
  see only `config::core::document` / `component_view`.
- Lifecycle observers and diagnostics are read-only views of canonical state;
  the event journal is bounded diagnostic history, not hidden business-flow
  transport. Process signals belong to the runner, not plugin coordination.
- APIs and immutable `service_view` are typed composition boundaries. Journal
  topics are not a replacement for calls, resource ownership or typed settings.

## Donor Decisions

Accepted:

- Boost.Asio executor/timer/coroutine model.
- FC-style numeric priority as a donor idea, but not FC runtime primitives.
- Container/service-style deterministic startup and rollback.
- Observable diagnostics from operations-oriented systems.

Rejected:

- `fc::thread`, `fc::future`, `fc::promise` and direct scheduling APIs.
- `std::async` as product-grade daemon/runtime worker substrate.
- Unbounded event subscribers or "fire and forget" detached work.
- CLI parser types inside plugin core.
- Global read/write windows when immutable backend snapshots permit readers and
  an ordered writer to coexist.

## Verification

- `test_forge_asio`: priority ordering, delayed tasks, cancellation, queue bounds
  plus compute admission, parallel CPU work, cancellation and shutdown.
- `test_forge_app`: config collection, configure-before-initialize, dependency
  ordering, rollback and retry, cancellation, shared lifecycle observation,
  bounded diagnostics and reverse shutdown.
- [application_environment_tests.cpp](../../tests/app/application_environment_tests.cpp):
  absolute/fail-closed path resolution and daemon-to-shell/builder-to-plugin
  environment forwarding after a factory changes the working directory.
- [diagnostics_tests.cpp](../../tests/app/diagnostics_tests.cpp): read-only
  canonical projection, concurrent bounded journal snapshots and clear/rebind
  retirement of reentrant failure destructors outside the registry lock.

These are executable coverage locations, not a claim that a particular build,
test run or downstream integration has passed.
