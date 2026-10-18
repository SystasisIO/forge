# Explicit LLVM JIT load boundary

This is a private qualification slice, not an enabled production JIT backend.
It consumes the existing WAMR runtime owner and allocator dispatcher. It does
not link the isolated compiler runtime or load an AOT artifact.

## Hook Contract

- `LoadArgs::defer_jit`, default `false`: `true` loads and validates bytecode
  without initializing either JIT, LLVM compiler context, executable code or
  loader compiler workers. The default retains upstream automatic compilation.
- `wasm_loader_compile_llvm_jit_ex(WASMModule*, const WASMLoadBudget*, uint64,
  char*, uint32)`: private, explicit
  preparation after common admission and before any instances or publication.
  The caller exclusively owns the module. Existing compiler state is refused
  without replacement or recompilation. Unsupported profiles fail rather than select an interpreter,
  fast JIT or AOT substitute.
  Its map limit must be positive and at most the legacy 1 GiB arena ceiling.
  The owner-free three-argument hook deliberately refuses native preparation.
  The published intermediate AOT+defer pin `156dbd0` only contains the refusal
  stub, not the new owner-bearing hook or this dirty ORC implementation. Positive
  JIT tests must not be disabled or counted as native qualification at that pin.
- The explicit path requires LLVM JIT without fast/lazy JIT and caller-thread
  ORC compilation. Removing loader workers alone does not remove the ORC pool.
  The ORC builder uses `setNumCompileThreads(0)`, an in-place dispatcher and
  `setSupportConcurrentCompilation(false)`; default upstream compilation retains
  its existing `WASM_ORC_JIT_COMPILE_THREAD_NUM` workers.
- JIT running mode must be selected and checked before guest initialization.
  A deferred module can be used in interpreter mode without compiling it.

The public load option and mirrored C-API layout are already in the frozen defer
slice. This ORC change does not alter the callback ABI. This directory does not
own the profile, pin, shared backend or root build graph.

## Probe

`load_probe.cpp` uses the existing byte-fixture builder, engine runtime owner,
thread scope and private system linear-memory owner. Cases:

- `deferred`: repeated real bytecode load, metadata present, absent compiler
  state/loader workers, actual interpreter execution and unchanged absence of
  compiler state afterwards. Raw native-mode selection must fail without
  changing the current interpreter mode; subsequent interpreter calls succeed.
- `compile`: explicit preparation, materialized native function pointers,
  refusal to recompile existing state, actual LLVM running mode, interpreter reuse, logical-depth
  denial/restoration, structured unreachable traps, scalar canonical NaN and
  negative zero. A JIT-disabled or fast/lazy profile must reject the hook.
  The fixture crosses two complete upstream group/partition spans plus a
  partial tail and executes every generated function.
- `legacy`: a zero-default load option retains upstream automatic compilation
  in a JIT-enabled configuration. The explicit hook must refuse this existing
  compiler context without destroying it or treating its pool as caller-thread
  preparation.

Register these against the single shared runtime target, with its effective
private include directories and definitions, and a finite external test timeout.
Do not make a second WAMR runtime for the probe. Positive native execution must
run in an explicitly selected LLVM-JIT configuration; a JIT-disabled rejection
pass is not JIT execution qualification. Build registration and execution remain
pending the shared-header/ORC interface and coordinator build slot.

`load_args_probe.cpp` is compiled twice with `FORGE_WAMR_C_API_FIRST=0/1` and
linked with `load_args_main.cpp`. It compares both header-order layouts of
`LoadArgs` and `WASMLoadBudget`, checking all fields and zero-default policies.
It does not link or initialize an engine. Separately, the C-API binary-only
cache now refuses `defer_jit=true` and every nonempty load-budget field, just as
it refuses nonstandard block policy.
The running-mode setter must also refuse an unprepared module's JIT mode,
rather than copy or call null native pointers. Both maintained-fork guards are
part of the frozen defer slice; cache-enabled behavioral qualification remains
separate from the default cache-disabled runtime profile.

## Instrumentation And Support

