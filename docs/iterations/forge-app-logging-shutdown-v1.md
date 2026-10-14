# Automatic Daemon Logging Shutdown

## Scope

Complete the App integration of [bounded logging](forge-log-nonblocking-v1.md).
Logging macros, wire formats and project version remain unchanged. Only
`run_daemon` acquires the process-shutdown responsibility by default; shell and
embedded runner lifecycles retain their existing ownership.

## Contract

- `daemon_options::shutdown_logging` defaults to `true`. External process hosts
  set it to `false` and arrange one final shutdown after all producers stop.
- `logging_shutdown_timeout` defaults to five seconds, rejects negative values,
  and treats zero as a nonwaiting fence check, not an unlimited wait.
- The daemon body releases the application and its owned callback captures
  before draining. Early CLI exits and exceptional exits use the same wrapper.
- Owning daemons use the existing borrowed runner overload to await cleanup
  even after the app budget expires. This prevents a background cleanup tail
  from producing logs after admission closes. External-owner mode preserves
  the owning runner's existing timeout behavior.
- A short-lived Forge Asio runtime awaits the existing logging shutdown API.
  It does not replace the dedicated logging compute worker, create another
  delivery queue or execute output callbacks on an I/O worker.
- A failed drain changes only a successful exit to code `1`; existing nonzero
  results and exceptions survive. The failure counter is incremented without
  recursive logging or a synchronous stderr fallback.
- A deadline cannot kill arbitrary C++ code. App cleanup and final process
  worker joins may still wait on a non-cooperative callback. Delivery completion
  is not a durability guarantee; output failures remain visible in metrics.
- Existing synchronous CLI stdout/stderr output is unchanged. It can block
  before the daemon body returns; the logging budget is not a whole-run timeout.
  Only the new drain-failure path guarantees no synchronous error output.

## Acceptance

Focused validation covers normal/nonzero exit, configuration/startup/shutdown
failures, early CLI exits, destructor records, cleanup past the app deadline,
logging saturation/timeouts and explicit external ownership. Multiple shell and
both embedded runner overloads must leave shared logging active.

Automatic ownership is exercised in fresh subprocesses, checking closed state,
empty queues and delivered records before process exit. Static destruction must
not be allowed to disguise a missing drain. Concurrent tests use barriers and
bounded waits. Installed App consumers, structure checks, unchanged-macro
comparison and two independent exact-head reviews complete the acceptance gate.

No full CI, forced callback termination or production readiness claim is added.

## Native Acceptance Receipt

Validated on macOS arm64 with Homebrew LLVM 22.1.8 and CMake 3.31.6/Ninja,
using one coordinator build tree and `-j4`:

- Thirteen focused App, Log, Asio, OTLP, README and structure suites passed.
- The daemon fixture contains twenty process-isolated cases. It checks counters
  before exit, captured factory/waiter destructors, two concurrent shells,
  cleanup held past the app deadline and real unread stderr pipes. Zero-budget,
  nonzero-exit and original-exception behavior are covered.
- App, Log and the daemon fixture each passed twenty consecutive repetitions,
  including four hundred daemon subprocess cases.
- The installed `app` consumer imports its directly used `log` API through an
  explicit owning target and verifies automatic close and delivery counters.
- Logging macros and shell/embedded-runner implementations are byte-identical
  to the base. `git diff --check` passed.

Raw coordinator logs are retained under the local ignored acceptance directory
`.cache/acceptance/app-logging-shutdown-20261014/`. This is native focused
evidence, not a Linux/Windows, sanitizer, full-CI or durable-export claim.
The interactive pipe fixtures explicitly skip their POSIX-only cases on Windows.
