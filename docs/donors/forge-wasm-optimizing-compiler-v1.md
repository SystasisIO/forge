# LLVM compiler: first production vertical slice

This slice is the real `forge_tooling_wasm_compiler` owner, not the earlier
isolated feasibility executable. Its public `compiler::compile` accepts the
shared owned request and produces the accepted artifact value. The runtime
mapping/executor/session and broader lowering are separate gates.

## File-level provenance

Spring pin: `e6a99f68b67abc4d89fe716755b2e1394a4991f7`. Exact per-file
hashes, target files and adaptation classification live in the checked
[optimizing source manifest](../../tests/vm_wasm/optimizing_source_manifest.json).
The existing source verifier has a separate opt-in mode for these inputs;
ordinary EOS VM verification remains unchanged. Spring bytes are read with
`git show <pin>:<path>`, not from possibly dirty working-tree files.

| Donor source | Actual adaptation | New owner |
|---|---|---|
| Spring `LLVMEmitIR.cpp` | Integer wrapping/explicit traps; signed-remainder exceptional-path PHI; masked shifts; typed branch/result merge and LLVM switch; unsigned memory offsets, Align(1) volatile raw loads/stores, global/growth patterns, newly expressed against Forge opcodes and ABI | `lowering.cpp` |
| Spring `LLVMJIT.cpp` | PIC generation and fixed `.stack_sizes` completeness/budget algorithm, newly expressed with LLVM 22 ELF APIs | `lowering.cpp`, `object_image.cpp` |
| EOS VM parser/writer/types/options/opcodes | Existing accepted Forge implementations reused through the exported parser alias and full preparation; no new parser or copied frontend | `compiler.cpp` |
| LLVM 22 `IRBuilder.h`, `Intrinsics.h`, `ELFObjectFile.h`, `LEB128.h` | Private dependency/public API use, not copied implementation | `lowering.cpp`, `object_image.cpp` |

No Spring/WAVM file is copied wholesale. There is no ORC, WAVM IR model,
donor namespace, FC framework, GS register or shared-code interruption port.
The semantic and fixed-frame algorithms are adaptations, not a claim of a
line-for-line donor port. `compiler.cpp` metadata traversal, shared-ABI offset
access and ELF-local relocation normalization are new Forge integration code.
Notice text is copied/reflowed into [Spring](../../libraries/tooling/wasm_compiler/LICENSE.Spring)
and [WAVM](../../libraries/tooling/wasm_compiler/LICENSE.WAVM) license files;
binary distribution must retain those notices. LLVM remains an external
Apache-2.0-with-LLVM-exception dependency. Existing EOS VM source/license
manifests remain the source of truth for the unchanged frontend.

The integer/CFG adaptation specifically inspected pinned Spring
`LLVMEmitIR.cpp` lines 570–670 (branches/result arguments/switch), 995–1063
(signed remainder and shifts/rotations), 1065–1075 (division guards) and
the following integer comparisons/count intrinsics. The `MIN % -1` branch
and PHI algorithm is newly expressed against the shared trap ABI; LLVM 22
funnel-shift intrinsics replace the donor's paired masked-shift rotation
expression. No donor operator decoder or control-stack model is copied.

The separate memory/global adaptation inspected the same pinned file's
307–325 (unsigned i32 address/offset widening), 871–901 (global access and
memory size/growth), and 921–966 (raw integer/bit loads/stores, sign/zero
extension, truncation, volatile and Align(1)). There is no GS/address-space-256
or donor intrinsic-registry port. Unlike the donor's guarded-address-space
inbounds GEP, Forge checks the full access extent against invocation bytes
before forming a non-inbounds byte GEP. The existing canonical writer's
`flags_align, offset` fields are consumed directly: its historically reversed
parameter names are not a new decoder contract. Parser admission remains in
the existing global/memarg/memory cases; no second validator is introduced.

`control_flow.cpp` and its paired private `.hxx` are new Forge integration
code over the **existing parser output**, not a second WASM parser. They use
the writer's module-wide PC convention, its high-bit result-preserving
branch pop count and its in-place `br_table` payload extent/default entry.
Payload slots are never read as opcode variants. A worklist propagates one
temporary-operand height per reachable instruction with exact merge checks;
it does not duplicate the canonical operand-type stack or admission policy.
Module bases accumulate once, avoiding quadratic rescans across functions.
Runtime operand slots are fixed entry allocas; LLVM O2 can promote their
merges to SSA. Branch transfer moves a preserved raw result over discarded
temporaries before entering the destination.

