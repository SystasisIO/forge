# forge_app

`forge_app` is the opinionated application shell for FORGE services. It owns the
runtime objects that every daemon tends to duplicate by hand: `runtime`,
`scheduler`, optional `compute::pool`, API registry, bounded event journal,
diagnostics, plugin registry, plugin context and lifecycle runtime.

The preferred production entrypoint is `forge::app::application_builder`, which
returns an `application_shell` without asking the application to subclass the shell.
Derived `application_shell` classes remain available as an advanced escape hatch
for applications with substantial state or custom lifecycle hooks.

## When To Use

- A program has plugins with config, dependency order and async lifecycle.
- A daemon needs one place to collect app config and plugin config.
- A foreground daemon wants standard YAML, `.env`, process env, CLI and default
  merge before lifecycle starts.
- You want deterministic startup, rollback, reverse shutdown and diagnostics.
- You want typed APIs between plugins without coupling them through globals.
- The application must connect a long-lived process-local client after plugin
  initialization and expose it before plugin startup.
- Plugins must publish config descriptors without knowing whether values came
  from source adapters such as YAML, JSON, `.env`, process env or CLI.

## When Not To Use

- Do not use `forge_app` as a generic dependency injection container.
- Do not parse `argv`, YAML or JSON inside plugins. Use `run_daemon(...)` for
  normal foreground daemons, or use `forge_config_program_options`, `forge_codec_yaml` and
   `forge_codec_json` before `application_shell::async_configure(...)` in coroutine
   hosts (`configure(...)` is the blocking boundary for external threads).
- Do not put security authority into UI/events/diagnostics. They are
  observability surfaces, not permission boundaries.
- Do not invent hook names that repeat the application context. The shell
  context already says this is application code.

## Public Modules

- `forge.app.application_shell` — production app shell and hook contexts.
- `forge.app.application_builder` — convenience builder that creates an
  `application_shell`.
- `forge.app.connect_context` — the bounded application composition phase for
  consuming local APIs and publishing connected services.
- `forge.app.service_registry` — exact-type one-shot service publication and
  immutable `service_view` lookup.
- `forge.app.daemon` — foreground daemon runner that loads YAML, explicit `.env`,
  process env and CLI, merges defaults and handles help/check/print/configure
  actions.
- `forge.app.runner` — foreground lifecycle runner with signal policy.
- `forge.app.application` — lower-level `application_runtime`.
- `forge.app.plugin`, `forge.app.plugin_context`, `forge.app.plugin_registry`.
- `forge.app.plugin_lifecycle` — shared transitions and read-only observation.
- `forge.app.application_environment` — immutable host paths and profile.
- `forge.app.events`, `forge.app.diagnostics`.

Target: `forge_app`.

Dependencies: `forge_asio`, `forge_config_core`, `forge_codec_yaml`, `forge_config_env`,
`forge_config_program_options`, `forge_log`, Boost headers.

## Stability

The scheduler and compute integration in `application_shell_options`,
`application_builder`, `application_context` and `application_shell` is Preview
in Forge 8.x. MINOR releases may make documented source-level changes to that
surface. The remaining App public contracts are Stable unless their owning
documentation marks them otherwise.

The current maintainer-approved App lifecycle clean break is documented in the
[migration note](../../docs/releases/unreleased-app-lifecycle.md). It changes
source interfaces only; no network or persisted format changes.

## Examples

The examples below show the intended production shape: app-owned config stays
in the shell, plugin-owned config stays in plugins, and lifecycle calls remain
shell-owned.

## Production Shape

`application_shell` makes lifecycle methods non-overridable. Inside an existing
coroutine, configuration must be awaited, not run through the blocking wrapper.
This shows the happy-path order, not a replacement for the runner's failure cleanup:

```cpp
boost::asio::awaitable<void> run_configured_app(
   forge::app::application_shell& app,
   const forge::config::core::document& document) {
   co_await app.async_configure(document);
   co_await app.initialize();
   co_await app.startup();
   app.request_stop();
   co_await app.shutdown();
}
```

Derived applications only implement hooks:

- `on_describe_config(registry&)`
- `on_configure(configure_context&)`
- `on_register_plugins(plugin_registry&)`
- `on_provide(application_context&)`
- `on_connect(connect_context&)`
- `on_after_initialize(const application_context&)`
- `on_run_foreground()`

