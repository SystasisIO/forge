# WAMR Fork Provenance

Status: experimental source pin, not production runtime acceptance.

The runtime and compiler source is maintained in the
[SystasisIO WAMR fork](https://github.com/SystasisIO/wasm-micro-runtime).
The upstream baseline is
[`ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`](https://github.com/wasm-micro-runtime/wasm-micro-runtime/commit/ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a).
The Forge submodule pins
[`f6fe6d5ae1c14b2cc4301e2fca0b3364be6a692b`](https://github.com/SystasisIO/wasm-micro-runtime/commit/f6fe6d5ae1c14b2cc4301e2fca0b3364be6a692b)
on the experimental `forge-runtime-profile-v1` draft branch. The fork commit
must be published before the parent Forge pin update. Its predecessor is
[`cb1eb77dd4d788bf475162223884b4fbfd7fe30a`](https://github.com/SystasisIO/wasm-micro-runtime/commit/cb1eb77dd4d788bf475162223884b4fbfd7fe30a).
Full qualification remains pending. Review is tracked in
[fork PR 1](https://github.com/SystasisIO/wasm-micro-runtime/pull/1).

## Darwin JIT Unwind Registration

Commit `f6fe6d5` keeps the existing bounded JIT allocator and W^X policy, but
uses LLVM's `EHFrameRegistrationPlugin` with always-emitted DWARF EH on Darwin.
Mach-O compact-unwind encodes image-relative offsets in 32 bits; independent
code/data mappings have no such proximity guarantee. CI57 exposed this during
the unchanged 8193-instance donor test. Darwin's DWARF FDE encoding uses
pointer-width PC-relative references instead.

The existing LLVM plugin removes compact-unwind before pruning and registers
and deregisters EH frames through the same allocation actions already owned
by the bounded load owner. Plugin-creation failures remain recoverable.
`setUpInactivePlatform` disables only generic LLVM global ctor/dtor discovery:
this WAMR pipeline has no such globals and owns WASM start/initialization
explicitly. Process-symbol lookup remains independent. Linux setup is unchanged.
No new allocator, mapping registry, public API or relaxed instance limit is
introduced. Fresh runtime and remote qualification are recorded in the
[CI follow-up](forge-wamr-dev-integration-20261018.md), not inferred from the
preceding pin's results below.

## Committed Native Protection Policy

Commit `cb1eb77` adds internal module protection provenance: uncommitted, no
defined guest bodies, sealed ordinary RX, or Darwin MAP_JIT. No public Forge
API, public WAMR C API, parallel allocator or global mapping registry is added.
The existing ORC allocation record becomes committed only after checked
protection, finalization, unwind registration and temporary-storage cleanup.
The AOT loader publishes its policy only after successful complete loading.

On Darwin ARM64 with actual pthread JIT protection support, the existing
MAP_JIT path and nested caller-state restoration remain required. If the OS
reports no such support, code uses an ordinary non-MAP_JIT mapping: writable
and nonexecuting during preparation, checked RX before publication. Ordinary
RWX is never used, and failed MAP_JIT setup does not silently retry another
policy. Unsupported or uncommitted native modules are rejected before guest
initialization/calls. Empty/import-only modules may have no native code.

The independent macOS15 ARM OS-only diagnostic verifies support=0, ordinary
RW-to-RX execution and rejection of RX writes/RW execution. It is not itself a
WAMR or Forge qualification. Fresh local coherent runtime/compiler builds at
this pin pass 81/81 source CTests, including 53 AOT tests, and the standalone
real-loader budget/fault suite passes 1/1. A subsequent normal local dev install
passes all three canonical package consumers, including relocated interpreter/
JIT/profile and five nested compiler/AOT/session tests. This JIT-ON package is
not a physically LLVM-free OFF proof. Remote support=0, installed delivery on
other platforms, x86 and product acceptance remain separate gates; see the
[CI follow-up](forge-wamr-dev-integration-20261018.md).

## Transferred Changes

| Commit | Existing patch responsibility |
| --- | --- |
| `1f431cd` | Deterministic numeric behavior, host floating-point environment and AOT cancellation |
| `73cf5eb` | Deferred guest initialization |
| `abf0438` | Raw native-call alignment and ARM64 relocation |
| `ab7bff6` | Auxiliary stack upper-bound validation |
| `bc786f8` | Validated instruction and frame admission metrics |
| `a029b33` | Logical interpreter and AOT call-depth accounting |
| `7459d1e` | Exported raw-import invocation ABI |

The seven-patch transfer at `7459d1e` changes 22 upstream files, with 555
inserted and 83 deleted lines.
All changed files were compared byte for byte with the previously qualified
seven-patch prepared source and matched. The transfer introduces no new engine
behavior and does not widen WAMR's local-cell or offset representations.

## Imported Initialization Follow Up

Commit `65f974b` executes imported start functions through the existing native
ABI and reads the imported function type correctly for `__post_instantiate`.
The change touches three existing runtime/loader files, with 30 inserted and
28 deleted lines. Four failing baseline cases and four passing signature
controls were reproduced independently; all 184 candidate cases passed across
classic, fast and both ASan/UBSan variants. The cases check deferred execution,
order, exactly-once behavior, depth, cancellation, host-failure precedence and
floating-point environment restoration.

The permanent fixtures and probe live in `tests/vm_wamr` and are registered
in its standalone CMake graph. The independent review covered the exact diff
SHA-256 `1922be242d126a137b94a81f0ba6489021805286c592082c08bda79462ad0c5a`.
Candidate AOT inputs were produced by the preceding compiler revision; rebuilding
compiler/runtime and running the full suite from this new pin remains required.

## Structured Errors And Table Layout

Commit `1217d28` adds a synchronized NONE/UNKNOWN/KNOWN exception classification
and the original engine error ID, including propagation and initialization.
Plain text remains UNKNOWN. Commit `077dfd3` supplies the existing out-of-memory
ID when raw argument-buffer allocation fails; no host callback runs on failure.

Commit `66e6be6` aligns table starts and strides in interpreter/AOT allocation and
native code-generation offsets. It keeps logical global sizes and capacity
counters unchanged. The compiler also rejects metadata layouts outside its
positive i32 offset representation, including extra-state field extents. This
is not a linear-memory page limit. The old fixture with three i32 globals remains
unchanged; no test padding conceals the original alignment defect.

The final six-file follow-up was independently reviewed at diff SHA-256
`a2a572de6919b0dc8ffae0ab137a6509e6d918682d280b16a18b0839aab6f6a0`.
Fresh compiler/AOT inputs passed 256 checks: 184 initialization, 64 structured
exception and eight allocation-failure checks across classic/fast and both
ASan/UBSan builds. A release layout probe passed target-width, zero-size,
overflow and signed-offset boundary checks. This qualifies the bounded fork
changes with the previous allocation-usage profile, not all public APIs or
downstream products. The new usage-dispatcher profile requires its own full run.

## Build Ownership

The [2026-10-16 follow-up](forge-wamr-follow-up-20261016.md) records three further
reviewed commits: usage-aware CLI callback signatures (`2395a93`), correct static
native frame placement/stack metadata (`066a5d7`), and explicit numeric/legacy-zero
block-type policies (`75e8f91`). That clean pin was published in the existing
draft PR; fresh combined compiler/runtime evidence is recorded in that follow-up, not
inferred from the older 256-check usage-0 matrix above.

The preceding `156dbd0` adds budgeted native mappings, checked protection/unmap
handling and deferred caller-thread LLVM-JIT preparation. Its exact 11-file
patch is SHA-256
`371d3884e6413e36a552e8137ddaebff439fe42c584ce5c1133fbceff9ebdf9a`.
It is published to the same draft branch/PR. The private Darwin loader checks
are recorded in completion block R3 of the follow-up; ordinary API,
session/cache, fresh compiler artifacts and native JIT qualification are still
separate gates. This commit does not include the pending ORC memory manager.

The preceding `ed5c12a` adds the eager LLVM JIT path, an ORC JITLink memory manager
that bounds code/RO/RW mappings and working buffers through the same load owner,
recoverable mapping/protection failures and caller-thread diagnostic TLS. It
does not promise a cap on LLVM's own IR/optimizer/object heap. The exact 21-file
Forge/fork review manifest is `tests/vm_wamr/jit/source-freeze-orc-v2.json`,
SHA-256 `334af80c0c2c7028f9f6ce5a5f2f2181f32b89d883f3601d7af05b2c0aba72fc`.
All source hashes were checked again before publication; the 12 fork files were
committed to the same draft PR. Independent source review found no P1/P2 in that
slice. Native compile/execution, mapping denial/recovery, thread isolation and
one-runtime link-map qualification are still NOT_RUN for this new pin. Earlier
`156dbd0` test results remain labelled with their original source identity.

## Instruction Profiling And Export Names

The published experimental `12d1f90` candidate adds bounded, opt-in LLVM instruction profiling
and length-aware export names in 18 existing/new fork files. Mapping uses the
existing compiler, JITLink and LLVM DWARF facilities; it does not introduce a
parallel WASM parser or relocator. The map publishes only after complete loader
preparation and remains owned by the module. Plain JIT has no profile map.

Export names preserve their original byte length through interpreter/JIT
metadata, instance lookup, AOT emission/loading and C API byte vectors. Empty
and embedded-NUL exports remain distinct. Import and symbol names stay strict.
The existing AOT wire representation is retained; names read by the AOT loader
are module-owned and freed on partial failure and unload.

Independent reviews of the final fork profiling and export-name slices found
no P1/P2 issues. Public sampling and runtime qualification are recorded in the
[2026-10-18 checkpoint](forge-wamr-dev-integration-20261018.md), not inferred
from older executable results. LLVM's IR/optimizer/debug-decoder transient heap
remains outside the runtime preparation ceiling.

[ForgeWamrSource](../../cmake/ForgeWamrSource.cmake) requires the exact clean
submodule revision and archives it into the build directory. It does not apply
patch files. The source identity combines the immutable fork revision with
the hash of [ForgeWamrProfile](../../cmake/ForgeWamrProfile.cmake) and the explicit
effective runtime role. AOT compiler and artifact identities use the shared
JIT-disabled role. An optional JIT runtime additionally identifies its LLVM
version, native target/CPU policy, optimization and caller-thread settings.

The existing files under `cmake/patches/wamr-*.patch` are historical evidence
referenced by earlier qualification reports. They are no longer build inputs.
New engine fixes belong in reviewed fork commits followed by an explicit pin
update and requalification; maintaining a second active patch series in Forge
would create conflicting sources of truth.

Compiler binaries and artifacts from the previous patched-source identity
must be rebuilt. Updating an identity sidecar on an old binary is not a valid
substitute for rebuilding the compiler from the pinned source.

Upstream copyright, attribution and
[license](../../vendor/wamr/LICENSE) remain with the vendored source.
Public API integration, resource bounds, cross-platform determinism and
downstream workflows retain the acceptance conditions in the
[replacement plan](../iterations/forge-vm-wasm-wamr-v1.md).
