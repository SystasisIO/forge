# WAMR Instruction Limits And Call Depth

Date: 2026-10-16.
Status: private implementation qualified on local Darwin ARM64; full API
replacement remains incomplete. The public-capacity promotion gate FAILS.
This report supplements, not replaces, the immutable earlier qualification.

## Identity

- Branch: `vm-wasm-wamr-experiment-v1`; base
  `8975f234dabf2cbfb91d766bf32194f3d8832a5b` plus pending changes.
- WAMR: clean pinned `ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`.
- Existing four patches remain unchanged. Three new ordered patches:
  `wamr-admission-metrics.patch`,
  `wamr-call-depth.patch`, `wamr-exported-raw-import.patch`.
- Their SHA-256 digests, in order:
  `dce1043a5934f800657b43709e6e7a21adf599ce351cda9f37cf072295dc932d`,
  `e9eec995d8c0c93b279ce27fab6e2a6e2ec4f7bc219bde4f6fc314ddadbea473`,
  `d8820f8fc05fe8ebe25a45065e17a00e1d2c0c05fb3bb2bb12a0fff87cf516a7`.
- Seven-patch/profile digest:
  `5e3bf1f4ab95df9c1ed239a18411f2f5813b75b993a8c134563f87617cd4bbc1`.
- LLVM/Clang 22.1.8, Ninja, `-j4`, Darwin ARM64. Runtime binaries do not
  link LLVM. Compiler remains a separate optional process/library.
- Comparison manifest rechecked unchanged:
  `cdabf9b0de61cbc0dd547c6cab267c24617e9efa29e0c379b4eaaeb54910bb08`.

## Implemented

The interpreter loader exposes observations on its already-validated module:
original function-body bytes, local declaration-set count, logical operand
cells, normal/control/legacy structural depth, branch-table size and memory
offset. No second WASM decoder is introduced.

`engine_limits::check_instructions` consumes the existing public option fields.
All eight combinations of parameter/local/operand accounting are covered.
It takes the maximum of each function's combined accounting, not the sum of
unrelated maxima from different functions. Fast-interpreter physical slots do
not replace logical operand cells. Unreachable bodies and inherited unreachable
control frames do not inflate the logical operand peak. The existing legacy
checker's `else` and cross-function counter behavior are recorded explicitly.

These are static admission metrics, not a newly invented execution-fuel or
blockchain resource-billing model.

Logical call depth has a private counter in the execution environment:

- Classic and fast interpreters count defined function entries and restore
  the incoming count on every normal/error/cancellation return.
- LLVM AOT generation instruments defined function entry and every return,
  before optimization. It therefore does not substitute native frame sizes
  or rely on recursion surviving LLVM optimization.
- Forge's module-local native dispatcher counts imported host calls, including
  direct exports of imports, without double-counting guest entries.
- Deferred initialization now accepts the already-created execution environment,
  so start uses the same configured limit and cancellation ownership.
- A typed overflow flag is read outside C/generated frames. No error-text
  parsing is used for call-depth classification.

The expanded test caught an AOT ABI bug: an exported raw import was called
through the ordinary native-code trampoline, giving the callback an invalid
argument-buffer pointer. The new patch routes that case through WAMR's existing
raw-import dispatcher. This is covered with depth zero and one, not merely a
successful guest-to-import call.

The separate compiler rejection test caught another issue: upstream
`aotclib` used `-fno-rtti`, emitting private standard-exception type information
on this Darwin toolchain. A surrounding `catch (const std::exception&)` could
not match a derived error and the process aborted. The compiler target now
requires RTTI-enabled LLVM and enables RTTI (runtime type information) for its
C++ compilation units. Merely changing the libc++ link path did not fix it;
that configuration experiment was reverted. Negative input now returns a
handled failure instead of aborting.

## Evidence

Final seven-patch build:

| Lane | Result | Local evidence |
| --- | --- | --- |
| Classic interpreter + AOT, Release | 82/82 PASS | `build/wamr-probe-initialization/depth-results.xml` |
| Fast interpreter + AOT, Release | 82/82 PASS | `build/wamr-probe-initialization-fast/depth-results.xml` |
| Classic + AOT, ASan/UBSan | 82/82 PASS | `build/wamr-probe-initialization-asan/depth-results.xml` |
| Fast + AOT, ASan/UBSan | 82/82 PASS | `build/wamr-probe-initialization-fast-asan/depth-results.xml` |
| Two depth cases repeated 100 times each | 200 executions PASS | `build/wamr-probe-initialization/depth-repeat.log` |
| Compiler base-exception rejection | 1/1 PASS | `build/wamr-compiler-library/depth-results.xml` |
| Existing public validation, invocation and execution | 3/3 PASS | `build/wamr-boundaries-host/depth-source-results.xml` |

The public regression tests still exercise the existing EOS-based backend;
they are not evidence of a public WAMR backend. The repository structure check
also passed across 2,647 first-party source files, and `git diff --check` passed.

Each runtime lane includes numeric/rounding/subnormal regressions, ordinary
memory AOT cancellation, module-local bindings and existing memory checks.
Depth tests cover direct/indirect recursion, exact and one-over boundaries,
start, exported imports, host failure, traps, nested independent instances,
an active-call barrier before cancellation, and reuse after failure.
Sanitizers run with halt-on-error; no blanket exclusion was added.

These are same-host results, not x86-64/Linux/Windows or cross-host numeric
acceptance. Logs and XML under `build/` are local artifacts, not committed
evidence substitutes.

## Blocking Capacity Gate

`tests/vm_wamr/capacity_probe.cpp` constructs one valid module with 65,536
`i32` locals, one live operand and `apply(i64,i64,i64)`. Local plus operand
bytes are 262,148, below the public default 1 MiB function limit.

The same source is built in two ways:

1. Root target `wamr_capacity_baseline` uses the actual
   `execution::compiled_module`, unchanged defaults and a real invocation:
   ACCEPTED, exit 0.
2. Standalone target `wamr_capacity_probe` uses WAMR admission:
   REJECTED, exit 1, loader reports `local count too large`.
3. The optional compiler also rejects that original WASM before AOT emission.
   After the RTTI correction it exits 1 through its handler, not SIGABRT.

Evidence: `build/wamr-boundaries-host/capacity-result.log`,
`build/wamr-probe-initialization/capacity-result.log` and
`build/wamr-probe-initialization/capacity-aot-result.log`.
The generated source fixture is `build/wamr-probe-initialization/capacity.wasm`.

This is a failed promotion gate. It is deliberately not a CTest that treats
rejection as success. Reproduce after the builds:

```sh
build/wamr-boundaries-host/tests/vm_wasm_execution/wamr_capacity_baseline
build/wamr-probe-initialization/wamr_capacity_probe build/wamr-probe-initialization/capacity.wasm
build/wamr-compiler-library/wamr_compiler_probe build/wamr-probe-initialization/capacity.wasm build/wamr-probe-initialization/capacity.library_aot
```

Relevant upstream owners include `WASMFunction`, `WASMFuncType`,
`load_function_section`, local offsets and fast-interpreter signed cell
offsets. Wider valid frames require an audited representation change across
loader/runtime/compiler, not just deleting an overflow check or lowering
Forge's public limits.

## Remaining Gates

- Wider accepted ranges and physical frame provisioning; early loader
  allocation bounds; typed allocation/validation/trap distinctions.
- All public parser options, module/context/allocator operations and JIT
  profiling disposition. Current options are not removed or silently ignored.
- Initialization export conventions also require parity review: upstream
  `execute_post_instantiate_functions` recognizes `__post_instantiate`;
  the existing Forge backend executes the declared WASM start. This report
  does not qualify those conventions as interchangeable.
- Production intrinsic registration/converters, whole-invocation deadlines
  and public poison/discard semantics remain unintegrated.
- OC session/cache remains owned by the existing bounded implementation.
  It is not replaced by another queue/cache. New artifact payload/profile
  identity, validation, invalidation/rebuild and executor adaptation are open.
  Raw WAMR AOT must not be accepted as a trusted OC cache artifact.
- Installed consumers, real donor SDK corpus and cross-platform acceptance
  remain required. No product pins, license files or public API were changed.

Consequently, the public backend is still EOS-based. Instruction metric and
depth regression success does not authorize switching production or declaring
the requested migration complete.