This is deliberately strict. The application controls composition, but FORGE controls
the order: collect config, configure app and plugins, provide app APIs, let
plugins provide APIs, initialize every plugin, run every plugin
`after_initialize()`, connect application services, run the application
`on_after_initialize(...)`, startup plugins, request stop, shutdown in reverse
order and finally destroy connected services. The application callback receives
a const context: it may consume installed APIs through `api_view()` and services
through `service<T>()`, but it cannot install APIs or publish services after
plugin initialization.

`service_view` is copyable. The registry synchronizes publication, lookup,
close and clear, so an initialization task may read while `on_connect(...)`
publishes. Use `get<T>()` or `connect_context::service<T>()` for required
dependencies. Use `try_get<T>()` or `connect_context::try_service<T>()` for an
optional dependency; they return an empty `shared_ptr` when the type is absent
without using exceptions for control flow. Calling `get<T>()` on a detached,
default-constructed view throws typed `exceptions::service_registry_detached`;
`try_get<T>()` returns empty. A view does not own the registry and must not
outlive the `application_shell` that produced it.

## Complete Builder Example

The following three files form a complete foreground daemon, not pseudocode.
They are compiled by `forge_app_readme_example`; the focused
`test_forge_app_readme_example` uses the same implementation and factory.
Link the leaf target `forge_app` (installed consumers: `Forge::forge_app`).

The application owns `service_config`. The small product-owned status plugin
owns `status_config` and publishes an immutable, process-local batch limit.
The connect callback consumes that API, caps the requested batch size and
publishes `service_settings` before plugin startup. This example has no
background jobs or external resources; plugins that own such resources must
cancel and join them in their protected cleanup hooks. App already records
canonical lifecycle state and history; plugins must not duplicate it.

Declarations come first. Reflection and schema/API metadata stay together at
the bottom of the declaration file. The local-only API uses the current
`contract<..., surface::local>` and `FORGE_API` identity metadata; it has no
remote binding or generated wire methods.

[readme_example.hxx](../../tests/app/readme_example.hxx):

<!-- app-readme-example: readme_example.hxx -->
```cpp
#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

import forge.app.application_builder;
import forge.app.application_environment;
import forge.app.application_shell;
import forge.app.daemon;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.dispatcher;
import forge.config.core.component;
import forge.schema.object;

namespace service_example {

struct service_config {
   std::uint16_t batch_size = 16;
};

struct status_config {
   std::uint16_t batch_limit = 32;
};

struct service_settings {
   std::uint16_t batch_size = 16;
   std::filesystem::path data_path;
   std::string profile;
};

class status_api : public forge::api::core::contract<status_api, forge::api::core::surface::local> {
 public:
   virtual ~status_api() = default;
   [[nodiscard]] virtual std::uint16_t batch_limit() const noexcept = 0;
};

class status_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] static forge::app::plugin_descriptor descriptor();
   forge::app::plugin_id id() const override;
   std::string version() const override;
   std::optional<forge::config::core::component_descriptor> describe_config() const override;

 protected:
   boost::asio::awaitable<void> on_configure(forge::config::core::component_view view) override;
   boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> on_startup() override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   class api_impl;
   std::uint16_t batch_limit_ = 32;
};

class status_plugin::api_impl final : public status_api {
 public:
   explicit api_impl(std::uint16_t batch_limit);
   std::uint16_t batch_limit() const noexcept override;

 private:
   const std::uint16_t batch_limit_;
};

std::unique_ptr<forge::app::application_shell> make_application(const forge::app::daemon_context& context);
forge::app::daemon_options daemon_options();

BOOST_DESCRIBE_STRUCT(service_config, (), (batch_size))
BOOST_DESCRIBE_STRUCT(status_config, (), (batch_limit))

} // namespace service_example

template <> struct forge::schema::rules<service_example::service_config> {
   static forge::schema::object_schema<service_example::service_config> define() {
      auto schema = forge::schema::object<service_example::service_config>();
      schema.field<&service_example::service_config::batch_size>("batch-size").default_value(16).range(1, 64);
      return schema;
   }
};

template <> struct forge::schema::rules<service_example::status_config> {
   static forge::schema::object_schema<service_example::status_config> define() {
      auto schema = forge::schema::object<service_example::status_config>();
      schema.field<&service_example::status_config::batch_limit>("batch-limit").default_value(32).range(1, 64);
      return schema;
   }
};

FORGE_API(::service_example::status_api, FORGE_API_CONTRACT("service.status", 1, 0))
```

