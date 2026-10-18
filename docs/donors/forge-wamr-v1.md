# WAMR Donor And Comparison Boundaries

Status: exploratory qualification, not migration acceptance.

## Dependency Pin

- Repository: <https://github.com/wasm-micro-runtime/wasm-micro-runtime>.
- Pinned development revision: `ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`.
- Source: root `vendor/wamr` submodule; no local upstream patches.
- The development pin currently reports `wamrc 2.4.3`. That version string is
  not the identity of this experiment; use the complete commit above.
- License: Apache-2.0 WITH LLVM-exception, as retained in the dependency.
- The stable 2.4.5 tag (`25bd7eb63e828e4bd242cc9b38d260b4b31c6605`)
  failed a local `wamrc` build against LLVM 22.1.8: the removed
  `LLVMOrcThreadSafeContextGetContext`, changed `PGOOptions` constructor and
  `Target::createTargetMachine` signature. The selected upstream revision
  already contains the corresponding version guards. This is a development
  pin, not a claim that stable 2.4.5 supports the current Forge toolchain.

## Files Inspected

| Upstream file | Accepted use | Limit or follow-up |
| --- | --- | --- |
| `build-scripts/runtime_lib.cmake`, `config_common.cmake` | Build interpreter and AOT loader without JIT/LLVM | Disable WASI, guest threads, SIMD, mini loader and extra features explicitly for the initial probe; final admission policy still needed |
| `core/iwasm/include/wasm_export.h` | Load/instantiate/call, host registration, bounds checks, instance termination | Public runtime errors are largely text; do not parse messages into Forge error kinds |
| `core/iwasm/common/wasm_runtime_common.c` | Upstream dispatch and lifecycle | Instantiation may execute start before a caller-owned execution environment is available; cancellation coverage remains open |
| `core/iwasm/interpreter/wasm_interp_classic.c` | Baseline interpretation | Numeric/environment behavior must be checked, not assumed |
| `core/iwasm/compilation/aot_llvm*.{c,cpp}` | LLVM lowering owned by upstream | Compiler/toolchain compatibility is distinct from runtime compatibility |
| `core/iwasm/compilation/aot_emit_control.c` | Upstream cooperative interruption checks | `check_suspend_flags` returns without emitting checks for non-shared memory; the ordinary-memory AOT loop test times out |
| `wamr-compiler/CMakeLists.txt`, `main.c` | Separate `wamrc` process and explicit code-generation options | No implicit host CPU policy in production artifacts; compiler provenance and admission checks remain Forge-owned |

The initial host tests use software bounds checks. This does not establish
equivalence of native stack limits across platforms or qualify upstream signal
handlers. The compiler's `--enable-multi-thread` enables the thread-manager
code-generation option and associated compiler capabilities, but it does not
emit loop interruption checks for ordinary memory. Guest threading remains
disabled in the test runtime. Rewriting contracts to shared memory merely to
pass the interruption test is not an accepted compatibility solution.

## Existing Forge Work

The [baseline inventory](forge-wamr-baseline-v1.json) records actual contents of
the dirty `vm-wasm-optimizing-b40-v1` worktree, including untracked OC files.
`tests/vm_wamr/capture_baseline.py` creates the inventory and an optional local
ZIP snapshot, and `--check` detects later source drift. The source directory
is never modified. Unrelated pending DB/network changes are not imported.

Retain the design and tests for typed failures, exclusive invocation state,
bounded compiler sessions, identity-checked native artifacts and shutdown.
Do not copy EOS-derived parser/conversion/numeric implementation into a new
namespace and call that a license-independent replacement. Some OC code has
separate Spring/WAVM provenance; audit by component rather than assigning one
license to every file. This experiment does not change any existing license.

## Evidence Limits

The standalone probes exercise upstream mechanisms. They do not implement a
Forge runtime adapter, establish full FP determinism, validate all donor
contracts, or certify product readiness. See the full
[acceptance matrix](../iterations/forge-vm-wasm-wamr-v1.md).
