# WAMR Initialization And Compiler Qualification

Date: 2026-10-15. Status: local experimental evidence, NOT production acceptance.
Branch: `vm-wasm-wamr-experiment-v1`, base
`8975f234dabf2cbfb91d766bf32194f3d8832a5b`, with pending changes.
This supplements, rather than rewrites, the earlier qualification reports.

## Identity

- Clean upstream WAMR pin: `ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`.
- Numeric patch SHA-256: `a31082ceab5ef963985d317a1fc835f95e99efc5caedf15e239fc56ba9a94a9a`.
- Deferred initialization patch SHA-256: `3e02f941134b309afdc7946b56972a8451eb1503699a02defd2ea1c7afec14f8`.
- Native-call/ARM64 alignment patch SHA-256: `d6f0e31fa3d80bce934e9591e13632da2ea82adf0c86663cdd2484c84338a719`.
- Combined ordered patch digest: `615778afcc62ee1a97c3c7a9e3c6bbd2aeb34c3515f57348c2c92ca28ad11990`.
- Host: macOS ARM64, Homebrew Clang/LLVM 22.1.8, Ninja, macOS SDK.
- Prepared source identity is the upstream pin plus the combined patch digest.
  Both compiler binaries have a source/binary-hash sidecar. These prevent
  accidental local mismatch, not hostile compiler substitution or a complete
  production artifact-profile/provenance validation.

The vendor checkout is unchanged. CMake applies the ordered patches to an
ignored build-local source copy. The captured OC baseline manifest/archive
remains unchanged. Compiler source preparation must not edit that baseline.

## Source Work

1. Moved SDK execution to `vm/wasm/execution`, including namespace, module,
   target, package component, tests and in-repository consumers. The operation
   is a mechanical source migration, not a new backend. The exact map is in
   the [boundary note](../releases/unreleased-wasm-family-boundaries.md).
2. Added a real private `vm/wasm/compiler` implementation. It calls the WAMR
   loader/compiler/emitter, returns owned native bytes and checks input/output
   caps. Test-only command code invokes this leaf. LLVM remains outside runtime.
3. Added opt-in deferred initialization in both upstream runtime modes. The
   private Forge instance creates its execution environment, registers a stop
   callback, then runs guest initialization. Existing normal WAMR instantiation
   remains unchanged unless the defer option is explicitly selected.
4. Initialization may be attempted once; failure destroys the private instance.
   A token can cancel before start, during start or before the subsequent export.
   Callback teardown precedes instance destruction. Host FP state is restored.
5. Added structural negative tests for reverse imports/links, LLVM in runtime,
   the retired execution identity/dispatcher and SDK registry ownership.

The start test emits a real WASM section-8 start function, not an export named
`start`. It records entry before looping, checks pre-cancel has no guest effects,
tests a start trap, then verifies successful fresh initialization, no duplicate
start and a stop requested between initialization and a later export.

## Defects Exposed By Expanded Tests

- Quick C entries into generated AOT code crashed Clang's function sanitizer,
  which reads function metadata before the generated entry address. Use the
  existing upstream assembly ABI bridge in **all** profiles via
  `WAMR_BUILD_QUICK_AOT_ENTRY=0`. No sanitizer exclusions or test skips were added.
  This changes a call-path optimization, not guest checks; performance remains
  an explicit unqualified gate.
- The normal native bridge read i64/f64 arguments through eight-byte-aligned
  pointer casts, although the cell buffer provides four-byte alignment. Use
  upstream cell access helpers and byte copies for FP values. Tests include
  mixed i32/f64 offsets, f32/f64 results and floating arguments spilling from
  registers into the stack argument area.
- The expanded fixture exposed a four-byte-aligned ARM64 procedure linkage
  table (PLT). Its address store used a `uint64*`; it now copies bytes without
  requiring eight-byte alignment. Instruction layout and relocation offsets
  remain unchanged.

Original failing logs are retained locally under
`build/wamr-probe-initialization-asan/` as `quick-entry-failure.log`,
`native-argument-failure.log` and `plt-alignment-failure.log`.
These are earlier profiles, not failures hidden from the current test result.