[readme_example.cpp](../../tests/app/readme_example.cpp):

<!-- app-readme-example: readme_example.cpp -->
```cpp
#include "readme_example.hxx"

#include <algorithm>
#include <utility>

import forge.app.connect_context;
import forge.config.core.decode;

namespace service_example {

forge::app::plugin_descriptor status_plugin::descriptor() {
   return {
      .id = {"status"},
      .factory = [] { return std::make_unique<status_plugin>(); },
   };
}

forge::app::plugin_id status_plugin::id() const {
   return {"status"};
}

std::string status_plugin::version() const {
   return "1";
}

std::optional<forge::config::core::component_descriptor> status_plugin::describe_config() const {
   return forge::config::core::describe_component<status_config>("status");
}

boost::asio::awaitable<void> status_plugin::on_configure(forge::config::core::component_view view) {
   batch_limit_ = view.get_or<std::uint16_t>("batch-limit", 32);
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<status_api>(std::make_shared<api_impl>(batch_limit_));
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_initialize(forge::app::plugin_context&) {
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_startup() {
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_shutdown() {
   co_return;
}

status_plugin::api_impl::api_impl(std::uint16_t batch_limit) : batch_limit_{batch_limit} {}

std::uint16_t status_plugin::api_impl::batch_limit() const noexcept {
   return batch_limit_;
}

std::unique_ptr<forge::app::application_shell> make_application(const forge::app::daemon_context& context) {
   auto settings = std::make_shared<service_settings>();
   settings->data_path = context.environment.resolve_data_path("service");
   settings->profile = context.environment.profile();

   auto builder = forge::app::application_builder{};
   builder.name(context.shell.name)
      .environment(context.environment)
      .runtime(context.shell.runtime)
      .scheduler(context.shell.scheduler)
      .config<service_config>("service", [settings](const service_config& config) {
         settings->batch_size = config.batch_size;
      })
      .connect([settings](forge::app::connect_context& connect) {
         const auto status = connect.api<status_api>();
         settings->batch_size = std::min(settings->batch_size, status->batch_limit());
         connect.publish(settings);
      })
      .plugin(status_plugin::descriptor());
   if (context.shell.compute) {
      builder.compute(*context.shell.compute);
   }
   return std::move(builder).build();
}

forge::app::daemon_options daemon_options() {
   return {
      .name = "service",
      .display_name = "Service daemon",
      .default_data_dir_name = "service",
      .env_prefix = "SERVICE",
   };
}

} // namespace service_example
```

[readme_example_main.cpp](../../tests/app/readme_example_main.cpp):

<!-- app-readme-example: readme_example_main.cpp -->
```cpp
#include "readme_example.hxx"

int main(int argc, char** argv) {
   return forge::app::run_daemon(service_example::make_application, argc, argv, service_example::daemon_options());
}
```

The factory forwards the daemon's immutable environment, runtime and scheduler
options, plus compute options when present. It does not reconstruct host paths
or invent a second application-options type. Callback state is captured by
value through `shared_ptr`, not by reference to factory-local variables.

`environment()` is a const host-facts view, not a plugin configuration store.
A relative `data-dir` is made absolute and frozen during daemon bootstrap;
`resolve_data_path(...)` uses that base even if the working directory changes.
Without an explicit base, relative path resolution fails with typed
`config_failed`. The environment does not create directories, resolve symlinks
or provide a filesystem sandbox. Plugins receive the same immutable environment
through their context.

## App And Plugin Config

The shell merges defaults from all registered descriptors before configuring
the application and plugins. Typed component settings remain plugin-owned;
shared host paths/profile remain in `application_environment`.

Coroutine hosts use `co_await shell.async_configure(document)` on their existing
runtime. It owns the input document and follows the same defaults, validation
and created-state requirements as `configure(document)`. The synchronous method
is the blocking convenience boundary for external callers; never call it from
a runtime worker. Configuration is one-shot, not concurrent reconfiguration.

