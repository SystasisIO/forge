# WAMR Host Binding And Metadata Qualification

Date: 2026-10-15. Status: local experimental evidence, NOT production acceptance.
Branch: `vm-wasm-wamr-experiment-v1`, base
`8975f234dabf2cbfb91d766bf32194f3d8832a5b`, with pending changes.
This is an increment after the [initialization qualification](forge-wamr-initialization-qualification-v1.md).
Public `interpret::backend` and `execution::compiled_module` still use EOS.
Full options and OC session/cache integration are **not complete**.

## Identity

- Clean upstream pin: `ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`.
- The three earlier patches retain their hashes recorded in the initialization report.
- New `wamr-aux-stack-boundary.patch` SHA-256:
  `ebfb956355a3d5fda792204cbc00f87bfbbdb97e8d3d806b848d981c1f744def`.
- Shared `ForgeWamrProfile.cmake` SHA-256:
  `f8a7c360400a71084c9224ce9302965f56e51959136d0f56c99b638535dae7fa`.
- Ordered four-patch-plus-profile digest:
  `bb1e9e7f4b30b9dcfcb09b8cdbf50f2ee2a618844938e8d7d7e84599a2a19c9e`.
- Prepared source/compiler identity is the upstream pin, a hyphen, and that digest.
- Host: macOS ARM64, Homebrew Clang/LLVM 22.1.8, Ninja, macOS SDK.

The profile sets `WAMR_BUILD_QUICK_AOT_ENTRY=0` and
`WAMR_BUILD_SHRUNK_MEMORY=0` in both compiler and runtime. It is a configure
dependency and participates in the compiler sidecar identity. The sidecar
detects accidental source/profile/binary mismatches; it is not a production
artifact authentication or OC cache invalidation mechanism.
Vendor source is unchanged; patches apply only to build-local prepared copies.
The captured OC baseline still matches aggregate
`cdabf9b0de61cbc0dd547c6cab267c24617e9efa29e0c379b4eaaeb54910bb08`.

## Implemented Private Bindings

`interpret/details/engine_bindings.hxx` and its paired implementation own:

- Immutable module-local function descriptors and exact module/name/signature
  resolution. The module is loaded with `no_resolve`, then its own import slots
  are bound before publication. No process-global native register/unregister
  operation, temporary registry replacement or product policy is added.
- Scalar i32/i64/f32/f64 argument/result conversion. Pointer signatures stay
  wasm32 numeric offsets; byte spans and bounded strings validate the actual
  current linear-memory range with overflow-safe arithmetic. Views cannot be
  retained across memory growth or callback completion.
- Instance-owned host context, stop token and original `exception_ptr`. A
  `noexcept` trampoline catches host exceptions; the outer C++ boundary rethrows
  after WAMR returns. No exception unwinds through C/generated frames and no
  error-string matching classifies host failures.
- Typed host failure precedence over racing cancellation, for defined guest
  start and later export calls. Pre-cancellation still prevents guest effects.
- Distinct bindings for different modules using identical import names;
  nested and concurrent invocations retain their own context and bindings.

The test expands every signature in `FORGE_CONTRACT_INTRINSICS`, including
pointer and bool entries. Five test callbacks implement the current pure
execution-profile shapes and run a synthetic SDK echo module. These are
**not** the production `execution::intrinsics` adapter, full compiler-builtin
behavior, original compiled SDK contracts or downstream acceptance.

The low-level instance can recover after a host failure; this only qualifies
engine cleanup. The public backend's host-failure poison/discard rule must be
implemented above it before switching consumers. Typed guest trap, allocation
failure, deadline and public unusable-instance mapping remain open.

## Partial Limits

`engine_limits::check_metadata` reads upstream-validated bytecode structures,
not a copied decoder or independent options DTO. Its tests compile the actual
public `interpret::options` module. It checks fourteen metrics:

1. Seven section counts: type, import, function, global, export, element and data.
2. Mutable global bytes and the initial table element count.
3. Maximum element-segment count and data-segment byte count.
4. Maximum initialized data end offset and import/export symbol byte count.
5. Initial memory in logical 64 KiB WASM pages.

Mutable-global export policy is separate. Explicit section bounds, including
zero, take precedence over the generic section fallback. Threshold and one-less
tests cover each metric. Non-function imports are refused by this private profile.

The instance also checks the caller's page cap **before** allocation and passes
it to WAMR for growth. Tests cover a larger module maximum, no module maximum,
initial memory above the cap, unchanged pages after failed growth, and a module
without memory.grow/size whose tail is still accessible through host callbacks.
Coalesced WAMR page counts are converted to bytes before applying logical limits.

This is explicitly **not full admission**. Still required:

- Function body bytes, local declaration sets, parameter/local/operand-stack
  accounting, control/legacy nesting, br_table and memory-instruction offsets.
- Logical call depth in interpreter and AOT, not a native stack-byte substitute.
- Complete disposition of historical permissive parser options and custom-name
  handling; no field may silently become ineffective.
- Early load/validation allocation bounds. Post-load metadata checks do not
  prevent allocations already performed by the loader.
- Valid zero-page profiles, typed host allocation failures and public option
  integration. The private instance currently requires a positive page cap.
- Source-derived admission/profile identity carried into authenticated AOT
  artifacts, checked before native execution. Raw AOT metadata is not such proof.

