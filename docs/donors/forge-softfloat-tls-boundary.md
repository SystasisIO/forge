# SoftFloat thread-local state: source and ABI boundary

2026-10-12. Narrow existing-dependency fix; independent of the frozen options
preparation slice. SoftFloat donor stays at
`19cbea6c254cdbb5d539584e5696675de997721d` and is not edited.

## Verified source facts

- `source/softfloat_state.c:43–51` defaults `THREAD_LOCAL` to empty and defines
  four ordinary globals: `softfloat_roundingMode`, `softfloat_detectTininess`,
  `softfloat_exceptionFlags`, `extF80_roundingPrecision`.
- `source/include/softfloat.h:53–83` uses the same macro in C declarations.
- `source/include/softfloat.hpp:5–7` defaults the macro to `__thread`, but does
  not declare those four variables. A previous C++ variable-declaration ABI
  mismatch is therefore not established by that header alone.
- Actual pre-fix arm64 object sections are `__common` / `__data`, not TLS.
- `SOFTFLOAT_ROUND_EVEN` appears only in the donor CMake definition and has no
  use in the pinned C/header source. `s_roundPackToF32.c:55–65`, its F64 and
  F128 counterparts read `softfloat_roundingMode`. Compilation does not force
  nearest-even arithmetic regardless of state.
- Current Forge interpreter helpers and inspected first-consumer execution
  code do not save, reset or restore those four state variables.

Relevant source SHA-256:

| Source | SHA-256 |
|---|---|
| `source/softfloat_state.c` | `5f7a70c4c5823cdde0518db77a74a6549dba62b21d910f6dadb9115b1d478301` |
| `source/s_roundPackToF32.c` | `4c5186501b37973eefbc3cfd8bbee20e61c6d52f4b2607b7c998142ac50d085d` |
| `source/include/softfloat.h` | `27c5f39c21e10e1e798457cdfada111cc2e440a17acba8804e6a2b0a1d7833fc` |
| `source/include/softfloat.hpp` | `75a9e8be0e054a5b5afee5609c1d24e16670482577619d5b10bba55de27e9d79` |

## Target-owned change

Use one narrow Forge CMake helper to apply
`target_compile_definitions(softfloat PUBLIC THREAD_LOCAL=__thread)` to the
existing compiled target. Both root configuration and the auxiliary graph
call that same helper. No directory/global C/C++ flags, mutex, numeric wrapper
copy or donor checkout edit is needed. This covers the current Clang/GCC
baseline; it does not expand supported product platforms.

`PUBLIC` is required: all C source definitions and all C/C++ consumer
declarations must agree. The installed internal SoftFloat target must export
the same usage requirement. Ordinary-to-TLS variable linkage is an ABI change:
rebuild the complete SoftFloat archive and every consuming library/executable;
never link old ordinary-state objects with new TLS declarations. Object
inspection must show Mach-O thread-local sections here and ELF TLS symbols on
the eventual Linux native lane.

## Required regression

Use barrier-controlled worker threads. Both capture fresh defaults and actual
addresses of all four variables before mutation; addresses differ across
threads. One thread selects nearest-even, the other round-toward-positive;
both preserve their independent tininess/precision state. For exact halfway
`f32_add(0x3f800000, 0x33800000)`, results must be `0x3f800000` and
`0x3f800001` respectively. A second coordinated phase checks exception flags
remain local after only one thread raises invalid or inexact.

Also execute actual float operations concurrently through the existing VM
path and the isolated LLVM fixture callback; derive expected bits from the
pinned algorithm and selected mode, not the unused compile definition. Restore
the test's main-thread state after each case. Rebuild and re-run existing
unit/internal/spec targets and record object sections, compile definitions,
commands, original logs and a separate patch hash.

## Same-thread invocation scope

TLS fixes cross-thread state sharing, not nested calls on one thread. Before
new native execution acceptance, the existing VM owner must provide one real
numeric-environment operation reused by engines: capture all four values,
set canonical nearest-even / tininess-after-rounding / zero exception flags /
extended precision 80 at invocation entry, and restore the captured values on
every exit, including traps, deadlines and exact host exceptions. A nested
invocation must restore the outer invocation's current state. Generated frames
must never longjmp across that C++ scope or host frames with destructors.

This target slice does not add or duplicate a public numeric scope. The exact
existing-owner API and its integration into both interpreter/JIT and OC need
coordinated review. Existing math helpers remain the implementation source.

## Local TLS acceptance evidence

The sequential auxiliary build uses LLVM 22.1.8, `-j 4`, the pinned SoftFloat
checkout and the existing owning VM targets. Reproduction:

```sh
bash artifacts/oc-phase0/run.sh LLVM_PREFIX PINNED_SOFTFLOAT_CHECKOUT
```

The barrier-controlled TLS case passed independently and inside the standard
unit suite. Standard suites passed: unit 188 cases, internal 12, spec 2428;
the selected auxiliary CTest run passed all five targets. The isolated LLVM
runtime also passed two concurrent threads, each executing 20000 actual float
callbacks with distinct halfway-rounding results and local flags. These are
TLS tests before the separate canonical invocation-environment change.

`build/softfloat-tls-object.log` records `__thread_bss`, `__thread_vars` and
`__thread_data` on the rebuilt arm64 state object. The C state definition and
C++ consumer commands both carry `-DTHREAD_LOCAL=__thread` in
`build/compile_commands.json`. Logs and binaries stay in ignored
`artifacts/oc-phase0/build/`; the source-only TLS patch is frozen separately
from the options patch before adding invocation scopes.

This is local auxiliary evidence, not root/package acceptance. Native Linux
ELF TLS execution, installed-consumer rebuild and native JIT execution remain
NOT_RUN. Same-thread host mutation/nested invocation restoration is the next
separate slice, not a property established by this TLS test.