The complete example separates the plugin's settings section
`status.batch-limit` from its shell-owned selection key
`plugins.status.enabled`. All `on_*` plugin hooks are protected; guarded public
lifecycle entrypoints are not extension points.

## Plugin Enable/Disable

Every registered descriptor gets a shell-owned selection key. For the example:

```yaml
service:
   batch-size: 48
status:
   batch-limit: 8
plugins:
   status:
      enabled: true
```

The resulting connected service has batch size 8. Its required status API means
disabling `status` makes the connect phase fail and triggers cleanup; selection
does not magically make a required dependency optional.

For the official HTTP server, the owning leaf is `net.http.server`, not
`net.http`:

```yaml
plugins:
   net:
      http:
         server:
            enabled: true
```

That YAML applies only when the application registers
`forge::plugins::net::http::server::plugin::descriptor()`; HTTP is not a
dependency of this minimal example.

The default comes from `plugin_descriptor.enabled_by_default`. Disabled plugins
are not configured, initialized, started or shut down. If a descriptor-declared
dependency is disabled, selection fails before lifecycle side effects.
Use typed API/service optional lookup only when application policy actually
permits the capability to be absent.

## Derived Shells And Embedded Hosts

Use a derived `application_shell` only when substantial state or customization
cannot be expressed cleanly through builder callbacks. Put its `on_*` overrides
under `protected:` and pass real `application_shell_options` to the base
constructor. Do not duplicate shell-owned runtime, diagnostics, contexts or
registries.

`run_daemon(...)` is the production foreground entrypoint above.
`run_application(...)` is the lower-level runner for hosts that already own a
merged document. It centralizes stop and cleanup on both success and failure;
destruction is not async cleanup. For example, using the same factory:

```cpp
#include "readme_example.hxx"

#include <utility>

import forge.app.runner;
import forge.config.core.document;

int run_embedded_service() {
   auto context = forge::app::daemon_context{};
   context.shell.name = "embedded-service";
   context.environment = forge::app::application_environment{"./service-data", "embedded"};
   context.shell.environment = context.environment;

   auto document = forge::config::core::document{};
   document.set("service.batch-size", 8U);
   return forge::app::run_application(service_example::make_application(context), document);
}
```

A custom host may set `run_options::wait_for_stop` to its own awaited stop
operation and disable the two OS signal options. Do not replace the runner
with an unowned detached task. `startup()` initializes automatically when needed;
tests may initialize explicitly to inspect API publication before startup.

## Optional Compute Pool

The shell creates no CPU worker pool by default. Applications that run bounded
synchronous CPU work opt in through the builder or shell options:

```cpp
auto builder = forge::app::application_builder{};
builder.name("service")
   .compute(forge::asio::compute::pool::options{
      .worker_threads = 4,
      .max_pending_tasks = 256,
   });
auto app = std::move(builder).build();
```

`application_context`, `plugin_context` and `application_shell` expose
`has_compute()` and a copyable `compute()` executor. Plugins receive the
executor; they do not own or stop worker threads. During shutdown, plugins run
first, then the shell requests scheduler stop so no queued work can start after
plugin shutdown. The compute pool drains while scheduler continuations that
were already running can still finish, then the task scheduler drains, and only
then may the Asio runtime stop. Scheduler drain is awaited rather than blocking
a runtime worker, so active awaitable tasks can finish even with the default
single-worker runtime. This keeps compute available for final plugin cleanup
without detached execution or a scheduler/compute shutdown deadlock.

## Daemon Runner

`run_daemon(...)` is the recommended entrypoint for ordinary foreground
daemons. It owns the boring but risky glue that products otherwise rewrite:
collect app and plugin descriptors, load YAML, load an explicit `.env`, read
process environment, parse daemon and app/plugin CLI, merge defaults, handle
`--help`, `--check-config`, `--print-effective-config` and `--configure`, then
call `run_application(...)`.

### Logging shutdown ownership

`run_daemon(...)` owns configured process-wide logging by default. It finishes
application cleanup, final runner/daemon records and owned application/factory
destructors before calling `forge::async_shutdown_logging()`. This applies to
normal execution, bootstrap/startup/shutdown failures and CLI-only exits such as
`--help` and `--check-config`. No extra shutdown call is needed in a normal
daemon's `main()`; logging macros are unchanged.