The existing LLVM lowering calls `forge_instrument_call_depth` after lowering
every function, outside an AOT-only guard. Scalar numeric lowering uses the
same canonical-NaN and constrained-FP implementation for LLVM JIT and AOT.
Thread-manager lowering supplies cancellation checks on calls and backedges.
This slice does not replace those mechanisms. Native cancellation, guest start,
host-error precedence, FP environment, heavy native frames and complete consumer
qualification remain separate execution gates.

Fast JIT is not the fast interpreter: its native backend is x86-64 only in the
reviewed source and lacks the Forge logical-depth/canonical-NaN patches. LLVM
JIT detects the native target, including an AArch64 route, and checks
`LLVMTargetHasJIT`. Source support is not an ARM64 execution pass.

## Reservation Contract

The accepted ceiling covers requested Forge bridge and WAMR storage plus actual
page-rounded owned native code, read-only and writable mappings, including any
mapping overlap. The legacy JIT constructor supplies a 1 GiB ceiling; the bounded
overload can request a smaller ceiling. Admission and native compilation finish
before sealing and publication. Failed physical unmapping must retain the same
owner and charge for explicit cleanup retry.

LLVM IR, optimizer/container heap and transient object buffers are explicitly
excluded. The historical JIT arena did not bound those heaps either. No global
allocator interception, process resource limits, universal OOM guarantee or
second runtime owner is introduced.

The dirty fork slice uses a narrow `JITLinkMemoryManager` adapter and
LLVM's existing `BasicLayout`, not a second linker or section parser. The standard
`MapperJITLinkMemoryManager` reserves code and data together; that cannot preserve
the reviewed Darwin MAP_JIT-code/NX-data split. Each LLVM allocation group instead
receives a separately precharged mapping. Every allocation group's page-rounded
extent, working buffer and map token is admitted before the first actual map
for that object. Native code fixups use a charged NX
working buffer; finalization copies into code and releases that overlap before
publication. Darwin code uses MAP_JIT with supported per-thread write protection
and prior-state restoration; data stays outside MAP_JIT. Other POSIX code uses
checked RW-to-RX protection. No writable-and-executable LLVM section is admitted.

Retained adapter records and unwind action payloads use the existing WAMR runtime
allocator. Failed physical unmaps transfer their still-live charge to the same
load owner's existing retry mechanism. There is no second failed-map registry.
The helper configures an eager `LLJIT` builder with an in-place task dispatcher,
no concurrent compilation, the ordinary LLVM object layer and its normal
platform/unwind setup. RuntimeDyld is unsuitable because map denial can terminate
the process; local lazy resolver/stub mappings must not bypass this manager.
Independent mappings do not guarantee address proximity: the explicit builder
therefore selects Large/PIC codegen and aggressive codegen optimization. The
effective JIT identity must name these settings in addition to the LLVM version,
target, CPU, opt/size levels and caller-thread mode.

The existing WAMR ORC wrapper owns either the real default lazy JIT or the explicit
eager JIT; it does not reinterpret one as the other. Factory errors return the
actual LLVM Error and a null handle; builders and thread-safe modules are
consumed exactly once on success and failure. The optimizer returns `false` with
the existing diagnostic on a bad pipeline instead of calling `ExitOnError`.
Eager codegen measures stack frames before the mapped metadata global is exposed:
a charged WAMR workspace holds the actual upstream measurements until lookup
completes, then copies them into the mutable native global and is released.
Missing measurements reject preparation, never become guessed frame sizes.

`native_maps_probe.cpp` isolates this mapper using real native LLVM IR with code,
read-only data and mutable data. It covers first-segment and aggregate native
ceiling refusal, first/later map-token and protection fault injection,
cancellation, held physical unmapping and subsequent
recovery with a fresh owner. It checks owner-charge restoration and, on Darwin,
actual mapping permissions and restoration of the caller's thread W^X state.
The same process checks invalid-factory Error/null behavior and invalid LLVM
pass-pipeline failure followed by a valid pass run and native execution.
This is not a WASM code-generation, guest trap, or consumer qualification test.
It requires the optional JIT target's exact private includes/definitions and
LLVM headers/linkage; it must not add LLVM to the default runtime test graph.
The first focused execution checkpoint is recorded below; other configurations
remain unqualified until executed with their own source/binary identities.