## Exact execution contract

The sole ABI source is `invocation_abi`: context offsets use `offsetof`, callback
IR signatures derive from C++ function-pointer types, and raw operands/results
use the existing eight-byte `expr_value`. No independently typed LLVM context
prefix is maintained. Each entry polls cancellation/deadline, admits depth and
its checked function-frame budget, then restores both counters on normal return.
Every actual backward edge also polls, including branch-table destinations;
a loop without calls cannot evade the mandatory deadline/cancellation check.
Traps terminate the guest path through the owner callback, never a C++ exception
unwind across generated native frames.

The normalizer admits ELF64 little-endian x86-64 and AArch64 relocatable output,
immutable code/readonly storage and a closed image-local relocation whitelist.
Unknown/external references, mutable/TLS storage and missing/duplicate/unmatched
frame metadata reject before artifact construction. The frame budget includes
LLVM's fixed frame plus the incoming x86-64 return address (eight bytes);
generated entries account this full per-call consumption. LLVM red-zone and
tail-call optimizations are disabled, so that accounting is not bypassed.
For AArch64, LLVM's fixed frame already includes any saved link register and
must be a multiple of 16; no x86 return-address adjustment is added.
The machine prologue allocates its physical frame before the generated budget
checks run. Those checks cannot replace the runtime's guard pages or prove
callback-stack headroom. Poll/trap callbacks also require the admitted host-stack
bounce before any C++ callback frames are constructed.
Invocation-owned guarded aggregate stack and measured callback bounce reserve
remain runtime-owner responsibilities, not proven by these code-generation tests.

### Explicit Darwin ABI with an ELF container

Wasmtime platform pin: `05e7deebf1ddcc726bfc4c30cd87e3dbb2881072`.
`crates/environ/src/compile/mod.rs` lines 313–360 emits ELF independently of
the target operating system. `crates/wasmtime/src/runtime/code_memory.rs` and
`runtime/vm/sys/unix/mmap.rs` provide the single-owned mapping/publication and
RW-to-RX pattern. `crates/jit-icache-coherence/src/libc/aarch64_apple.rs` uses
the public libSystem `sys_icache_invalidate` API; its `libc.rs` companion needs
no additional cross-thread pipeline flush on Darwin. These are inspected
integration patterns, with no Rust implementation copied and no Wasmtime
dependency, namespace, runtime or cache added to Forge. The donor license is
Apache-2.0 WITH LLVM-exception; source hashes are in the existing manifest.

