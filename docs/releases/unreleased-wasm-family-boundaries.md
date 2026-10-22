# WASM Family Boundaries

Status: experimental branch, pending changes. No release, product pin update
or production WAMR switch. Maintainer approved this layout on 2026-10-15.

## Ownership

```text
libraries/vm/wasm/
  interpret/    shared WAMR admission/execution, host bindings and neutral utilities
  optimizing/   AOT artifacts/execution and existing compiler-session/cache ownership
  compiler/     optional WASM-to-native compiler; private LLVM dependency
  execution/    bounded Contract SDK action adapter, above neutral VM leaves
```

`vm` and `wasm` remain empty family roots, not aggregate modules/targets.
The `execution` leaf may depend on SDK/Chain values; generic VM leaves must not
import it, Chain or tooling. Compiler algorithms live in the compiler leaf;
the tooling command is only an isolated process wrapper. Interpreter/AOT runtime
components do not require LLVM codegen libraries with
`FORGE_ENABLE_VM_WASM_LLVM_JIT=OFF`; the optional compiler component has its own
LLVM dependency. With runtime JIT enabled,
LLVM 22.1.8 is a runtime link and installed-package dependency even if a consumer
selects interpreter execution. No second scheduler, worker manager, cache or
intrinsic registry is introduced. AGENTS and the structure checker enforce these
boundaries.

## Mechanical Execution Migration

| Before | After |
| --- | --- |
| `libraries/execution/wasm` | `libraries/vm/wasm/execution` |
| `include/forge/execution/wasm` | `include/forge/vm/wasm/execution` |
| `forge.execution.wasm.*` | `forge.vm.wasm.execution.*` |
| `forge::execution::wasm` | `forge::vm::wasm::execution` |
| `forge_execution_wasm` | `forge_vm_wasm_execution` |
| `Forge::forge_execution_wasm` | `Forge::forge_vm_wasm_execution` |
| package component `execution_wasm` | `vm_wasm_execution` |
| `FORGE_EXECUTION_WASM_SDK_FIXTURE` | `FORGE_VM_WASM_EXECUTION_SDK_FIXTURE` |
| `tests/execution_wasm` | `tests/vm_wasm_execution` |
| `test_forge_execution_wasm` | `test_forge_vm_wasm_execution` |

There are no compatibility aliases. Update imports, CMake component/target and
namespace references mechanically. The root old execution dispatcher is gone.
Constructor, `invoke`, limits, results, stop tokens and guest ABI are unchanged
by this rename. This does not authorize a downstream behavioral refactor.
All in-repository consumers, tests and package export names are updated.
Products and the other OC worktree are intentionally untouched.

The current public SDK adapter uses the shared WAMR interpreter backend. WAMR
also owns validation, AOT loading and explicitly enabled LLVM JIT/profiling;
the retired EOS parser, interpreter/visitor/writer and x86_64 code generator are
not current engine paths. Retained donor-derived host/value conversions and
allocation/container/signal/watchdog utilities still execute and retain their
attribution. Engine replacement is not a claim of EOS-free source or binaries,
nor does it establish full compatibility or product acceptance. The approved
dependent API removals are scoped in the
[bounded-profile migration note](unreleased-wamr-bounded-profile.md).

## Compiler State

The compiler API moves from `forge.tooling.wasm_compiler.compiler` /
`forge::tooling::wasm_compiler::compiler` to
`forge.vm.wasm.compiler.compiler` / `forge::vm::wasm::compiler::compiler`.
Request `vm_wasm_compiler` and link `Forge::forge_vm_wasm_compiler` instead of
the old tooling compiler component. There is no compatibility alias or exported
old compiler declaration without a definition. Update imports and namespace
references mechanically; the tooling leaf retains only the isolated command.

The existing request/artifact/session ownership lives in `optimizing`; the
`tooling/wasm_compiler` command wraps the isolated compiler leaf rather than
owning a second compiler implementation or a new product-facing API.

The new `forge_vm_wasm_compiler` target owns a real private WAMR compilation
implementation. It is optional (`FORGE_ENABLE_WASM_COMPILER=OFF` by default),
and is installed/exported as the separate `vm_wasm_compiler` package component
when enabled. Runtime-only consumers do not request this component. Compiler
qualification uses this leaf, not a second implementation inside tooling/tests.

The helper produces owned AOT bytes, validates input/output byte caps and uses
one process-global compiler-state lock. It must run in the isolated compiler
process, not beside the differently configured runtime WAMR instance. The lock
is not a new work scheduler. Peak memory, deadlines, child cancellation and
artifact provenance remain the existing session/worker owner's responsibility.

## Validation

Current local results, source identity and limitations are recorded in the
[qualification follow-up](../donors/forge-wamr-dev-integration-20261018.md).
They distinguish a successful source build from installed-package acceptance,
and private WAMR tests from full public backend/donor-contract acceptance.
The earlier [initialization report](../donors/forge-wamr-initialization-qualification-v1.md)
is historical evidence for its recorded source and backend, not the current pin.
The frozen F6 interpreter/AOT delivery with JIT/compiler/tooling disabled passed
the full build/install, 14 selected runtime tests, two installed AOT/session
tests and a direct interpreter smoke. LLVM/Clang discovery was disabled and
actual resolved runtime dependencies exclude LLVM. Its external full-ON compiler
helper is a separate process. These local F6 results do not qualify the later
publication reconciliation, its new operational oracles or another platform.
The original [baseline manifest](../donors/forge-wamr-baseline-v1.json) is
unchanged; old paths in it and historical reports are intentional.