The full [option map](forge-wamr-api-map-v1.md) remains normative.

## Defects Found And Fixed

- A Clang/lld stack-first fixture has `aux_stack_bottom == aux_data_end == 65536`
  and a 65536-byte auxiliary stack. WAMR accepted the module but could not create
  the execution environment: its stack-layout test used `<` rather than `<=`.
  The new patch corrects this boundary in interpreter and AOT. The issue was
  **not** a missing `__stack_pointer`; no optional-stack bypass was retained.
- WAMR's memory cap helper raises a requested cap to the module's minimum.
  Forge now refuses an oversized initial memory before instantiation.
- WAMR may shrink memory when the guest does not contain memory.grow/size,
  although an intrinsic can legally access the declared tail. The shared
  compiler/runtime profile disables this optimization. It retains bounds checks.
- The first private bridge implementation let racing cancellation hide the
  original host exception. Both start and export boundaries now rethrow the
  saved host exception first, matching the existing public precedence rule.

Embedded NUL import names were already rejected by upstream UTF-8 validation;
the negative test proves refusal without adding another loader patch.
No check was disabled to obtain a passing result.

## Verification

| Evidence directory | Final result |
| --- | --- |
| `build/wamr-compiler-library` | Rebuilt with the current profile/identity; all library-AOT fixtures generated |
| `build/wamr-probe-initialization` | Classic Release: 79/79 PASS |
| `build/wamr-probe-initialization-fast` | Fast Release: 79/79 PASS |
| `build/wamr-probe-initialization-asan` | Classic ASan/UBSan: 79/79 PASS, halt-on-error |
| `build/wamr-probe-initialization-fast-asan` | Fast ASan/UBSan: 79/79 PASS, halt-on-error |
| `build/wamr-boundaries-host/bindings-source-results.xml` | Existing public execution and family structure: 2/2 PASS |
| `tests/structure/check_structure.py .` | PASS, 2643 first-party source files |
| Baseline capture `--check` | Unchanged captured OC baseline |

Each runtime directory contains `bindings-full-results.xml`,
`bindings-ctest.log` and `bindings-build.log`. Existing earlier reports are not
rewritten. Ten binding tests (cancel, busy, exceptions, start and isolation in
WASM/library-AOT modes) passed 100 repetitions each: **1000 executions** in
`build/wamr-probe-initialization/bindings-repeat.log` and
`bindings-repeat-results.xml`.

The 79-test suite retains the previous numeric corpus and initialization tests.
Local interpreter/AOT parity and FP restoration pass; full numerical coverage
and Linux/x86-64/ARM64 cross-platform determinism remain unqualified. Sanitizers
instrument runtime/host code, not generated guest machine code. Guest bounds
and stack checks remain independently enabled. `otool -L` on the binding probe
shows only libc++ and libSystem, not LLVM.

The broader source/package selection also ran and retained its failure in
`build/wamr-boundaries-host/bindings-execution-results.xml`: two tests passed,
the installed-package consumer failed because `ForgeConfig.cmake` was absent.
The prior installation remained blocked by the unchanged OpenSSL header/build
failure documented in the initialization report. The isolated source rerun is
not an installed-package pass. No package files or acceptance results were faked.

## OC Integration: Inspected, Not Implemented

Inspected the captured comparison owners: `compiler_session.cppm`,
`compiler_protocol.cppm`, `compiler_session_impl_cache.cpp`, `artifact.cppm`,
`compile_request.cppm` and their build ownership. The existing contract requires:

- `compiler_protocol` version 2 carries the compiler/target/configuration/resource
  handshake. A reply is an encoded Forge artifact, not an arbitrary native blob.
- Artifact version 1 currently contains EOS-OC native sections, relocations,
  functions, globals, memory, table and start metadata. WAMR AOT is not a bytewise
  replacement for that layout or the existing executor's native assumptions.
- `compiler_session` already owns scheduler/executor integration, bounded jobs,
  tickets, byte budgets, deadlines, stop and shutdown. Reuse it rather than adding
  a parallel runtime worker or queue.
- Existing cache ownership includes private-directory checks, no-follow opens,
  exclusive locking, accounting of corrupt/orphan files, atomic fsync publication,
  and old-plus-temporary-byte budgets. Preserve these operations and their tests.
- Cached artifacts are decoded with expected identity and size limits before
  preparation. Introduce a versioned WAMR payload and engine/target/numeric/admission
  identity here; reject legacy/foreign payloads and rebuild from original WASM.

No second cache/session was created. Neither a real OC session nor a real cache
entry currently executes WAMR. Direct fixture AOT loading and compiler sidecars
must not be reported as session/cache integration or invalidation coverage.

## Next Mandatory Work

1. Complete source admission and structured guest failure mapping in the shared
   engine layer, retaining upstream parsing rather than a second WASM decoder.
2. Wire the existing public host/converter and invocation API, preserving the
   single deadline/start/apply scope, poison semantics and SDK behavior.
3. Adapt the existing OC artifact, compiler process and session/cache owners;
   prove deadline/cancel/crash/restart, bounded memory and safe cache rebuild.
4. Run actual SDK/donor/product contracts, installed runtime-only packages,
   cross-platform numerical and performance qualification and residual-EOS audit.

No commit, push, merge, license edit or product pin change is part of this increment.
The [full acceptance matrix](../iterations/forge-vm-wasm-wamr-v1.md) remains open.