`daemon_options::logging_shutdown_timeout` is a separate nonnegative budget,
defaulting to five seconds. Zero checks the delivery fence without waiting;
negative values are rejected. Failure to drain increments `log_failure_count()`
and turns a successful exit into code `1`, while preserving an existing nonzero
code or exception. The failure path does not log or write to stderr, which may
itself be the blocked output. A completed fence means accepted records have
finished delivery attempts, not that every output succeeded or reached durable
storage; inspect `logging_delivery_metrics()` for failed/dropped records.

An owning daemon retains the application until cleanup completes, even if the
application shutdown budget expires. It then reports the timeout as failure.
Neither budget can forcibly terminate non-cooperative C++ callbacks. The logging
budget bounds the drain wait, not process termination: a blocked callback can
still delay the logging owner's final worker join.
The budget starts after the daemon body returns or unwinds. Explicit CLI results
(`--help` and `--print-effective-config`) still use synchronous stdout and can
block before this boundary. They are command results, not log records.

Daemon operational and early configuration errors use the `forge.app.daemon`
logger exclusively. Before application logging configuration exists, the logger
uses its existing lazy default: a bounded asynchronous console sink on stderr.
The daemon does not reconfigure shared routes or synchronously print a fallback.
Configured routes, levels, queue limits and delivery metrics apply to these
diagnostics just like other logs. A disabled/failed/saturated sink can lose a
diagnostic but cannot change the original failure into success. Diagnostic fields
contain safe codes, recognized schema paths and reviewed reasons, not raw argv,
config values, filesystem paths or callback exception text. Early records are not
replayed to an OTLP exporter initialized later.

This removes synchronous daemon diagnostic writes, not arbitrary blocking from
user-supplied synchronous sinks or final joins of non-cooperative sink workers.
The logging timeout remains a delivery-wait budget, not a hard process deadline.

`application_shell::shutdown()` and both `run_application(...)` overloads do
**not** close process-wide logging. Several shells can share it safely. An
external process host using repeated, nested or concurrent `run_daemon()` calls
must set `daemon_options::shutdown_logging = false` for those calls and perform
one final logging shutdown after all its producers have stopped. In this mode,
the existing owning runner's asynchronous cleanup-after-timeout behavior is
unchanged. Do not mix a process-owning daemon with independently running shells;
the daemon cannot infer external producers' lifetimes. Configured logging is
not restartable after its final shutdown.

OTLP and directly attached sinks keep their own lifecycle. The daemon drains
the configured async console/file/JSONL owner, not arbitrary external exporters.

The complete factory and thin `main()` are shown above; no additional options
type or manual signal bridge is needed.

The factory is intentional. `application_shell` creates its runtime and
scheduler in the constructor, so early daemon config such as
`daemon.runtime-threads`, `daemon.scheduler-queue-depth`, `daemon.data-dir` and
`daemon.profile` must be known before the application object exists.
For runtimes with two or more workers, the daemon derives the scheduler blocking
budget as `runtime-threads - 1`, leaving one worker available for timers and
awaitables. Single-thread mode remains valid for minimal daemons, but blocking
tasks that wait on the same scheduler can deadlock there.

Built-in daemon YAML:

```yaml
daemon:
   profile: dev_local
   data-dir: ./service-data
   runtime-threads: 2
   scheduler-queue-depth: 4096
   shutdown-timeout-ms: 10000

service:
   batch-size: 16
status:
   batch-limit: 32

plugins:
   status:
      enabled: true
```

The merge order is fixed:

```text
schema defaults < daemon defaults < YAML < .env < process env < daemon CLI < app/plugin CLI
```

Daemon bootstrap flags are always parsed: `--help`, `--profile`,
`--data-dir`, `--config`, `--dotenv`, runtime/scheduler flags and action flags.
Individual source adapters can be disabled with `daemon_options::read_yaml`,
`read_dotenv`, `read_process_env` and `read_cli`. An empty `env_prefix`
disables both `.env` and process env sources; `run_daemon(...)` never searches
parent directories for `.env`.

`--print-effective-config` and `--configure` write redacted documents using the
same registry. Secret fields from schema descriptors are never printed as real
values.

## Process Signals

