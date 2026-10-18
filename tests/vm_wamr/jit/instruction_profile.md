# Native instruction profiling

## Runtime Feature Source Freeze v3

Status: **source implemented; complete-runtime configure/build/execution
NOT_RUN**. The earlier v2 private proof below is historical evidence, not a
result for this feature or the concurrent export-name ABI changes. No pin or
vendor archive is modified by this workstream.

The private entry is now
`wasm_loader_compile_llvm_jit_profile_ex(WASMModule*, const WASMLoadBudget*,
uint64 max_native_bytes, char*, uint32)`. The existing `_ex` is the plain-JIT
wrapper of the same internal implementation. Neither `LoadArgs` nor the WAMR
public export/runtime-common API acquires a profiling flag. The existing eager,
caller-thread compiler, module and compctx remain the only owners.

The profile entry enables instruction metadata before `aot_compile_wasm`.
Native emission builds the map privately. Only the successful loader's final
stack/function checks, owner check and `aot_jit_profile_publish` expose it to
`aot_jit_profile_intervals/translate`. These observers allocate nothing and
require the caller to retain the module/compctx and exclude compilation/unload.
A module with zero defined functions succeeds without a compiler context and
has an empty map; every PC is unknown, including import-only modules.

Early refusal discards an unfinished `DIBuilder` while its LLVM module/context
still exist. The remaining profile/plugin storage survives until ORC resource
teardown, then is freed through the original WAMR allocation owner. No failure
publishes a partial map, synthesizes function-only locations or switches to
another execution engine. A failed owner remains unavailable; fresh-owner
recovery must compile the same source successfully. LLVM's own transient
IR/optimizer/decoder/object heap is explicitly outside the accepted ceiling.

`iwasm_compl.cmake` includes the three compiler translation units exactly once
in its existing source list. Both `runtime_lib.cmake` (LLVM-JIT runtime) and
`wamr-compiler/CMakeLists.txt` (`aotclib`) consume that list. The test fragment
now links **only the complete shared runtime**, checks exactly-one source
membership and compares candidate/runtime sources and headers. It refuses the
old candidate-object-over-old-archive technique: the concurrent export-name
layout change makes that unsafe. No second runtime or compiler implementation
is linked into the probe.

Eight coordinator-sequenced subprocess cases are registered:

- `runtime`: real loader preparation and LLVM-mode execution, original opcode
  intervals, unknown host/prologue/gaps, independent simultaneously live modules,
  cross-thread destruction/refund, and real native SIGPROF samples.
- `plain`: `_ex` executes real native code without enabling a profile/map.
- `empty`: empty and import-only modules have zero defined functions, successful
  repeated preparation, no unnecessary compiler context, and unknown PCs.
- `multi`: two defined functions, direct guest call, original offsets from both
  function bodies and an exact native result.
- `early_heap`: after metadata-builder construction but before its first
  owner-allocated function record, reserve the owner's remaining capacity so
  the real allocation is refused. No native mapping may be attempted.
- `early_cancel`: cancel at the same pre-finalization point, also before any
  native mapping.
- `late_cancel`: cancel after materialization/stack lookup but before loader
  publication; the already-built map must remain unobservable and be refunded.
- `mapping`: refuse the first native mapping token, with no partial map or
  retained executable compiler state.

Each failure case checks the typed owner category, exact refund to the loaded
module baseline, rejected retry with that unavailable owner, immutable caller
input, complete unload, and a positive fresh-owner retry. No error-message
classification, limit relaxation or skipped assertion is used.

The qualified bounded path deliberately marks both guest bodies and their
prechecks `noinline` to preserve measured frames/native-stack safety. The
multi-function test checks those attributes while the IR is still alive. It
does not force unsafe guest inlining or claim genuine inlined-guest coverage;
that configuration is not reachable in the current production profile. LLVM
still performs its ordinary optimization/lowering, so removed instruction
locations remain unknown rather than inheriting a neighboring row.

After the parent publishes and pins the **whole reviewed fork**, reconfigure
the warm graph with its existing candidate option, then request the heavy slot:

```sh
cmake --build build/wamr-dev-r4 --target test_forge_wamr_jit_instruction_profile -j2
ctest --test-dir build/wamr-dev-r4 \
  -R '^test_forge_wamr_jit_instruction_profile($|_)' \
  --output-on-failure --output-junit instruction-profile-runtime-v3.xml
```

Use new v3 log/XML names; do not overwrite v2/earlier failed evidence. Inspect
the emitted link map for exactly one WAMR C provider and compiler/profile
objects coming from the one runtime archive. The normal compiler/helper target
must also be rebuilt after the pin, since `aotclib` now includes the profile TU.
`source-freeze-instruction-profile-v3.json` records this source-only boundary.
Public Forge debug-owner lifetime and consumer acceptance remain Carver/parent
gates, not claims made by these private probes.