## Review And Focused Proof Plan

The ORC fork slice was published at `ed5c12ae6bcec88f2863aa7c541b6d829d657a4c`.
Syntax-only checks do not close linking, relocation or execution gates. These
checks must not introduce a second runtime, isolated compiler archive or broad
build:

1. Coordinator registers `test_forge_wamr_jit_native_maps` from
   `native_maps_probe.cpp`, only when `FORGE_ENABLE_VM_WASM_LLVM_JIT` is enabled.
   Reuse the exact private include directories/definitions from the existing
   shared runtime target, add that target's private LLVM includes/linkage and
   the maintained fork compilation include directory. Do not create a second
   runtime. Run serially under an external timeout.
2. Build only the native-maps and existing `test_forge_wamr_jit_load` targets in
   a coordinator-owned warm JIT graph at `-j2`. Run native-maps before the
   deferred/compile/legacy/bridge cases and both-header-order layout test.
3. Inspect the final link map for one C runtime provider and execute the real
   WASM compile/bridge cases. Verify all function pointers, measured frames,
   mode-before-start, logical depth, traps, scalar FP and interpreter reuse.
   Default-OFF refusal is a separate regression, never a positive JIT proof.
4. Native cancellation, heavy live frames, guest start/host-error precedence,
   concurrent module isolation, Linux/x86 and hardened-Darwin qualification
   remain explicit follow-up gates, not source-support claims.

The separately approved `compilation/aot.c` change gives the existing
`aot_error[128]` array thread-local storage using `os_thread_local_attribute`,
with the C11 `_Thread_local` spelling when the platform macro is absent. It
introduces no new diagnostic API, allocator, lock or concurrency profile.
The diagnostic-array isolation probe now has an execution pass, not a claim of
general concurrent compilation qualification.

## First Native Checkpoint

Release, LLVM 22.1.8, Darwin/AArch64, JIT ON, isolated compiler OFF, fork `ed5c12a`:

- The first seven-probe run passed six cases and failed the mapper's final
  factory scenario. `pass_recovery` had run optimizer passes on the very module
  subsequently used to demand three mapping groups. The optimized IR acquired
  `nounwind` and materialized two groups. Those original logs/JUnit remain intact.
- Recovery now uses `LLVMCloneModule`. The probe compares canonical LLVM IR
  before/after recovery to require the mapping fixture to remain unchanged.
  Actual output confirms `nounwind=0->1` on the clone, then three native groups
  for the unchanged factory fixture. No `maps >= 3`, permission, negative-case,
  cleanup or recovery assertion was removed or weakened.
- The corrected seven-probe CTest run passed 7/7, including actual WASM native
  compilation/execution, scalar FP, logical depth/unwind/reuse, deferred and
  legacy behavior, all materialization groups, code/RO/RW maps, denial/cleanup
  recovery, header-order ABI and 64-round diagnostic thread isolation.
- The five follow-up public/runtime CTests passed 5/5: 81 Boost cases and 1,775
  assertions. Invocation includes actual `jit` specializations for cancellation,
  deadlines during start/export, reset/reuse and independent-instance isolation.
  The remaining public suites do not become JIT-specific merely by linking a
  JIT-enabled runtime.
- All 75 WAMR source commands use one effective feature set. Three probe link
  maps identify a single C-runtime provider in `libforge_vm_wasm_interpret.a`,
  with no isolated compiler runtime. LLVM heap remains outside the agreed bound.

Evidence is under the workspace artifact directory
`artifacts/wamr-migration-20261016-r3/native-jit-ed5c12a`: source identities,
initial/final binary hashes, preserved failed-run logs, corrected JUnit, public
runtime JUnit and per-probe link maps. This checkpoint does not qualify the full
32 KiB/depth-1024 live-frame envelope, hardened Darwin, Linux/x86, arbitrary
parallel compilation, installed product consumers or the entire WAMR migration.
