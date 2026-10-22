# WAMR Initial Qualification Results

Date: 2026-10-15. Status: **NOT ACCEPTED** for the replacement.
This is a local upstream mechanism probe, not a Forge consumer acceptance run.

Historical snapshot before the tracked patch. Follow-up results and current
limitations are in [patched qualification](forge-wamr-patched-qualification-v1.md).
The original XML reports are retained; local executable paths can be rebuilt
and should not be assumed to retain the historical hashes below.

## Identity

- Forge base: `8975f234dabf2cbfb91d766bf32194f3d8832a5b` plus the
  experimental pending changes on `vm-wasm-wamr-experiment-v1`.
- WAMR: `ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`, clean submodule,
  no local upstream patches. It reports version 2.4.3; identify it by commit.
- Host: macOS 26.1 (25B78), ARM64; Clang/LLVM/lld 22.1.8, Release builds.
- Runtime: classic interpreter plus AOT loader, software bounds checks,
  no WASI, guest shared memory, guest threads, SIMD or JIT.
- Fixture: Clang `wasm32`, `-mcpu=mvp -O2 -ffreestanding`, explicit `wasm-ld`;
  2 initial / 4 maximum memory pages. It is a test fixture, not a donor contract.
- AOT: generic aarch64, explicit memory/stack bounds checks, thread-manager
  option enabled. The compiler logs `aarch64v8-pc-linux-gnu` even on this host.
  Its output executes through the custom AOT loader here; this does not prove
  a portable production target identity or foreign-OS artifact compatibility.

| Local artifact | SHA-256 |
| --- | --- |
| `build/wamr-compiler-current/wamrc` | `26be2f1e3f3ce3e5b434635d098b414d205276c7af8c22bb2454c968578c79ba` |
| `build/wamr-probe/wamr_probe` | `91171b318a5f0610930045d1adc8d397d3faa4ffbc134232ab431f7223977d80` |
| `build/wamr-probe/fixture.wasm` | `ba59ed5ce3381a3a741d261de1b82e47a68be5f06355283abc45fa228941f0d2` |
| `build/wamr-probe/fixture.aot` | `dd2d3836fb95cd947887589f72880cebee661bcc9aa64527a626b44ad22f6846` |

These hashes identify local evidence, not reproducible-build guarantees. The
binaries and raw XML/logs stay in ignored `build/`, not in source control.

## Observed Results

Reproduction: [probe README](../../tests/vm_wamr/README.md).
CTest after the explicit linker correction: **13 passed, 4 failed, 17 total**;
exit status 8. The initial run had the same four failures. No failing test is
marked expected-failure, disabled or counted as success.

| Probe | Interpreter | AOT |
| --- | --- | --- |
| Integer/i64 values and native import | PASS | PASS |
| Initial memory, growth and module-declared maximum | PASS | PASS |
| Division/OOB traps and subsequent simple call | PASS | PASS |
| Overflowing host memory range | PASS | PASS |
| Independent and fresh instance state | PASS | PASS |
| Terminate entered infinite loop, then use another/fresh instance | PASS | TIMEOUT, 15.04 s |
| Finite arithmetic, signed zero and small subnormal samples | PASS | PASS |
| Host rounding perturbation | FAIL | FAIL |
| Cross-mode comparison of seven numeric bit patterns | FAIL, shared comparison | FAIL, shared comparison |

The last row is one CTest case, not two. The NaN values are emitted by the
numeric cases and compared by that separate gate.

Other checks:

- `wamrc` build against LLVM 22.1.8: PASS at the development pin.
- Stable WAMR 2.4.5 build against the same LLVM: FAIL on changed LLVM APIs;
  see the [version-selection note](forge-wamr-v1.md).
- Probe runtime linkage: `otool -L` shows only system C and C++ libraries,
  not LLVM. Full Forge package installation/relocation is NOT_RUN.
- Forge structure lint: PASS. Comparison baseline recheck: PASS,
  scope hash `cdabf9b0de61cbc0dd547c6cab267c24617e9efa29e0c379b4eaaeb54910bb08`.

## Blocking Findings

### Cross-Mode NaN Bits

For `f64.add` with quiet NaN operands `7ff8000000000123` and
`7ff8000000000456`, the interpreter returns the second payload; AOT returns
the first. The analogous f32 test differs as well (`7fc00456` versus
`7fc00123`). WASM can permit NaN nondeterminism; Forge's required execution
profile cannot let an admitted reinterpret/store observe different results.

This is not a requirement to reproduce historical EOS NaN payloads. It is a
requirement to define and enforce one result across the new execution modes.
Canonicalization, software floating point and compiler lowering need a scoped
design and a full numeric corpus before choosing an implementation.

### Host Rounding Leaks Into Guest

Both modes return 2.0 for `f64.nearest(2.5)` under `FE_TONEAREST`, but 3.0
under `FE_UPWARD`. The fixture contains the actual WASM `f64.nearest`
instruction. Upstream classic interpretation uses `rint`, and the compiler
lowers it through `llvm.rint.f64` in `aot_emit_numberic.c`.

A scoped FP environment guard is a candidate part of the adapter, not a full
determinism proof. Entry, host callbacks, nested calls, normal return, trap and
cancellation must preserve the required guest state and restore host state.
Subnormal modes, conversions and constant folding remain unqualified.

### Ordinary-Memory AOT Cancellation

The guest calls a host entry marker and then loops without further host calls.
`wasm_runtime_terminate` interrupts the interpreter, but the AOT process must
be killed by CTest's timeout. In the pinned compiler,
`check_suspend_flags` in `aot_emit_control.c` returns immediately for
non-shared memory before emitting a loop cancellation check, even with the
thread-manager compilation option enabled.

The replacement needs cancellation for ordinary-memory contracts. This probe
does not implement a patch, start-function cancellation, concurrent active A/B
isolation, or a typed poison/reuse policy. Sharing guest memory or adding
application callbacks inside every loop is not an accepted workaround.

## Evidence And Next Gate

Raw reports: `build/wamr-probe/results.xml`,
`build/wamr-probe/results-initial.xml`, and
`build/wamr-probe/Testing/Temporary/LastTest.log`.
The dirty comparison snapshot is `build/wamr-baseline/source.zip`; the
[tracked inventory](forge-wamr-baseline-v1.json) identifies its source files.

No production adapter, SDK bridge, compiler-session replacement or downstream
migration is implemented in this branch yet. API compatibility and full
functionality remain NOT_RUN; numeric and cancellation qualification are
blocked by the failures above. Fast interpreter, x86-64/Linux, Linux ARM64,
performance and real donor-contract suites have not run.

Continue with the [compatibility map](forge-wamr-api-map-v1.md) and the full
[acceptance matrix](../iterations/forge-vm-wasm-wamr-v1.md). No default backend
switch or consumer pin update is justified by these results.