## Historical v2 Private Proof

Status: **Darwin ARM64 private native proof PASS**, plus three repeat passes.
Independent review of frozen v2 found no P1/P2 source defects in this private
proof. Public `jit_profile` integration remains pending. Base WAMR commit:
`ed5c12ae6bcec88f2863aa7c541b6d829d657a4c`. Unpublished changes live only in the
maintained fork; the vendor, source selector, public Forge modules and backend
are unchanged. This compiler-level proof is not completed `jit_profile`
consumer integration.

## Mechanism

`aot_jit_profile_enable(AOTCompContext*)` is a private opt-in before opcode
translation on the existing bounded, eager, caller-thread LLVM JIT. It attaches
LLVM compile-unit/subprogram metadata, without requiring source debug sections
in the WASM input or using LLDB. The existing opcode translator sets LLVM
locations whose line is **original WASM file offset + 1**. Line zero identifies
compiler-generated code. Optimized-out opcodes need not have native ranges.

An object-layer plugin preserves debug sections as `NoAlloc`, including Darwin
Mach-O `__DWARF` sections. JITLink applies the relocations. The post-fixup adapter
passes those relocated section bytes to LLVM `DWARFContext`; it does not decode
DWARF or relocate native PCs itself. Native intervals are clipped to actual
linked guest-function symbols. Every lookup uses full native addresses and a
half-open interval, not a common arena base or a floor lookup across gaps.

The immutable map is available only after successful emission. Translation
does not allocate, lock, invoke LLVM, or classify PCs by function name. Host
PCs, compiler prologues, line-zero rows, unmapped gaps and foreign-object PCs
must remain unknown (`UINT32_MAX`). Compiler/map/plugin records and adapter
buffers use the existing WAMR owner. LLVM's own IR, optimizer, decoder and
object scratch heap remains outside the accepted preparation ceiling. Temporary
adapter buffers are released after emission, and map ownership ends with the
compiler/native object.

## Probe

`instruction_profile_fixture.hxx` builds a stripped WASM function containing a
loop-carried multiply/xor/checked-division recurrence. Fixture construction records opcode starts
directly; it introduces no second parser. A non-debug custom padding section
makes original-file offsets observably different from Code-relative offsets.

`instruction_profile_probe.cpp` requires:

- At least three distinct original-file offsets within the same native guest
  function, with exact native interval boundary checks and unknown prologue,
  host and outside PCs.
- Two concurrently live native objects on the **same runtime owner**, with
  independent maps. Each rejects the other's PCs; destroying one preserves the
  other's map and returns its owner charge. No contiguous arena is assumed.
- Genuine native execution via the upstream WAMR invocation trampoline and an
  ordinary WAMR execution environment, including an exact result check. No
  handcrafted native calling convention or loader-function tables are added.
- Real timer-delivered native PC samples on Darwin ARM64/x86-64 or Linux
  ARM64/x86-64, resolving at least two distinct same-function WASM offsets. The
  exclusive subprocess restores its signal handler/timer. No production sampler
  is introduced by this probe.
- Injected mapping refusal publishes no partial map, preserves the typed load
  owner failure, returns all owner charges on cleanup, and permits a subsequent
  positive trial with a fresh bounded owner. Caller WASM bytes stay immutable.

## Bounded build request

The parent may explicitly include `instruction_profile.cmake` in its warm JIT
test graph and call:

```cmake
forge_add_wamr_instruction_profile_probe("${FORGE_WAMR_INSTRUCTION_PROFILE_CANDIDATE}")
```

The fragment compiles only the candidate `aot_compiler.c`, `aot_llvm.c`,
`aot_jit_profile.cpp` and the probe. It reuses the existing runtime/LLVM graph,
does not change the source selector or identity of existing binaries, and emits
its own candidate identity and link map. It deliberately refuses a different
base revision. The private context field is trailing; existing member offsets
are unchanged, and its sole allocator is among the candidate objects.

Build/run remains coordinator-sequenced at `-j2`. Retain configure/build/CTest
logs, JUnit, binary SHA256, candidate hashes and the link map. The link map must
show one runtime C provider and the candidate objects providing compiler/profile
symbols. No second `vmlib`/isolated compiler runtime may be pulled into the probe.

After this scoped review, the next slice can make the existing loader accept a profiling
flag and expose the immutable map through its existing module owner. Forge
public/backend signatures and effective cache identity are a later coordinated
slice, not implied by this private hook.

## Executed Checkpoint

