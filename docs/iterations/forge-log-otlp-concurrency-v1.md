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

The initial normal focused run passed all five CTest entries. The final patch
adds ten logger, seven exporter, four plugin and three Schema tests. The
no-argument API regression checks both `flush()` and `metrics()` after every
API handle, the plugin registry and the source runtime have been destroyed.

Build tree: `build/log-otlp-concurrency`; all builds use `-j4`. The same tree is
reconfigured sequentially for sanitizers, rather than retaining three builds.
Sanitizer flags apply to production Forge objects and test objects. Prebuilt
system/Homebrew dependencies are not rebuilt with instrumentation.

Code revision: `d36ab846c2d01e5f7f1f7f44a2cbe0e0bda306d4`.

- AddressSanitizer with LeakSanitizer enabled: all six executable CTest entries
  pass. This covers 29 logger, 30 exporter, 23 plugin, 22 Schema and 20 config
  test cases, plus the late-shutdown helper. No suppression or leak exclusion
  is used.
- Both external build-tree package consumers (`otlp`, `plugins_log_otlp`)
  compile and run with ASan and leak detection. Their temporary builds are
  removed after preserving configuration and run evidence. This is not an
  install/relocation or full-package-matrix claim.
- `test_forge_structure` and `git diff --check` pass.
- The focused plugin target explicitly enables its MDBX and RocksDB test
  variants. Both execute in the final ASan pass; the early unsanitized focused
  pass is not used as backend evidence because that target initially omitted
  these compile definitions.
- Final ThreadSanitizer validation passes all six executable suites, each
  repeated three times with `--repeat until-fail:3`. Both DB backend variants
  execute here as well. No TSan suppression is used.

Independent review caught and closed three defects before acceptance:

- Copy self-assignment could overwrite a concurrently published logger route.
  It now returns without loading/storing the shared handle.
- HTTP's request-specific cancellation filter could leak into the exporter's
  subsequent retry wait. HTTP requests now run in an isolated coroutine chain
  on the same owning strand; tests prove deadline cancellation of a retry timer
  that was already started before shutdown.
- No-argument API wrappers deferred virtual dispatch while retaining a raw
  `this`. They now obtain the owned typed operation eagerly. The void-returning
  flush adapter owns that awaitable by value, preserving the public signature.

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

The callback cycle is removed with weak references back to the owning fields.
Callbacks retain a strong owner only during invocation. Schema copies and live
builders preserve ownership; an extracted schema-dependent callback invoked
after its last owner dies throws typed `invalid_value`. That lifetime boundary
is documented and covered explicitly, including vector growth.

## Reproduction

Use the environment above with both DB drivers enabled. On the same build tree,
set `CMAKE_CXX_FLAGS` and `CMAKE_C_FLAGS` to include either
`-fsanitize=address -fno-omit-frame-pointer` or
`-fsanitize=thread -fno-omit-frame-pointer`, and matching executable/shared
linker flags. Production/test Debug flags are `-O1 -gline-tables-only`;
the external package consumers use `-O0 -g0` with ASan.

```sh
cmake --build build/log-otlp-concurrency -j4 --target \
  test_forge_schema test_forge_config_core test_forge_log \
  test_forge_otlp test_forge_plugins_log_otlp

ctest --test-dir build/log-otlp-concurrency --output-on-failure \
  -R '^(test_forge_structure|test_forge_schema|test_forge_config_core|test_forge_log|test_forge_log_shutdown|test_forge_otlp|test_forge_plugins_log_otlp)$' \
  --timeout 120

git diff --check
```

ASan runs use `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`; TSan runs use
`TSAN_OPTIONS=halt_on_error=1`. The LLVM symbolizer is set explicitly for both.
Local logs and configuration evidence are retained under
`.cache/acceptance/log-otlp-concurrency-20261013/` before build cleanup.
Linux and the full CI matrix are not part of this acceptance.

Independent Sol reviews cover logger/Schema ownership and OTLP/plugin ownership
separately. All three actionable review findings are closed on the code revision
above. The receipt-only followup does not change executable sources.

This records focused native acceptance, not a proof of every possible schedule
or a cross-platform/full-CI result. The runtime ownership requirements above
remain part of the supported application contract.
