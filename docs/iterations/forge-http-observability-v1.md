# HTTP Observability And Executable App Examples

Status: implementation complete; focused native acceptance and two independent
code reviews passed. Integration targets `dev`; no release version change.

## Scope

- Repair App README/runtime examples against the current guarded lifecycle.
  Prefer builder/daemon composition, protected hooks, metadata after types,
  immutable environment and awaited configuration/cleanup.
- Compile and run the canonical examples; mechanically check their documented
  source instead of treating illustrative Markdown as tested production code.
- Add HTTP server operational logs and explicitly opt-in redacted completion
  logs, using Forge macros and existing sink routing.
- Keep App as plugin lifecycle owner. The HTTP plugin logs TLS context reload
  outcomes, not a second server lifecycle.
- Preserve HTTP/TLS/auth behavior, streaming, public defaults and wire formats.
  Do not bump the project version or migrate downstream products.

## Acceptance

Focused native builds use one build tree and `-j4`. Run App/example, HTTP server,
TLS/auth, streaming, lifecycle, logger and package consumers plus structure
checks. Cover secrets/exception redaction, request opt-in, one streaming terminal
record, canceled/failed versus completed sends, bounded noisy failures, failed
TLS reload preservation and throwing sinks. No full CI is requested.

Two independent exact-head reviews cover observability/privacy/semantics and
ownership/layout/examples. Fix valid findings before a reviewed PR is merged
into `dev`. Preserve the acceptance logs, then remove only this task's inactive
build tree. No neighboring worktree or downstream source is part of cleanup.

## Boundaries

The application supplies log routing/exporters. HTTP owns neither OTLP lifecycle
nor delivery guarantees. Access records are diagnostic, not billing, security
audit authority or proof that a remote application consumed bytes. Library
readiness and product production-readiness are separate claims.

See the [donor traceability](../donors/http-server-observability.md).

## Acceptance Receipt (2026-10-14)

Code revision: `180127a9f12499ba84ee2dc8d77e28d1fc713ceb`, based on
`1dfce5eeb2d8ebc8fb577d8565033c642b090a2e`. Native arm64 macOS 26.1,
Homebrew LLVM 22.1.8, CMake 3.31.6, Ninja, OpenSSL 3. Debug configuration used
`-O1 -gline-tables-only`; every build was coordinator-owned with `-j4`.

- PASS: 17 focused/structural CTest entries: App, executable README example and
  source correspondence, logger/shutdown, HTTP observability, HTTP plugin
  lifecycle/auth, TLS, shutdown retry, WebSocket/streaming, client preparation,
  parameter binding/error rendering and the three structure gates.
- PASS: five suites repeated 20 times each: HTTP observability, HTTP plugin
  lifecycle, TLS, shutdown retry and the App README example.
- PASS: installed `net_http`, `api_http`, `plugins_net_http_server` and `app`
  component consumers. Exact registered consumer projects/options were used,
  with separate explicit `cmake --build --parallel 4` steps and execution of
  their registered binaries. The HTTP component consumer starts/stops a real
  listener, rather than merely instantiating a config DTO.
- PASS: 13 HTTP observation cases, including real malformed WebSocket upgrade
  responses (400/426), streaming producer/guard failures, reset, redaction,
  disabled access logging and throwing sinks. A separate TLS regression proves
  eight malformed records after successful HTTPS responses produce bounded
  summaries, not per-session ERROR records.
- PASS: negative structural mutations detect raw-socket reads and unsafe TLS
  timer captures in seven extracted implementation/header owners.
- PASS: `git diff --check`. Two independent Sol 6.1 xHigh read-only reviews of
  the exact code revision returned CLEAN after review fixes. Reviewers did not
  run duplicate builds; test evidence was collected by the coordinator.

Review fixes addressed WebSocket failure classification/actual refusal status,
post-handshake TLS flood logging and coverage of the extracted TLS owners.
Shared private polymorphic classes use the repository's existing ordinary C++
linkage pattern; no duplicate-symbol linker suppression was added.

Forge logging remains synchronous. A user-supplied blocking sink can delay its
caller; this is an inherited, documented sink contract, not a new HTTP latency
guarantee. Exception isolation and avoiding sink calls under observation mutexes
do not make an arbitrary sink asynchronous. Applications own bounded sinks and
export queues; HTTP does not introduce a second logger runtime.

Full CI, Linux and sanitizer matrices, and a live OTLP collector were not run.
No claim is made about external telemetry delivery or whole-product readiness.
Local raw logs are retained under
`.cache/acceptance/http-observability-20261014/`; only this task's inactive
`build/http-observability` is eligible for removal after integration.
