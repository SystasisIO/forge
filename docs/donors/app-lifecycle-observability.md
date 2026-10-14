# App Lifecycle Observability

Status: implemented and locally validated, 2026-10-14. The scoped build, runtime,
package, sanitizer and review results are recorded in the
[acceptance receipt](../iterations/forge-app-lifecycle-v1-acceptance.md).
This donor note alone is not evidence of production readiness.

## Inspected Revisions

Only existing local sources were read. No donor was fetched, cloned or built.
Checkout locators below are relative to the workspace Projects directory, not
machine-specific absolute paths.

| Donor | Checkout | Revision and evidence boundary |
| --- | --- | --- |
| Spring | `Storlane/spring` | `e6a99f68b67abc4d89fe716755b2e1394a4991f7`; inspected `programs/nodeos/main.cpp` is clean against HEAD. |
| Spring Appbase | `Storlane/spring/libraries/appbase` | `74ac065ddacc46b28318f46965b6686976e84e3c`; matches Spring's gitlink; inspected files are clean against this HEAD. |
| EOSIO | `Storlane/eos` | `11d35f0f934402321853119d36caeb7022813743`; inspected `programs/nodeos/main.cpp` is clean against HEAD. |
| EOSIO Appbase | EOSIO gitlink only | `144b2e239d6fd93a8336543bf9eda7c52ea8c77e`; source checkout is absent. Its implementation was NOT inspected or inferred from Spring. |
| Local BitShares/Graphene-derived code | `Storlane/bitstore-core` | `738940ea7a93a342ad9d0883d4595ced3258470b`; inspected pinned Git blobs via `git show`, NOT dirty working files. This is a renamed local derivative, not a verified exact upstream BitShares-core revision. |

A bounded search of the Projects tree to depth three found no separately named
BitShares checkout. The local derivative README references BitShares-core and
its wiki; that establishes provenance context, not an upstream commit mapping.
`Projects/BitStore` is an alias of the Storlane workspace, not another checkout.

Exact files and ranges used:

- Spring `programs/nodeos/main.cpp:32-69,128-141,155-254`: selected option masking,
  logging initialization, process startup/version/config context, interruption
  and exit/error reporting.
- Spring Appbase `application_base.cpp:163-174,351-410,426-445`: startup rollback,
  configuration warnings, logging callback before plugin initialization, reverse
  shutdown and exception reporting.
- Spring Appbase `include/appbase/application_instance.hpp:25-55`: final plugin
  entrypoints, registration before startup hook and eager phase assignment.
- Spring Appbase `include/appbase/application_base.hpp:103-159,303-334` and
  `include/appbase/abstract_plugin.hpp:25-40`: initialization/execution and plugin
  lifecycle boundaries.
- EOSIO `programs/nodeos/main.cpp:81-95,108-179`: logging setup after initialization,
  version/config location logging and explicit process exit/error outcomes.
- Local derivative `libraries/app/application.cpp:285-345,1156-1221,1243-1247,
  1430-1463`: configuration/setup context, teardown stages, before/after plugin
  hook logging, application startup/error logging and destructor shutdown.
- Local derivative `libraries/app/config_util.cpp:76-117,229-245,248-253`: default
  logger levels/routes, logging-configuration fallback and configuration creation.
- Local derivative `programs/witness_node/main.cpp:170-246`: configure/init/start
  flow, normal stop signals and process exit/error reporting.
- Local derivative `README.md:8-15`: BitShares provenance references.

## Accepted And Rejected Patterns

Accepted:

- Initialize logging before resource/plugin initialization. Spring Appbase calls
  its supplied initializer at `application_base.cpp:369-371`; EOSIO's older
  nodeos initializes its logging later, at `main.cpp:135`.
- Log significant application startup and teardown progress at INFO. The local
  derivative surrounds successful application hooks with INFO records and names
  teardown stages before closing dependencies.
- Keep a process-boundary record for version/run/exit and a separate typed
  operational failure outcome. Do not confuse an interrupted start with ready.
- Preserve cleanup ownership for partially started resources: Spring registers
  a plugin before invoking its startup hook (`application_instance.hpp:42`).
- Keep INFO as the normal application default and DEBUG detail opt-in. The local
  derivative's logger configuration distinguishes routine application logging
  from noisier transport-specific routes.

Rejected for Forge:

- Copying per-plugin before/after records into new App logs. Forge's guarded
  [plugin entrypoints](../../libraries/app/plugin.cpp) already own these logs.
  App must not duplicate them in logs or another per-plugin history table. The
  existing bounded journal is retained only as non-authoritative diagnostic
  history, with no increase in coverage and no signaling/control role.
- Logging arbitrary option values with a hand-maintained sensitive-key denylist.
  Spring masks selected credentials, but forwards other option values; Forge
  accepts no raw config/options/argv/environment payload in lifecycle logs.