## Verification

| Build/evidence directory | Result |
| --- | --- |
| `build/wamr-compiler-library` | Compiler/helper build PASS; fixture generation checks input ownership, malformed input, input/output caps and repeat compilation |
| `build/wamr-probe-initialization` | Classic Release: 56/56 PASS |
| `build/wamr-probe-initialization-fast` | Fast Release: 56/56 PASS |
| `build/wamr-probe-initialization-asan` | Classic ASan/UBSan: 56/56 PASS, halt-on-error |
| `build/wamr-probe-initialization-fast-asan` | Fast ASan/UBSan: 56/56 PASS, halt-on-error |
| `build/wamr-boundaries-host/execution-results.xml` | Renamed execution regression plus WASM-family structure suite: 2/2 CTest PASS (8 structural unit tests) |
| `tests/structure/check_structure.py .` | PASS, 2636 first-party source files scanned |

Each runtime directory contains `results.xml`, `ctest.log` and build logs.
The runtime matrix covers WASM, upstream-compiled AOT and library-compiled AOT.
The nine initialization/loop-cancellation/concurrent-cancellation tests also
passed 100 consecutive runs each in the final classic Release profile (900
executions); see `cancellation-repeat.xml` and `cancellation-repeat.log` there.
Sanitizers instrument the host runtime/bindings, not the generated guest machine
code; guest bounds/stack checks remain enabled independently.
The seven numeric samples and 3165-record corpus agree across interpreter/AOT
and four host rounding modes. This is still a local subset, not proof of full
numeric semantics or cross-platform consensus determinism.

`otool -L` on the Release runtime probe shows only libc++ and libSystem, not
LLVM. The renamed Forge execution target built with compiler/tooling disabled
and `CMAKE_DISABLE_FIND_PACKAGE_LLVM=ON` plus the equivalent Clang setting.
That is source-build evidence, not an installed-package proof.

**Installed-package gate is blocked locally.** Building the complete native
Forge target for installation fails in unchanged `libraries/crypto/pki/x509.cpp`
on OpenSSL 4's opaque `ASN1_OCTET_STRING`. Selecting the installed OpenSSL 3
libraries still selected the machine-wide OpenSSL 4 headers due to include
precedence. The unrelated crypto source and global Homebrew installation were
not changed. Logs are `package-build.log`, `package-build-openssl3.log` and
`package-build-openssl3-pinned.log` under `build/wamr-boundaries-host`.
No fabricated package files or skipped install failures count as acceptance.

Reproduction commands are in [the probe README](../../tests/vm_wamr/README.md).
No commits, pushes, merges, license changes or downstream pins are part of this
work. Build artifacts are local and not a promise of a published binary.

## Still Mandatory

- Public `backend` and `compiled_module` still use EOS. The new compiler is
  private, opt-in and not exported. No claim of complete engine replacement.
- Implement canonical host/intrinsic bindings and typed failure transport;
  do not classify by error strings or unwind C++ exceptions through C frames.
- Enforce the full existing option/limit surface or obtain an explicit reviewed
  disposition for historical permissive parsing. Native stack bytes are not
  logical call-depth/resource accounting.
- Complete the single public invocation deadline/stop scope, including admission
  and host boundaries. Imported start functions and other initialization forms
  need their own compatibility evidence; this test covers a defined WASM start.
- Adapt the pending OC request/artifact/session API without a second worker,
  cache or scheduler. Bound compilation peak memory/time and test process
  cancellation/crash/restart. Input/output byte caps do not prove these limits.
- Reject foreign/tampered native artifacts, retain authenticated original WASM,
  and prove safe invalidation/recompilation. Current fixtures are trusted local
  compiler outputs, not untrusted downloaded AOT code.
- Run donor contracts, SDK/validator/testhost and downstream rollback tests;
  installed/relocated packages; Linux x86-64, ARM64 and full numeric corpus;
  performance and residual-EOS dependency audits. These remain NOT_RUN.

The [mandatory acceptance matrix](../iterations/forge-vm-wasm-wamr-v1.md)
remains in force. Passing the focused matrix cannot authorize a default switch.