Warm graph: `build/wamr-dev-r4`, Release, Darwin ARM64, LLVM 22.1.8, JIT ON,
fast/lazy JIT OFF, shared runtime usage allocator ON. Build command:

```sh
cmake --build build/wamr-dev-r4 --target test_forge_wamr_jit_instruction_profile -j2
```

Final compiler candidate identity:
`c20af97eb22e97eb9dc2cf6fc5d20a08483dd801babcac1e342a9fc101ec248e`.
This identifies the five compiler files and shared runtime identity, not the
fixture revision; the v2 freeze separately hashes the fixture, probe and binary.

- Final build: `build/wamr-dev-r4/instruction-profile-build-r4-dev-07.log`.
- First complete pass: `instruction-profile-dev-05.xml` and
  `instruction-profile-ctest-dev-05.log` in that build directory: 1/1 PASS,
  5 native intervals, 3 original-file offsets (`0x163`, `0x16a`, `0x16d`),
  299 real signal samples covering all three offsets. Two concurrently live
  objects, host/prologue/gap rejection, injected denial, exact result and
  complete cleanup assertions all passed.
- Repeat run: `instruction-profile-dev-repeat-01.xml` and
  `instruction-profile-ctest-dev-repeat-01.log`: three more subprocess passes,
  samples 275/240/270, each covering the same three original-file offsets.
- Link map: `tests/vm_wasm_execution/test_forge_wamr_jit_instruction_profile.linkmap`
  under that build directory. Candidate objects [2]/[3]/[4] supply compilation,
  compiler-context creation and translation. One shared archive member [28],
  `libforge_vm_wasm_interpret.a(wasm_runtime_common.c.o)`, supplies `full_init`,
  `load_ex`, `unload`, `destroy`; each appears once. No vmlib/aotclib or isolated
  compiler runtime is linked. Symbol check: `instruction-profile-symbols-dev-01.log`.

Earlier failures are retained, not replaced: parent `instruction-profile-build-r4.log`
(Apple ucontext include), developer build logs 01 (LLVM SymbolStringPtr API),
and CTest XML/logs 01 through 04 (insufficient distinct mapped offsets in the
earlier arithmetic-only/proven-nonzero fixtures). The probe now includes the
Darwin type header `sys/ucontext.h`, without global feature-test macros. Its
fixture type is named `image`, avoiding a dependency scanner misinterpreting
`module result;` as a named-module declaration.

The IR diagnostics `instruction-profile-ir-diagnostic-dev-01.log` and `-02.log`
show multiple opcode locations before/after IR optimization. The preserved
diagnostic objects `tests/vm_wasm_execution/profile-object-dev-01-1.o` and `-2.o`
show that later LLVM codegen can drop locations for moved arithmetic. Those
instructions correctly remain unknown. This proof does **not** promise a row
for every optimized WASM opcode or fabricate an offset for such native gaps.
The temporary object-dump callback and compiler printf diagnostics were removed
before the final passing build. No assertions or thresholds were weakened.

## Historical v2 Interface Proposal

Keep the consumer surface `backend<..., jit_profile, ...>::get_debug()` and
`profile_instr_map::translate(const void*) const -> uint32_t`, including unknown
`UINT32_MAX`. The internal map must use absolute, half-open native intervals;
the old single-arena/Code-section comments and implementation are not applicable.

Add one private profiling flag to the existing loader compile implementation,
calling `aot_jit_profile_enable` after context creation and before
`aot_compile_wasm`. Existing plain-JIT entry behavior stays unchanged. A private
owner observer can forward translation to the context-owned immutable map;
there is no need for another parser, sampler, compiler, allocator or map copy.
The prepared module must pin that owner before exposing the existing debug
observer, and preparation/cache identity must include the effective profiling
flag. Exact Forge declaration changes remain coordinator/Carver-owned and are
not implemented by this checkpoint. Fresh whole-fork and consumer execution
after publication remain separate gates.

## Historical v2 Review And Remaining Gates

Review verified all eight v2 source hashes, the retained executable and link
map, and the passing logs. It confirmed use of the existing translator,
JITLink relocations and LLVM DWARF decoding, the single runtime provider, and
the immutable interval lookup. No P1/P2 source finding was identified in this
scope; this is not approval of public integration or unrestricted profiling.

- Add owner-heap refusal and cancellation before metadata finalization. The
  current injected mapping refusal occurs later. Require no published map,
  complete refund and a successful fresh retry for each failure point.
- Prove that the public debug observer pins its map owner while sampling and
  cannot race with teardown. The private raw-context lookup relies on its
  caller retaining the context.
- Add multi-function/inlining and cross-thread destruction coverage.
- Requalify the full maintained fork and public backend after the reviewed
  source is actually pinned. This probe still leaves the production pin intact.
