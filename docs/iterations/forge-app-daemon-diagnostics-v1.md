# Daemon Diagnostic Output Ownership

## Scope

Follow up [automatic logging shutdown](forge-app-logging-shutdown-v1.md) by
removing direct stderr diagnostics from `run_daemon`. Preserve CLI stdout,
logging macros, exit codes, application lifecycle and shared-shell ownership.
No new logger runtime, emergency queue, thread, platform writer or public option
is introduced. The project version is unchanged.

## Contract

- Operational failures and early argument/configuration errors use Forge macros
  on `forge.app.daemon`. No diagnostic uses direct `std::cerr`/`std::clog`.
- Existing lazy logger initialization already supplies asynchronous stderr with
  bounded delivery. Daemon code neither replaces configured sinks nor creates
  an output fallback when logging fails or is disabled.
- Safe diagnostic codes, schema-owned paths and fixed reasons remain useful to
  operators. Raw input, parser messages, resource paths and arbitrary exception
  text must not leak into local or remote sinks.
- stdout remains command output for help and effective configuration. Rendering
  and secret redaction remain unchanged; log records do not replace CLI output.
- Error codes/exceptions are not swallowed by delivery failures. Existing
  `shutdown_logging=false` composition and automatic final drain are unchanged.
- Producer-side diagnostic emission and drain waits are bounded with the default
  asynchronous sink. Arbitrary synchronous sinks and non-cooperative workers
  retain their documented limitations: no hard process-exit deadline is claimed.

## Acceptance

- Verify early errors with a fresh process and no explicit logging setup.
- Capture configured structured records, asserting useful safe reasons and no
  direct stderr output. Include invalid CLI/config, missing files/factories,
  startup/shutdown failures and untrusted values/exception messages.
- Fill a real stderr pipe and do not read it until `run_daemon` returns. Exercise
  standard exceptions and early errors, not only normal/nonstandard exits.
  Then release the reader and prove complete cleanup; do not hide a worker join
  behind process termination or claim a permanently blocked sink can be killed.
- Preserve help/config stdout, nonzero codes, multiple shells and embedded
  logging ownership. Repeat concurrency/pipe cases with bounded harness waits.
- Run focused App/Log/Asio, package and structure checks with one `-j4` build;
  conduct two independent exact-head reviews before merging. No full CI.

The existing [donor note](../donors/app-lifecycle-observability.md) remains the
basis for owner-scoped logging and redaction. This work reuses Forge's existing
logger rather than copying donor synchronous stderr/exception dumping.

## Evidence

Pending focused validation and independent review. Native logs are kept in the
ignored `.cache/acceptance/app-daemon-diagnostics-20261014/` directory. Historical
acceptance receipts describe their original revisions, not this change.