`run_daemon(...)` delegates SIGINT/SIGTERM waiting to the lifecycle runner.
`request_stop()` is synchronous and `noexcept`, but it signals intent rather
than joining cleanup. Those properties do not make it POSIX async-signal-safe;
do not call it from a raw signal handler. An embedding host should await its
own stop notification through `run_options::wait_for_stop`, then let the runner
request stop and finish shutdown while the owner remains alive.

## APIs

Plugin-to-plugin contracts use `forge_api_core`: the application can publish
implementations during the `on_provide(...)` phase, plugins can publish
implementations during their `provide(...)` phase, and runtime consumers receive
a read-only API view through `application_context::api_view()`. This keeps
lifecycle in `forge_app` and contract/version/error semantics in
`forge_api_core`.

The complete example installs its local-only `status_api` during protected
`on_provide(...)` and consumes it during builder `connect(...)`. The API owns an
immutable value, not a pointer back into plugin resources. More substantial
resource-backed APIs must keep their owner alive and preserve stop admission;
an API handle alone is not permission to access already-released resources.

Infrastructure plugins should likewise publish narrow typed capabilities
instead of transport/runtime internals. See [API Core](../api/core/README.md)
for contract, method and remote-binding examples.

The private implementation declaration lives in its exact `details/*.hxx`
pair; consumers only see the published API interface.

Do not use APIs as lifecycle modules. A plugin owns behavior/lifecycle; an API is
the typed contract exposed by that plugin or application.

## Events And Diagnostics

Diagnostics reads the same lifecycle objects that guard transitions. A failed
operation is separate from the phase: a failure does not mean cleanup finished.
The journal is bounded diagnostic history, not an event-delivery or command bus.
Plugin contexts expose diagnostics through a const pointer; only the host owner
binds or clears the observer registry.

```cpp
auto snapshot = app.diagnostics().snapshot(app.events());
for (const auto& plugin : snapshot.plugins) {
   if (plugin.lifecycle.failure) {
      report(plugin.id, plugin.lifecycle.failure->operation);
   }
}
```

Observe lifecycle changes asynchronously instead of subscribing to callbacks:

```cpp
#include <chrono>

import forge.app.plugin_lifecycle;

// Inside an existing coroutine, with the app owner kept alive by the host:
auto observer = app.lifecycle();
const auto before = observer.snapshot();
const auto after = co_await observer.async_wait_until(
   before.epoch, std::chrono::steady_clock::now() + std::chrono::seconds{2});
```

Waits may time out or be canceled, and changes may coalesce. A terminal snapshot
(`stopped` or `owner_alive == false`) returns immediately even at its current
epoch; owner destruction is not proof of successful cleanup. Observers confer
no transition authority or resource-lifetime guarantee.

Plugin configure/provide/initialize inputs are one-shot: repeated or concurrent
calls reject new inputs. After-initialize/startup/shutdown callers join an
accepted attempt or skip an already successful hook. `stopped` is terminal for
that owner; a new run needs a new instance. Historical failure may remain visible
after successful shutdown. See [Runtime + App](../../docs/runtime/asio-app.md)
for the shared lifecycle and admission contract.

## Lifecycle Logging

Lifecycle records use named loggers `forge.app.application`, `forge.app.plugin`,
`forge.app.runner` and `forge.app.daemon` through normal Forge macros and configured
sinks. Logging is observational: sink failures cannot veto transitions or cleanup.
Records carry bounded operation/phase metadata and numeric error codes, not raw
configuration, argv, exception text, secrets or automatic path/profile dumps.

Early startup may precede OTLP exporter initialization; late cleanup may follow
its teardown. Local/configured sinks follow normal routing, but whole-process
remote delivery is not guaranteed. App neither owns an exporter nor keeps it
alive solely for lifecycle logs. Exporter drain and collector verification are
separate concerns; see the [donor note](../../docs/donors/app-lifecycle-observability.md).

## Startup Failure And Rollback

Plugin order comes from `plugin_registry` dependencies. Initialize,
`after_initialize` and startup run in that order; shutdown runs in reverse
order. Configure/provide/initialize/after-initialize/connect/startup failure
requests stop and cleans up partially configured or initialized owners, including
the failing plugin, not just plugins that initialized successfully. Cleanup hooks
must tolerate never-initialized resources. Successful rollback rethrows the
original failure; failed rollback reports `shutdown_failed` with the cleanup
exception nested.

