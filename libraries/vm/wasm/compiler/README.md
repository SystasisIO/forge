# WASM Compiler

Status: Experimental. Not production-ready.

The optional `forge_vm_wasm_compiler` leaf owns WASM-to-WAMR-AOT compilation.
Enable `FORGE_ENABLE_WASM_COMPILER=ON` together with
`FORGE_ENABLE_VM_WASM_OPTIMIZING=ON`. The public module is
`forge.vm.wasm.compiler.compiler`; `compiler::compile` consumes the existing
owning optimizing `compile_request` and returns its existing `artifact`.
LLVM 22.1.8 and upstream code generation stay private to this compiler leaf.
Optimizing/runtime package consumers do not discover LLVM when
`FORGE_ENABLE_VM_WASM_LLVM_JIT=OFF`. Explicit runtime JIT selection requires
the qualified LLVM package independently of this isolated compiler component.

The [tooling command](../../../tooling/wasm_compiler/README.md) is a thin
stdio/resource-limit adapter. The existing optimizing compiler session remains
the sole bounded scheduler, child reaper and cache owner. No alternative
protocol, parser, scheduler or cache is introduced here.

## Isolation

Use this library only in the isolated compiler helper. Its upstream compiler
runtime contains process-global WAMR state and dummy AOT runtime relocations;
it is not an execution engine. Calls serialize initialization, source loading,
code generation and teardown under one compiler mutex. HELLO-only preflight
does not initialize LLVM or WAMR. Do not link compiler operations into a process
which executes through the interpreter/runtime owner: same-process coexistence,
shared lifetime and ABI-layout safety are NOT qualified.

Artifact serialization is separated from native preparation so the helper can
encode output without pulling the execution runtime into its linked image.
A helper link map must establish a single WAMR C-symbol provider; counting final
symbols alone is insufficient because static-archive resolution can mix objects.
Installing private compiler archives does not authorize mixing runtime owners.

## Admission And Identity

The private helper copies original source before WAMR may mutate it. It uses the
same source-owned capacity type and metadata admission implementation as the
runtime, then emits owned AOT bytes under finite input/output budgets. Source
admission and generated native-frame budgets are different checks. Generated
native frame sizes are read by the common runtime during explicit preparation,
not by loading AOT through the compiler's dummy relocations.

The artifact retains original source and its canonical WASM/configuration
identity. Engine source/profile/capacity, target, imports and codegen version
participate in the identity. Legacy OC artifacts are rejected and must be
rebuilt from WASM. Checksums detect corruption, not correspondence between
untrusted native instructions and source. Only trusted compiler output may be
given executable provenance.

`cmake/ForgeWamrProfile.cmake` owns both source-selection variables and effective
feature definitions. Include it before upstream source selection, then call
`forge_wamr_apply_profile(directory runtime|compiler)` after upstream flags.
The compiler role is the only intentional feature difference in the production
pair. Standalone fast-interpreter qualification may select its mode explicitly.
Memory profiling, thread management and usage-aware allocation are enabled in
both processes. Native frame sizes use the AOT object data section and do not
require `LOAD_CUSTOM_SECTION`. Codegen version 5 rejects earlier layouts.
Fresh effective-flag comparisons and compiler AND runtime rebuilds are required
for this new profile identity. Old binaries must never receive refreshed
identity sidecars. Matching flags do not qualify same-process runtime owners.

Input/output byte budgets do not bound peak LLVM memory or compilation time.
Finite helper process limits, deadline, cancellation, physical reap and explicit
drain remain mandatory. Do not log source, native code or upstream diagnostics.
See the [optimizing limitations](../optimizing/README.md) and
[focused tests](../../../../tests/vm_wasm_optimizing/README.md).

## Raw Wamrc CLI Qualification

The separate upstream `wamrc` CLI is not qualified by the passing library/helper
path. Its serial small-fixture compilation has reproduced exit 139; four fresh
standalone lanes failed while generating raw CLI AOT, so their runtime matrices
were NOT_RUN. A crash report ends in a libLLVM C++ map destructor at process exit;
this is not proof of OOM, parser failure or a libc++ ABI collision.

The maintained-fork fix, published as `2395a935` and included in current pin
`69c107c5`, changes only `wamr-compiler/main.c`:
typed malloc/realloc/free adapters consume the configured usage, realloc boolean
and optional user-data arguments before forwarding pointer/size to libc. It does
not disable usage-aware allocation or change the runtime/code generator.

A controlled ASan baseline uses the original CLI main with the candidate's exact
runtime/codegen archives. It proves an earlier heap-buffer-overflow in
`bh_read_file_to_buffer`: an 89-byte read into the one-byte allocation returned
for `malloc(0)`. The requested size was displaced by usage argument zero. A second
343-byte input reproduces the same first invalid write. This precedes LLVM
destruction; the terminal destructor crash is not evidence of a separate LLVM
ABI defect. No separate exit failure reproduced after the adapter fix in the
focused Release and ASan/function-sanitized runs.

Both candidate lanes pass 16 serial real WASM-to-AOT compilations and 32 normal
failures with deterministic recovery. The original 89-byte start and 1760-byte
standalone fixture also compile successfully in Release. Actual commands agree
on 49 explicit `WASM_*` definitions across CLI, library compiler and runtime;
only the runtime/compiler role differs. These are local candidate results, not
fresh qualification of the current published pin or the previously blocked runtime matrices.
See [CLI qualification](../../../../tests/vm_wamr/compiler/cli/README.md) for
source/binary hashes, controls, reproduction and evidence paths. Maintained-fork
publication and pin update are complete for this fix; fresh standalone CLI
matrices remain required. Library/helper results do not qualify that separate route.