- Logging `what()`, detailed exceptions, diagnostic dumps, secrets or key material.
  Donor code contains these patterns; they are provenance evidence, not a
  redaction policy. Also omit automatic absolute data/config paths and endpoints.
- Setting started/stopped before a hook completes. Spring's template does this;
  Forge's shared lifecycle is authoritative and must never log false success.
- Warning on normal SIGINT/SIGTERM merely because the older derivative does.
  An orderly requested stop is INFO, not WARNING or ERROR.
- Adopting destructor-driven cleanup or unconditional continue-through-failure
  teardown from a donor. Forge requires explicit async cleanup and preserves a
  failing dependent's resources for retry; observability does not change that.
- Creating another logger/scheduler, OTLP owner, generic event bus or lifecycle
  state table. Normal Forge sink routing remains the only logging mechanism.

## Implemented Taxonomy

Use existing `tlog/dlog/ilog/wlog/elog` macros. Stable event names and canonical
operation/phase values are structured fields; prose messages are constant.

| Level | App/Process event | Emission rule |
| --- | --- | --- |
| TRACE (`tlog`) | optional diagnostic wait/admission detail | Off by default; no per-wait INFO and no unbounded observer trace. Not required for this slice. |
| DEBUG (`dlog`) | `app.operation.begin`, `app.operation.complete` | Actual operation owner only. Configure/provide/initialize/after_initialize completion belongs here, including accepted configuration. |
| INFO (`ilog`) | `app.ready`, `app.stop.requested`, `app.cleanup.complete` | Ready only after accepted startup completion; stop only on first admission closure; cleanup only after successful shutdown. |
| INFO (`ilog`) | `runner.start`, `runner.exit` | One process-boundary record, separate from application operation completion. Exit does not imply cleanup succeeded if an owned timeout tail is still running. |
| DEBUG/INFO | `runner.signal`, `daemon.config.accepted` | Runner owns OS signal context; daemon summarizes successful input validation without values. No routine provide/after-initialize INFO. |
| WARNING (`wlog`) | `app.operation.canceled`, `app.cleanup.retry`, optional config/logging fallback | Only a typed interruption/recovery decision; normal stop and canceled observer/joiner are not application failures. Retry records only when an actual failed cleanup is attempted again. |
| ERROR (`elog`) | `app.operation.failed`, `runner.failed`, `daemon.config.failed` | Actual operation/process failure; numeric typed code, no exception text. A cleanup failure remains stopping, not stopped. |

Minimal fields:

- `event`: fixed bounded vocabulary above; never concatenate user input.
- `operation`: canonical configure/provide/initialize/after_initialize/startup/
  request_stop/shutdown or a fixed process/config stage name.
- `phase`: canonical lifecycle phase, using stable string or existing numeric
  representation; no logger-owned phase enum.
- `generation`, `epoch`: copied from the same canonical snapshot when relevant.
  Do not add independent counters for logs. Failure attempt identity may use the
  existing snapshot failure; do not fabricate a success attempt id.
- `error_code`: numeric Forge/Boost/std code; unknown exception gets a fixed
  sentinel. If needed, `error_category` is a fixed allowlisted classification,
  not arbitrary category text or exception context.
- Optional bounded counts: selected plugin count, validated source count.
  `exit_code`/signal number belong only to the runner. Plugin `id` stays with its
  owning wrapper, not repeated App records.

No raw config document, option value/key dump, credential, private key, token,
exception message, backend response, URI, configured resource/config/data path
or profile value is allowed in lifecycle fields. The standard logger still owns
source_location/thread/timestamp and ERROR stacktrace metadata; legitimate code
source paths in that metadata are not configured resource paths. Do not strip
or manually recreate standard logger metadata. No automatic config hash:
low-entropy secret values can be guessed from
such fingerprints. Product-specific context is outside this neutral taxonomy.

## Integration Ownership

- Shell owns App operation records when it owns the shared lifecycle token.
  A standalone application runtime emits the same records only for its own
  token; embedded private runtime stages must not produce a second completion.
- Begin/completion logging happens only after a real token is acquired, never
  for a coalesced caller or an already successful operation.
- Resource completion/error is published before observational logging. Log
  outside lifecycle/resource locks; catch logging/sink failures. Observers/sinks
  cannot delay admission closure or veto cleanup correctness.
- Failure during configure/init/start retains its typed cause. Cleanup failures
  are separately visible; successful rollback never turns failed startup into
  ready. Late startup success invalidated by stop is cancellation/failure, not
  a ready event.
- [runner](../../libraries/app/runner.cpp) owns process start/exit/signal and
  timeout reporting. Avoid duplicating successful App shutdown or per-plugin
  records. A timeout must not be logged as cleanup complete.
- [daemon](../../libraries/app/daemon.cpp) owns parsing/validation mode records
  before an App exists. Help/version/check-config/print-config exits are not
  application-ready events. Explicit output rendering is a separate boundary.
