# Logger And OTLP Concurrent Shutdown

## Scope

Fix concurrent logger configuration and OTLP management API/shutdown races
present at base `41164423`. Preserve public contracts, the synchronous
`forge_log` boundary, OTLP wire encoding, queue bounds and reverse plugin
shutdown. No project version change or full CI run is part of this patch.

The earlier operational logging receipt only accepted ordered lifecycle.
It did not establish safety for live setters or management calls overlapping
exporter teardown.

## Ownership Requirements

- Shared logger handles must synchronize level, enabled, name and parent state.
  Default-route replacement must also be safe for the existing reference API.
- Routing updates must not invoke sink callbacks or destroy user sinks while
  holding registry/routing locks. Reentrant sinks remain supported.
- Configuration snapshots and already acquired logger handles have explicit
  semantics; installing a new named route does not mutate an old handle.
- OTLP API admission and shutdown must synchronize access to the exporter.
  Accepted asynchronous operations retain their owner across suspension.
- Exporter lifecycle operations execute on its own strand, not merely after
  posting a continuation through that strand. Completion is sticky and shared
  where concurrent callers await the same shutdown.
- OTLP request-stop does not discard producers' final shutdown logs.
  Explicit exporter shutdown closes admission and completes or cancels accepted
  operations before releasing their resources.
- Existing Forge async primitives are reused. No new thread pool, polling
  runtime, generic task framework or Log-to-Asio dependency is introduced.

## Acceptance

Focused tests exercise concurrent setters and logging, default replacement,
reentrant sink removal, metrics/flush/shutdown overlap, retained API handles,
multiple executors, cancellation and bounded completion.

The coordinator builds with `-j4` and runs:

- `test_forge_log` and `test_forge_log_shutdown`;
- `test_forge_otlp` and the focused `test_forge_plugins_log_otlp`;
- `test_forge_schema` and `test_forge_config_core` for the sanitizer-discovered
  callback ownership cycle;
- external OTLP and plugin package consumers;
- `test_forge_structure` and `git diff --check`.

ThreadSanitizer and AddressSanitizer builds are separate instrumentation modes.
They exercise real production implementations, not substituted concurrency
models. A passing unsanitized test does not replace sanitizer evidence.
Any unsupported sanitizer case, suppression or uninstrumented external
dependency must be reported explicitly.

Only the current task's inactive build artifacts may be removed after evidence
is preserved. Merge requires clean exact-head independent review and the focused
acceptance results.

## Evidence

Native environment: macOS arm64, Homebrew LLVM 22.1.8, Ninja, Boost 1.90,
OpenSSL 3.6.4. OpenSSL 3 and the matching Boost include prefix are explicit:
the machine's generic Homebrew include prefix currently exposes OpenSSL 4.
RocksDB and MDBX are both enabled. The DB Store integration build also needed
an explicit `std::size_t` for its nonnegative `count_if` log field; the previous
signed difference type made Variant conversion ambiguous on this toolchain.

The initial normal focused run passed all five CTest entries. Review added a
logger self-assignment regression and two already-started retry-wait regressions,
bringing the new scenarios to ten logger, seven exporter and three plugin tests.
Both external build-tree package consumers (`otlp`, `plugins_log_otlp`)
compiled and ran.
This is not an install/relocation or full-package-matrix claim.

Build tree: `build/log-otlp-concurrency`; all builds use `-j4`. The same tree is
reconfigured sequentially for sanitizers, rather than retaining three builds.
Sanitizer flags apply to production Forge objects and test objects. Prebuilt
system/Homebrew dependencies are not rebuilt with instrumentation.

ThreadSanitizer passed all four executable CTest entries after both review fixes:
29 logger, 30 exporter and 22 plugin test cases, plus the late-shutdown helper.
The focused plugin target explicitly enables its MDBX and RocksDB test variants;
those variants ran in this sanitizer pass. No TSan suppression was used.

Independent review caught and closed two defects before acceptance:

- Copy self-assignment could overwrite a concurrently published logger route.
  It now returns without loading/storing the shared handle.
- HTTP's request-specific cancellation filter could leak into the exporter's
  subsequent retry wait. HTTP requests now run in an isolated coroutine chain
  on the same owning strand; tests prove deadline cancellation of a retry timer
  that was already started before shutdown.

Runtime lifetime remains an explicit application obligation. Caller executors
must progress until their admitted API calls join. Producer shutdown must also
join synchronous logging calls: sink detach excludes new snapshots but is not
a barrier for a delivery that already captured an OTLP sink.

The initial ASan/LeakSanitizer run passed functional assertions but failed the
logger and plugin processes on real Schema ownership cycles: respectively 2,784
bytes / 8 allocations and 879,528 bytes / 2,051 allocations. Field callbacks
retained the vector that owned those callbacks. This preexisting leak is reached
through diagnostic redaction and plugin config construction; it is not hidden
with `detect_leaks=0` or a suppression. The same patch corrects that ownership
and adds focused Schema/config validation.

Leak repair and final exact-head validation are in progress. No final acceptance
claim yet.