The exact Darwin profile is `aarch64-apple-macosx11.0.0-elf`, `generic`,
`+fp-armv8,+neon`, LLVM 22.1.8, codegen version 1 and the existing host ABI.
The LLVM 22.1.8 [AArch64 subtarget](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/lib/Target/AArch64/AArch64Subtarget.h#L278)
selects Darwin by OS independently of ELF/Mach-O.
[Call lowering](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/lib/Target/AArch64/AArch64ISelLowering.cpp#L7776)
selects DarwinPCS for ordinary C calls. The bounded `llc` probe emits
`EM_AARCH64`, `.stack_sizes` and actual ABS64, PREL32, CALL26/JUMP26,
ADR_PREL_PG_HI21, ADD_ABS_LO12_NC and scalar LDST8/16/32/64_ABS_LO12_NC
relocations. An ELF OS/ABI header does not authenticate the calling convention;
the full canonical target profile remains in the artifact identity.

`object_image` normalizes those real relocation forms into the existing
artifact. `artifact` checks source/target extents, instruction opcode, 4-byte
instruction alignment, load/store scale, branch/page range and patch overlap
before construction. The fixed 64KiB image granule makes ADRP page deltas
independent of the actual mapping address. `native_image` rejects a wrong
host OS/architecture before allocation, patches the existing mapping while
RW, performs public Darwin instruction-cache maintenance before RX, and
publishes the same immutable prepared-image owner. It introduces no Mach-O
loader, MAP_JIT arena, remapping cache or entitlement policy. Hardened-runtime
packaging and Mach fault handling remain separate gates.

### Cancellation cost remains an open executor contract

Current lowering calls the indirect `poll` callback at function entry and
every taken backward edge, including branch-table edges. This slice keeps
that bounded cancellation behavior unchanged. If the executor callback
switches from guest to host stack on every poll, a tight loop pays that cost
per iteration; code-generation fixtures establish no performance acceptance.
The present ABI has no inline epoch/guard-page field, so a donor-shaped cheap
fast path requires an explicit coordinated ABI/codegen version change.
A Wasmtime-style epoch comparison or Spring-style invocation-local guard page
could load a small invocation-owned deadline/cancellation state and enter the
existing slow callback only when interrupted. The interpreter's watchdog
owner must remain the sole stop/deadline authority; shared immutable code
must not become inaccessible for cancellation. Frequency reduction is not
an accepted substitute. Joint compiler/runtime review and measurements of
tight loops, host calls and node/compiler contention remain required.

Only the current execution snapshot is parsed; product setcode/admission limits
are not imported or revalidated. Parser/preparation failures become typed local
invalid-module results, resource failures local compiler limits. Valid operations
not yet implemented become compiler unavailability, never a silent interpreter
fallback or an invalid historical-contract verdict. Source bytes are unlogged.

The integer slice emits all MVP i32/i64 comparisons, counting operations,
arithmetic/bitwise/shift/rotate operators and integer-only conversions.
Wrapping operations carry no LLVM no-wrap flags. Division checks zero before
LLVM division/remainder and signed division checks `MIN / -1`; signed
remainder skips LLVM's poisonous `MIN % -1` evaluation and produces zero.
Shift counts are masked to word width; count-zero intrinsics explicitly
define their zero-input result. The separate memory draft lowers all MVP
loads/stores, memory.size/grow and global access. Float memory/global operations
only move raw i32/i64 bits; no native FP arithmetic, new SoftFloat implementation
or numeric ABI numbering is introduced. The later FP slice extends raw
signatures/locals/constants and dispatches numeric operators to the existing
shared callback owner. The subsequent import/indirect source slice below uses
that same callback/context boundary; whole-module/executor readiness remains
separate from opcode lowering support.

Guest address and immediate offset are both zero-extended from i32 to i64;
their sum plus up to eight access bytes cannot overflow i64. The complete
extent is checked before a guest pointer is formed, so address+offset cannot
wrap into valid memory. Alignment hints never become LLVM alignment promises:
guest loads/stores retain volatile access and Align(1), on the already checked
little-endian target. Signed loads extend only after the narrow load; stores
truncate before writing. Globals use the existing eight-byte expr_value slots
and parsed content width; their initial values and mutability remain existing
artifact/parser records, and mutable bytes belong to each invocation.

memory.size loads current_pages. memory.grow invokes the existing ABI callback
and returns its old-pages/-1 raw i32 result. Every later memory operation reloads
base and extent from the context; no base is cached across a generated call or
callback. The runtime growth owner enforces the minimum of declared maximum
and artifact execution max_pages, zero-fills admitted pages and publishes the
refreshed context. The compiler does not add another limit snapshot/allocator.

## Current evidence and remaining gates

### Separate import/indirect source slice

Pinned Spring `LLVMEmitIR.cpp` lines 696–855 supply the inspected call/indirect
pattern: ordered operands, unsigned table bounds, signature check before call,
and mixed imported/defined destinations. Forge does not adopt the donor's GS
intrinsic pointers, negative host offsets, native typed FP arguments, alternate
registry, memcpy specialization or WAVM type model. Every imported function
instead has an ordinary generated entry thunk that shares depth/frame/poll
admission and invokes the existing raw `host_call` ABI. Returned typed callback
reasons terminate the guest path before consuming the result.

The existing parser/finalization owner alone establishes structural type
equivalence through `type_aliases` and `fast_functions`. The compiler compacts
those representatives into the existing artifact signature table; it does not
compare types again or introduce a parallel canonicalizer. Declared duplicate
signatures share a compact artifact index, which both invocation table entries
and prepared native-function metadata must carry. Module function indexes still
include imports; defined-code PCs still count only the canonical defined bodies.
Bindings require exact module/name/signature in sorted `configuration.imports`;
the callback ordinal is that configuration index, not `import_functions` or a
process registry index. Missing/mismatched bindings yield `unsupported_import`.

Indirect lowering checks unsigned selector bounds before table-slot access,
then null function index, table signature, function-table bounds, native
signature and null entry before invoking the existing entry-function ABI. Null/
out-of-range indexes produce `table_bounds`; mismatched signatures/null native
entries produce `indirect_type`. Metadata belongs to the admitted executor:
the compiler adds no table owner, host registry, invocation layout or parser.

Tests add mixed imported/defined destinations, reversed binding order, duplicate
structural declarations, ordered arguments, raw FP import bits, no-argument/
void imported entries and imported start metadata. Portable tests exercise both
canonical target profiles and exact Raw roundtrips. Native positive probes use
distinct original/decoded mappings on an ordinary host stack. Nine fresh-exec
negative controls require exact typed exit and zero host effects, rejecting
hardware signals, timeouts, wrong exit or native return. They include OOB with a
null table base, null slot, wrong structural type, OOB function index, mismatched
native type, null native entry, and direct/indirect host callback failure reasons.
These are software-guard primitives, not Mach recovery or production bridge tests.

The sole test-process helper is the runtime owner's shared `fresh_process.hpp`
snapshot `357106df9ff0e5ce6937f6081017d9c78d23b8aec5d9c89c69ee10bdaf6de6c7`.
It uses public fresh exec, child core suppression and bounded kill/reap under a
controlled test environment without competing reapers/SIGCHLD auto-reap and
with finite EINTR. It is reused byte-for-byte, not a second process harness;
the complete runtime owner remains pending acceptance. ECHILD failure injection,
actual Mach/guest-host callbacks, Linux native, session/cache/helper packaging,
host registry semantics and performance remain NOT_RUN for this compiler slice.
All 172 MVP input opcodes now have source lowering, but unsupported frontend
features, module instantiation, runtime ownership and end-to-end execution gates
are not opcode-count claims. The existing auxiliary compiler target built with
`-j4` without warnings; all three CTest targets passed in 3.99 seconds (10
artifact, 18 compiler and four shared numeric-owner cases). The Darwin mixed
positive primitives and all nine fresh-exec software-guard controls ran. Four
structure tests and pinned donor/LLVM provenance checks also passed. Exact
source/preimages/log hashes are frozen separately from the FP slice; no new
graph, root/package build, CI or broad runtime acceptance is implied.

The separate FP transport slice preserves f32/f64 constants, parameters,
results, locals, globals, memory and control-flow merges as raw 32/64-bit
integers. The 62 FP/comparison/conversion operations lower to the existing
numeric callback with the canonical opcode enum and one/two ordered eight-byte
slots; a returned typed trap ends the guest path through the existing trap
callback. The compiler's shared private `control_flow::numeric_arity` supplies
both height analysis and callback shape. It introduces no parser/type checker
or independent opcode numbering. LLVM floating types, arithmetic, comparisons
and conversion intrinsics are absent from this emitter.

Pinned Spring `LLVMEmitIR.cpp` lines 1150–1190 provide a runtime-intrinsic
fallback pattern for operations whose semantics differ from LLVM. Forge applies
the existing shared `interpret::execute_numeric` owner to **all** FP operations;
the donor's native LLVM FP arithmetic is deliberately not adopted. That owner
dispatches the unchanged interpreter overloads/private SoftFloat helpers,
including NaN/signed-zero and conversion checks, under the existing FP guard.
No donor/native FP implementation or duplicate SoftFloat helper is copied.

The emitted numeric call is an indirect C callback. Its real executor must
switch to the admitted host stack and establish whole-NULL signal recovery
before constructing C++/FP RAII, catch typed conversion failures in that scope
and return a trap reason only after destructors complete. Tests on the ordinary
host stack without active recovery are primitive evidence only. Joint Mach/
guarded-stack/callback execution, Linux native, full package/session/cache and
numeric callback overhead remain independent gates.

The final FP auxiliary build and CTest passed (3/3, 1.56s: 10 artifact,
15 compiler and four shared numeric-owner cases). The first build caught a
test-fixture enum-to-byte narrowing; its log is retained alongside the repaired
warning-free build. All 62 operators emit verified objects for both canonical
profiles with typed locals/direct-call relay and Raw roundtrip. On Darwin,
each operator executes direct and relay calls through the actual shared
numeric owner at distinct decoded/prepared mappings. Additional primitives
preserve signaling-NaN constant/reinterpret bits, zero-initialized float locals
and mixed-parameter float if-result NaN/signed-zero values. Shared owner tests
separately compare all 62 operations against the real interpreter and exercise
typed failures/FP restoration through the actual NULL callback boundary.
These are positive nonfault primitives on an ordinary host stack. Emitted
callback error-transport guards are source/codegen evidence; production
guest-stack bounce and Mach numeric callbacks remain NOT_RUN. Source opcode
coverage is 171/172 MVP input operations, with `call_indirect` still unavailable;
function-import modules, process/session/cache, execution isolation and package
readiness are separate blockers rather than one remaining opcode test.

The Darwin target slice's final existing auxiliary graph build used `-j4`
and passed without warnings. Its two CTest targets passed (10 artifact cases
and 13 compiler cases): ten portable compiler cases include real dense
16-entry `br_table` readonly storage/AArch64 page relocations; three native
nonfault primitive cases execute generated integer/direct-call/CFG/memory/
global/growth code on macOS ARM64. Original and Raw-decoded images are both
prepared and pinned at distinct mappings. Malformed opcode, section/instruction
alignment, scaled target, extent and exact positive CALL26 range reject before
mapping; the mapping fixture checks every admitted relocation patch's bytes.
The existing parser remains the fixture semantic oracle. Logs are retained as
`compiler-target-v3-build.log`, `compiler-target-v3-ctest.log` and
`compiler-target-v3-LastTest.log` under the compiler worktree's phase-0 artifacts.
Four structure tests and pinned Spring/LLVM/Wasmtime source-hash checks passed.
No root/package graph, runtime executor, hardware fault, Linux native, signal
owner, hardened-runtime packaging or performance acceptance follows from this
ordinary CLI primitive evidence. Those gates remain NOT_RUN or with their
separately recorded owner status.

The first fixture has four real functions: `add(i32,i32)->i32`, a direct-call
`relay` with that signature, `wide(i64,i64)->i64`, and `empty()->void` (also start).
LLVM verifies IR before/after O2, emits x86-64 ELF, and every generated function
has checked entry/stack metadata. Real artifact identity/Raw roundtrip,
determinism, existing interpreter arithmetic oracle and explicit budget/invalid
input/local-unavailability cases pass in the auxiliary owning-target graph.
The optional test fixture-output path writes owned artifact/WASM bytes for the
runtime owner's separate Linux gate; it does not print or log those bytes.

Commands use the existing immutable installed prerequisite prefix and current
source-owning targets. No LLVM/source/toolchain download occurs:

```sh
cmake --build artifacts/oc-phase0/artifact-build --target oc_compiler_tests -j 4
ctest --test-dir artifacts/oc-phase0/artifact-build -R oc_compiler_tests --output-on-failure
python3 tests/vm_wasm/verify_donor_sources.py --root . \
  --optimizing-donor "$SPRING_DONOR_ROOT" --llvm-include "$LLVM_INCLUDE_ROOT" --check
```

The subsequent integer/CFG owner was built with the coordinated, coherent
signal/FP prerequisite in the existing auxiliary graph. Five macOS arm64
cases pass, including real LLVM verification/ELF normalization across the
integer families and control-flow fixture, with the interpreter only as the
fixture oracle. A missing dependent-template keyword in the new test helper
was corrected after the first failed build; its failed output is preserved.
The final source and raw logs are frozen separately from `c3018d52`.
New fixtures exercise seven definitions at distinct module PC bases:
if/else merges, result-carrying `br_if`, multi/default and empty branch
tables, a counting loop, canonical early return and select. Codegen tests
cover every MVP integer family. Linux-only primitive probes additionally
cover actual integer/branch results, backward-edge poll counts, 32/64-bit
division traps and bounded cancellation of a call-free loop. These are not
the guarded-stack executor or signal/callback-bounce acceptance tests.
The small two-entry branch table may be optimized to selects: it does not
establish large LLVM switch jump-table relocation handling. Native probes
prepare a direct compiler artifact, not a decoded/reencoded CFG artifact at
a different mapping. Those serialized/native relocation gates remain
NOT_RUN, as does native execution of every new primitive on this arm64 host.

macOS/arm64 execution is not enabled. Native integer-entry smoke, actual Linux
executor/guarded stack/trap/callback/cancellation/reentry, full opcode CFG and
float/host/memory/indirect coverage, isolated process/session and installed OC
package gates remain NOT_RUN. Default/root configuration and structure/package
registration are reviewed separately from this implementation patch.

## Separate memory/global slice — auxiliary evidence only

This source delta is based on the reviewed integer/CFG v2 freeze, not merged
with its evidence. New portable fixtures describe every memory width/sign,
little endian, offset-vs-alignment and unaligned hinted access, truncated stores
with adjacent canaries, exact float/NaN payload transfer, globals, start/data
initialization, growth zero/success/failure, declared/execution maxima,
zeroed pages, zero initial memory and canonical malformed-input rejection.
The UINT32_MAX offset fixture explicitly raises the **existing execution
option** for that one request; production/default acceptance is unchanged.
Interpreter results are fixture oracles only, never OC execution.

Linux-only primitive tests decode and reencode the real artifact, pin its first
prepared mapping and execute the decoded artifact at a distinct second mapping.
They test widths and explicit bounds traps (a hardware fault is not success),
distinct invocation storage, and a fixed, preallocated POD growth callback that
forces memory relocation and alters an old byte to expose a stale cached base.
That callback is only a generated-code reload probe: it is not the real runtime
allocator/callback owner and proves no guest-stack bounce or signal lifetime.
The coordinated incremental compiler build and focused CTest pass with eight
Boost cases on macOS arm64 (1/1 CTest). This exercises real canonical preparation,
LLVM verification before/after O2, x86-64 ELF generation/normalization and the
existing interpreter as a fixture oracle. Source and raw command/exit-status
logs are frozen separately from CFG v2; no new graph/prefix or donor edit occurs.
The five additional Linux primitive cases are NOT_RUN, including decoded/remapped
artifact execution and forced base refresh. Large-switch jump-table relocation,
real executor growth limits/allocation, guarded stack, signal-owner acceptance
and concurrent interruption remain independent required gates. No native Linux
OC, JIT, installed OC package or full compiler readiness is claimed.

## ABI2 epoch compiler slice — separate narrow evidence

Wasmtime exact pin `05e7deebf1ddcc726bfc4c30cd87e3dbb2881072` inspected sources:

- `crates/cranelift/src/func_environ.rs:808-837,879-959,5296-5308`:
  entry/loop epoch load, unsigned deadline check and rare cold callback;
  source SHA256 `8fc0f61cf7d7c452543c8e010c2c29f23cd089e6c0c46308519a5977e0702eba`.
- `crates/wasmtime/src/engine.rs:886-922`: one shared atomic epoch, embedder tick;
  source SHA256 `c9bbfc1b0f41c1ca2f876623f25200e442f19d897255059d4b68e1c801ab591f`.

Exact official raw files are saved in
`artifacts/oc-phase0/build/wasmtime-epoch-donor/<pin>/`, not a build dependency
or Rust source copy. License is Apache-2.0 WITH LLVM-exception. Forge adapts
this pattern with acquire/system-scope loads, a snapshot+1 threshold and
independent sticky stop reason so a stopped periodic ticker cannot lose
compute stop. That stricter host publication/lifetime/saturation mechanism is
the runtime owner's separate slice; this compiler does not add a timer/thread.

The sole ABI declaration appends epoch144/threshold152/sticky_reason160,
size168/alignment8, lock-free atomic_ref requirements. ABI/codegen profiles are
2; old ABI/codegen1 (including mixed version pairs) reject canonicalize,
from_payload and checksummed Raw decode before prepared publication. Container
and configuration schemas remain version1; no persisted invocation context or
new key algorithm. Generated entry rejects old/short/null-word context before
guest or host effects using existing typed host_exception transport.

Actual SAME O2-verified IR is captured through a PRIVATE lowering::emit sink
from an owning test module, not public compiler::compile diagnostics or an env
dump. Sink/no-sink real object bytes match for Linux x86-64 and Darwin ARM64;
the controlled entry/backedge fixture has two acquire64 loads total, unsigned
comparison, system scope, no invariant epoch metadata and cold callback. No
duplicate caller-edge poll. Native ordinary-host-stack primitives observe no
poll on a stable epoch, callback threshold reload/rearm after host changes,
concurrent atomic bursts, slow saturation and typed fresh-exec software guards.
Seven negative/wake controls require exact normal typed exit and no host effects;
signal death, timeout, wrong exit or native return cannot pass. Infinite guest
wakes without a ticker after actual standard stop_callback publication in this
fixture, NOT after a production compute/executor stop path.

The first imported ordinary-TU stop_callback instantiation failed link for a
libc++ header-only constexpr template member. Same clang/SDK/default-link
standalone control passed; no dylib mismatch is established. A separate owning
standard-stop call initially masked that failure by emitting its member; review
rejected this emission-only shim. It is REMOVED. The ACTUAL infinite-guest
callback scenario now lives in its owning compiler test implementation unit,
reusing the controlled epoch-loop fixture and actual artifact owners. Ordinary
0..5 software guards remain; no private std definitions or linker/library
override exists. The ordinary imported stop_callback consumer link failure
remains OPEN. Raw failed and superseded build/test logs are retained, including
two imported-core BMI unused '-c' warnings. _Exit wake evidence does NOT confirm
callback destruction/unregister or a production lifecycle.

Final remediated existing-graph build and all3 CTest pass (artifact14/compiler22/
numeric4, 2.96s); actual owning O2/infinite scenario, unchanged ordinary guard
scenarios and old-profile artifact fixtures separately pass. Full structure
scanner passes2381; pinned Spring/LLVM/platform/epoch source checks are recorded
in the final raw provenance log. Earlier shim-dependent green logs are retained
as SUPERSEDED evidence, not acceptance of the final source.

Production guest/host bounce, active recovery NULL/FP/stop-callback teardown,
actual compute-stop after scheduler stop, epoch owner startup/drain, Linux native,
installed package, helper resource/restart/session/cache and performance remain
NOT_RUN/OPEN. Codegen/fixture PASS is not whole OC or Spring throughput parity.

## Codegen3 local stack taxonomy — pending separate evidence

Spring exact pin `e6a99f68b67abc4d89fe716755b2e1394a4991f7` keeps its
canonical call-depth counter in `LLVMEmitIR.cpp:1284` and resets native stack
from the canonical maximum in `executor.cpp:173-180`. Forge's portable local
stack capacity may instead be smaller (including a nested invocation's paused
outer stack). A finite path below canonical depth must not become an objective
WASM error solely because of that local capacity. This taxonomy is a Forge
adaptation, not a claim that Spring exposes the same raw reason or local limit.

The sole `trap_reason` appends `native_stack_exhausted=12`; old values0..11 and
ABI2's size168/layout remain unchanged. Generated `call_depth>=max_call_depth`
still transports `stack_overflow=8`. ONLY `remaining<frame_bytes` transports the
new operational reason. The SAME exact target profile changes codegen2 to3,
rejecting old/mixed profiles through canonicalize/from_payload/decode before
native preparation; no new hash/codec/cache or configuration schema.

Owning tests add a three-function finite path with every single frame fitting
the small allowance but the aggregate exceeding it, depth3 below canonical
maximum251. Fresh-exec controls require small-local reason12, large-local
success/result42/restored counters, canonical depth failure reason8, and depth
precedence when both limits fail. Hardware signal death/timeout/wrong exit fail.
The existing private SAME O2 fixture checks separated reason8/reason12 guarded
blocks on both actual target profiles. Exporters retain the same seven owners
and assert ABI2/codegen3; prior fourteen codegen2 pairs are never overwritten.

This source is pending targeted build/review/new fixture export. Native Linux,
runtime operational mapping and actual guest/host stack/recovery/lifetime,
helper/session/package and performance are NOT_RUN by this source delta.