- The bounded event journal is diagnostic history only. The coordinator chose
  to retain existing per-plugin publication in
  [runtime](../../libraries/app/application.cpp); no new records/coverage are
  added by the logging slice.
- If cross-file helpers are required, use one exact private `.hxx/.cpp` pair
  containing logging mechanics, not a new public API or lifecycle framework.

## Current Implementation Boundary

App/standalone runtime logs only after acquiring real shared operation tokens;
embedded runtime stages do not duplicate shell completion. Intermediate
completion is DEBUG, accepted startup/cleanup is INFO, typed interrupted startup
is WARNING and cleanup errors remain ERROR. First stop before resource setup
is DEBUG, so ephemeral schema prototypes and successful help/check-config modes
do not resemble stopped live services. Runner adds process start/exit and signal
context; daemon adds validation/mode outcomes without logging input values.

The private `details/lifecycle_log.hxx` / `lifecycle_log.cpp` pair routes through
existing Forge macros/sinks. Runner shutdown owner/state and daemon parsed-CLI
records remain exact private header-only value records, not decorative compiled
sources. Logging introduces no new scheduler, transport, exporter or logging API.

CLI diagnostics and sink logs are separate boundaries. CLI may print fixed
reviewed schema validation reasons and known daemon option paths, including
bootstrap type errors and min/max validation, but never an arbitrary callback
exception dump. Legacy configure callbacks that forward a bare
`invalid_argument` are recognized only for exact fixed validation vocabulary;
other messages remain generic. Built-in daemon conversion uses existing Forge
config_failed with structured path context. Log sink records continue to omit
every diagnostic reason/input value, regardless of whether CLI can show it.

Cancellation provenance is typed: the shared lifecycle normalizes late success
to `exceptions::operation_stopped`, an invalid_state subtype in the single App
exceptions owner module, preserving existing code/catch compatibility. This
marker propagates through App/runner and can be logged as interrupted startup.
A hook's ordinary invalid_state remains ERROR even after rollback sets
stop_requested. Cleanup errors are never ordinary stop. No parallel failure flag
or message/late-phase heuristic is used.

Early startup logs may precede initialization of an optional OTLP exporter;
late cleanup logs may follow exporter teardown. Existing local/configured sinks
receive these records according to normal routing, but remote delivery for the
whole process lifetime is NOT guaranteed by this slice. App does not own OTLP
or keep it alive solely for lifecycle records. Exporter lifecycle/drain policy
and remote collector evidence are separate acceptance work; capture-sink tests
prove neither network delivery nor commercial/production readiness.

### Daemon Diagnostic Follow-Up

The [daemon diagnostic change](../iterations/forge-app-daemon-diagnostics-v1.md)
supersedes the separate CLI-error rendering boundary described above. Explicit
help/effective-config results remain stdout output; early and operational errors
use the same Forge logger routing. The existing lazy default async console is
reused, with no new bootstrap worker or process-wide reconfiguration.

Daemon diagnostic records may include a fixed diagnostic code, a schema-owned
canonical field path and an allowlisted reason. They never forward raw parser
messages, user-supplied paths/option names, config values or arbitrary exception
text. Descriptor-provided free-form migration messages are not treated as safe
logging vocabulary. Lifecycle classification remains typed and distinct from
these safe diagnostic details. This is a Forge output-policy refinement, not a
new donor compatibility claim; the earlier acceptance receipt is unchanged.

## Acceptance Gates

The following requirements are exercised by the scoped acceptance suite:

1. Capture-sink tests see exactly one App completion per actual operation under
   concurrent joined startup/shutdown and no duplicate runtime/shell records.
2. Startup/shutdown/first stop are INFO; intermediate provide/after-initialize
   completions are DEBUG. Normal orderly stop has no ERROR/WARNING.
3. Failure/retry tests retain stopping and the correct typed code; no ready or
   cleanup-complete event for failed/canceled/late completion or a timeout tail.
4. Inject secrets in configuration and exception message/context. Neither log
   messages nor any structured field contains them, raw options, paths or argv.
5. Throwing/reentrant sinks do not change operation results, cancellation,
   admission, resource rollback or deterministic successful shutdown.
6. Help/check-config/foreground paths keep their existing result and resource
   semantics; no new OTLP ownership, scheduler or detached logging worker.
7. Static inspection finds only Forge macros at App boundaries, no exception
   text/config dump/second state table, and exact private helper pairing.

The [plugin lifecycle tests](../../tests/app/plugin_lifecycle_tests.cpp) cover
terminal observer completion racing both stop-token and Asio cancellation,
coalesced shutdown errors/retry and throwing sink isolation. The
[App observability tests](../../tests/app/app_observability_tests.cpp) cover normal
run, joined operations, late stopped startup, secret omission, cleanup error/retry,
throwing/reentrant sinks, standalone runtime and daemon dry modes. The coordinator
executed these suites in the native, ASan and TSan runs listed in the acceptance
receipt; reviewers independently checked source ownership and evidence boundaries.
