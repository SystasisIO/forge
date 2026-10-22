# WAMR Patched Qualification

Date: 2026-10-15. Status: experimental, pending changes, **NOT ACCEPTED** as
the complete replacement. This follows the [initial failures](forge-wamr-qualification-v1.md).

## Changes And Evidence

The clean upstream pin remains `ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`.
The [tracked patch](../../cmake/patches/wamr-deterministic-profile.patch) is
applied to build-local copies for both runtime and compiler, never to the
checked-out submodule. Its SHA-256 is
`a31082ceab5ef963985d317a1fc835f95e99efc5caedf15e239fc56ba9a94a9a`.

- Classic/fast interpretation and AOT arithmetic canonicalize NaN results to
  f32 `0x7fc00000` and f64 `0x7ff8000000000000`. Demote/promote follow the same
  rule. Transport, reinterpretation, abs, neg and copysign retain payload bits.
  This defines the experimental profile, not historical EOS numeric identity.
- Entry, instantiation/start, native callbacks, indirect calls and nested calls
  save/reset/restore the floating-point environment. Guest execution uses
  nearest-even rounding with gradual underflow, independent of the tested
  host rounding and flush-to-zero settings. Host state is restored after
  normal returns, traps and the tested cancellation paths.
- AOT loop cancellation no longer depends on guest shared memory. The generated
  cancellation load is volatile/atomic acquire. The compiler still explicitly
  enables thread-manager checks; guest threads/shared memory remain disabled.
- Sanitizer inspection found native pointer alignment defects in classic
  frames/branch blocks and wide accesses to 32-bit cells. The patch aligns
  frames and branch blocks; the build selects upstream's safe unaligned-access
  implementation with `WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS=0`. No sanitizer
  category is suppressed. Both compiler/runtime disable fast-math and FP
  contraction and enable rounding-aware compilation.

## Beginning The Private Implementation

The existing `libraries/vm/wasm/interpret` leaf now owns private implementation
components used by the probe:

- `engine_runtime`: one shared process-lifetime upstream runtime, no per-instance
  registry initialization/destruction and no new scheduler.
- `engine_module`: owned mutable loader buffer; caller bytes are untouched;
  modules retain runtime ownership.
- `engine_instance`: instances retain module ownership; checked scalar call
  cell buffers; rejection of overlapping calls; per-instance termination.
- `engine_thread_scope`: thread setup/cleanup around execution, without an
  ad-hoc worker or manual thread setup in consumer tests.

This is implemented private code, not a new public WAMR facade. There is no
second intrinsic registry, decoder, allocator, code cache or compiler queue.
**It is not yet wired behind `backend`, `compiled_module` or the OC APIs.**
The normal Forge target and installed package still use the existing backend.
Diagnostic exceptions/bool results in the private prototype are not substitutes
for the existing typed public errors. Global import registration and callback
lifetime still need the canonical registry bridge before production selection.

## Verified Locally

Host: macOS 26.1 (25B78), ARM64, Clang/LLVM/lld 22.1.8. Release unless noted.

| Run | Result | Local report |
| --- | --- | --- |
| Classic interpreter + AOT + identity checks | 33/33 PASS | `build/wamr-probe-patched/results-private.xml` |
| Fast interpreter + AOT + identity checks | 33/33 PASS | `build/wamr-probe-fast-patched/results-private.xml` |
| Classic/AOT loader + private layer, ASan/UBSan, RelWithDebInfo | 33/33 PASS, no sanitizer diagnostics, halt-on-error enabled | `build/wamr-probe-asan/results-asan.xml` |
| Fast/AOT loader + private layer, ASan/UBSan, RelWithDebInfo | 33/33 PASS, no sanitizer diagnostics, halt-on-error enabled | `build/wamr-probe-fast-asan/results-asan.xml` |
| Classic/AOT cancellation cases, 25 repetitions each | 4 cases x 25 PASS | `build/wamr-probe-patched/cancel-repeat.log` |
| Existing dirty OC comparison baseline recheck | Unchanged | [baseline manifest](forge-wamr-baseline-v1.json) |

The initial sanitizer run reported alignment UB despite CTest returning zero
under the sanitizer's recovery default. That run is **not** accepted as a pass;
`build/wamr-probe-asan/results-asan-initial.xml` retains it. The final tests
force halt-on-error, including comparison subprocesses. Native AOT-generated
instructions are not instrumented by the host C/C++ sanitizer flags.

The numeric corpus has 3165 records, each containing f32/f64 results, and runs
in both modes under four host rounding modes. It covers arithmetic, sqrt,
rounding, min/max, NaN payload preservation for bitwise operations and
demote/promote. It is **not** exhaustive, not an independent full arithmetic
oracle, and not an x86-64/ARM64 equivalence proof.

Cancellation tests first observe the host entry marker, cancel A while B is
still executing, reject an overlapping call to A, then cancel B and reuse both.
The workers have different host rounding modes, restored on return. These
tests do not establish pre-start cancellation or host-call interruption.

Runtime linkage inspection (`otool -L`) reports only system C/C++ libraries,
not LLVM. This does not replace installed-package relocation acceptance.

| Local artifact | SHA-256 |
| --- | --- |
| `build/wamr-compiler-patched/compiler/wamrc` | `8ae2ef79fbe951d70997871b53026df08056bd14f1c8e25dd520aec51cadbad8` |
| `build/wamr-probe-patched/wamr_probe` | `cca49a278b37e1bd9849d161394818fea031312291afaea87b5a37e95419181b` |
| `build/wamr-probe-patched/fixture.wasm` | `f61801cc48f374f5006031783c2680a448eb65ffe0b2ca9cad9a3276c2e11c9b` |
| `build/wamr-probe-patched/fixture.aot` | `3ee4b626153e64e3b39fe6caf4d77a759f7ddbe1ee524e92ce17482895ae77f8` |
| `build/wamr-probe-fast-patched/wamr_probe` | `af9f57625703433f395ea69a4eaddcc2e1d4c4268d6eb52265a76ed455dbb85a` |
| `build/wamr-probe-asan/wamr_probe` | `de31e4d734fa989d747c6a85204382d894fddb756d600e8fc5cbcbb94b6e9fae` |
| `build/wamr-probe-fast-asan/wamr_probe` | `d7cb41b9362d7bd6251bd549acf9f8ea0a90ffa6d23bd29ee0db6b48a7a92665` |

These identify local evidence, not reproducible builds or trusted remote
artifacts. Compiler sidecar checks detect mismatches, not malicious rewriting
of both compiler and sidecar. [Build instructions](../../tests/vm_wamr/README.md).

## Remaining Mandatory Work

1. Structured upstream failures and complete admission checks for every existing
   option, including logical call depth. No parsing exception strings or
   retaining the old parser as an unnoticed runtime dependency.
2. Canonical registry-driven host binding, checked views/converters, host
   exception capture and the existing public error categories.
3. A single invocation scope covering pre-canceled entry, instantiation/start,
   apply, callbacks, deadline/stop races, cleanup and reuse/poison semantics.
4. Replace implementation behind the existing public APIs and run unchanged
   installed-package/SDK/testhost/validator consumers. Adapt the existing OC
   compiler session and cache; do not create replacements for their ownership.
5. Full donor contracts, malformed-input/fuzz tests, exhaustive numeric and
   conversion cases, compiler folding, x86-64/Linux, Linux ARM64, performance
   and downstream transactional effects.

The original [acceptance matrix](../iterations/forge-vm-wasm-wamr-v1.md) is
unchanged in scope. No default switch, product pin update, license change,
commit, push or merge is part of this local qualification.