Reverse cleanup stops at a failed dependent and retains its dependencies for
retry; already-stopped plugins are skipped. Cleanup failure leaves `stopping`,
not `stopped`. On plugin cleanup failure, keep the same owner, scheduler, compute
pool and connected services alive for another shutdown attempt; the shell
releases these facilities only after plugin cleanup succeeds.


The app should still call `request_stop()` and `shutdown()` from the outer
entrypoint cleanup path. Stop closes admission without waiting for cleanup;
concurrent shutdown callers join the accepted cleanup attempt, successful
repeats run no hook again, and a failed cleanup may be retried. Do not destroy
the owner while cleanup is pending or failed.

The cleanup path is not optional ceremony. It keeps failed startup paths from
leaving APIs, background jobs or plugins in half-started states. Prefer
`run_daemon(...)` for normal foreground daemons, or `run_application(...)` for
hosts with an already merged config document; both centralize that flow.

## Lower-Level Escape Hatch

`application_runtime` remains available when a host framework already owns
runtime, API registry and diagnostics. That is an escape hatch, not the normal
daemon pattern. Prefer `application_shell` or `application_builder` for new
services.


## Typical Mistakes

- Do not copy shell-owned members into every application.
- Do not use `application_builder` to define another lifecycle; it only creates
  a shell.
- Do not manually instantiate plugins in application startup code when
  `application_shell` can own the registry.
- Do not use APIs as fake plugins. Plugins own lifecycle and behavior; APIs
  expose typed contracts.
- Do not configure plugin options from `build_plugins()`; plugin config belongs
  to `plugin::describe_config()` and `plugin::configure(component_view)`.
- Do not parse `argv` or backend parser objects inside plugins.
- Do not wrap `run_daemon(...)` with another generic config framework. Application
  code should define typed config structs and plugin descriptors, not another
  merge engine.
- Do not put secrets into events or diagnostics without redaction first.
- Do not assume `request_stop()` waits for cleanup; it only requests shutdown.
- Do not stop the scheduler or `io_context` from a stop callback or hook.
  Plugins may still need async cleanup work during `shutdown()`.
- Do not assume failed `initialize()` or `startup()` leaves the shell stopped.
  Complete or retry shutdown before destroying it; another startup attempt
  requires a new instance after cleanup.
- Do not use `abort()` from signal handlers or lifecycle catches. Request stop,
  run shutdown at the application boundary, then return an error.

## Runtime Risks And Anti-Patterns

- A plugin that starts background work must own cancellation and shutdown.
  Hidden detached work is a process-lifetime leak.
- APIs must be provided before plugins initialize and must outlive plugin
  shutdown. Removing APIs early creates use-after-shutdown failures.
- App events have no subscriptions or callbacks. Use read-only lifecycle
  observers for changes; a copied observer is not ownership of plugin resources.
- Events and diagnostics are observability surfaces, not recovery. Log/report
  them, but still propagate correctness-path failures to the caller.
- Lower-level `application_runtime` users must preserve the same lifecycle
  ordering as the shell. If they cannot, they should use `application_shell`
  instead of reimplementing daemon plumbing.

## Tests

`test_forge_app` covers API publication, event journal bounds, plugin dependency
order, config collection, shell-owned default merge, configure-before-initialize,
after-initialize ordering and failure cleanup, startup rollback, reverse shutdown
and diagnostics.

The complete example is compiled separately as `forge_app_readme_example`.
`test_forge_app_readme_example` exercises that same factory, schema/defaults,
local API consumption, immutable environment, coroutine configuration on one
runtime worker, required-plugin selection and runner cleanup after a wait
failure. It links only `forge_app`, not the whole FORGE umbrella.

`test_forge_app_readme_correspondence` runs
[check_readme_example.py](../../tests/app/check_readme_example.py). It compares
the three marked README blocks and the runtime document's `main()` block exactly
against the fixture files. This is a small App-specific drift guard, not a
general documentation framework or a substitute for compiling/running tests.

Acceptance targets: `forge_app_readme_example` and
`test_forge_app_readme_example`; CTest names:
`test_forge_app_readme_example` and `test_forge_app_readme_correspondence`.
These names describe executable coverage, not a claim that any build or test
has passed.
