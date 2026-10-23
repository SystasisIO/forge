# Limited native CEL profile, 2026-10-23

This is the historical in-process profile qualification at `2d299f01`.
Its original evidence is retained unchanged. The
[isolated worker and installed-package follow-up](expression-worker-memory-2026-10-23.md)
records the subsequent memory/lifecycle/package work and its separate gates.

## Scope and status

This change adds an optional native **Preview** library, `forge_expression`,
using CEL-C++ `9c562a306569e90c8dea6d3af71f4433aa386162` and Bazel `8.7.0`.
Base: Forge `e51c6929fd120d13976963e9d633ba77b3044e01`.
The preceding [native comparison](https://github.com/SystasisIO/forge/pull/201)
is a separate change; its raw results remain the canonical comparison evidence.

This is a native profile acceptance on macOS ARM64. It does not add a financial
contract, Contract SDK intrinsic, canonical serialized program, Spine executor
integration, or a Turbine pin. Linux and real providers were not exercised.

Public module imports are `forge.expression.program`, `.types`, `.exceptions`;
the package component is `expression`, enabled with `FORGE_ENABLE_EXPRESSION`.
The [library README](../../libraries/expression/README.md) owns the supported
types, overload policy, rounding and limit semantics.

## Implementation boundaries

CEL's native Pratt parser, checker, checked AST, planner and runtime are reused.
The Forge policy traverses the original AST before/after checking; it does not
introduce another grammar, numeric type, intermediate representation or
evaluator. All nodes and resolved overloads are checked, including unselected
branches. No publisher-supplied opaque program is accepted in this version.

The exact extension is `math.mul_div(a, b, divisor, rounding_mode)`, separately
typed for signed and unsigned 64-bit arguments. It uses a sufficient 128-bit
product and checks the final range after rounding. The four modes are truncate,
floor, ceiling and nearest/ties-to-even. Ordinary arithmetic remains CEL's
checked arithmetic; the extension is explicit rather than a source rewrite.

Prepared state is immutable. Forge inputs are borrowed through views; strings
and entire input objects are not copied into intermediate owning DTOs. CEL
activation/arena state is per invocation. Declarations are sorted once; lookup
uses binary search, duplicate detection uses fixed-size scratch arrays, and map
key checks sort borrowed views. Only a scalar result leaves the private backend.

The standard CEL reference resolver handles the named rounding constants.
The private backend is built by the upstream Bazel dependency graph and exported
as a transitive static archive. CMake builds the Forge modules and imports that
archive privately; public modules export no CEL/Protobuf/Abseil headers or types.
The archive's remaining linker requirements are checked explicitly. Pins,
qualified module lock, compiler configuration and upstream notices are retained.

## Acceptance evidence

Toolchain: Homebrew LLVM 22.1.8, C++23, CLT MacOSX SDK, macOS deployment target
15.0, native ARM64; four build jobs. The private backend uses `-O2`,
`-fno-fast-math`, `-ffp-contract=off`; conflicting ambient include variables are
removed only in child processes. The build manifest records actual inputs and
archive identity. Native Forge dependencies use the normal owning CMake targets.

| Gate | Result | Evidence |
| --- | --- | --- |
| Feature OFF | PASS | Configure does not discover/build CEL or Bazel |
| Feature ON, native library | PASS | Real upstream closure, Forge modules and final executable |
| Arithmetic oracle | PASS, 1460 cases | Python Fraction oracle, signed/unsigned boundaries and randomized cases |
| Private arithmetic CEL test | PASS, 1460 cases | Real parse/check/execute path, no replacement evaluator |
| Public Forge test | PASS, 9 groups | Includes all 1460 arithmetic cases and 45 donor fixtures |
| Public CTest plus oracle regeneration | PASS, 2/2 | Exact categories/codes, source/input/AST/event limits and negative cases |
| Final focused CTest subset | PASS, 3/3 | Public profile, oracle regeneration and package metadata |
| External build-tree package consumer | PASS | `find_package(Forge COMPONENTS expression)`, module rebuild, final link and execution |
| Parent `add_subdirectory` | PASS, configure only | Owning project composition; no second backend build claimed |
| Package metadata | PASS | Genuine installed export audit, metadata relocation and core discovery |
| Installed expression runtime consumer | NOT_RUN | The existing all-target export/install requires the full Forge installation |
| Linux, SDK/WASM/WAMR, Spine, live providers | NOT_RUN | Outside this native stage |

The 45 donor fixtures derive from comparison corpus SHA-256
`3092a37192c4ae6bee522ad4219527060e2e0098e762cd40d95b9f1ae7eb5546`.
They cover lookup by resolution/audio, TTS steps, video/reference calculations,
BitShares data fees, scale and asset arithmetic. The formerly overflowing wide
scale example explicitly uses `math.mul_div`; it is not reported as an unchanged
stock-CEL expression. The separate oracle also covers a `10^30` intermediate.
Fixture derivation verifies the published corpus hash.

Negative tests cover forbidden types/functions in unselected branches, missing
and extra inputs, exact signed/unsigned kinds, ordinary overflow/division by zero,
missing lookup keys, invalid profiles, node/depth/input limits, resource failure
that cannot be pruned by CEL logical operators, reset between calls, lifetime
after source/declaration destruction, move semantics and concurrent evaluation.

The permanent [validation directory](../validation/expression-cel-profile-2026-10-23)
contains manifests and compressed logs. Earlier failed attempts are retained;
the final gate records identify the successful state. Metadata acceptance is
explicitly separate from an installed runtime qualification.

## Findings closed during implementation and review

- CEL's checker counts extra traversal records. A direct caller-node limit
  rejected expressions exactly on their allowed node boundary. Exact AST
  admission now owns that limit; the checker has a sufficient defensive bound.
- Named checker constants alone did not make `rounding.*` executable. The
  upstream checked-reference resolver is now enabled; no custom source rewriting
  or duplicate activation constants were added.
- Forge boundary arrays no longer allocate or copy an owning input DTO.
  Preparation allocation failure is mapped at the Forge boundary. Invalid
  profile limits are classified before applying lowered declaration limits.
- Sorted declaration lookup and sorted borrowed map keys replace quadratic
  duplicate/name scans.
- Implicit map selection keys now obey the same byte limit as explicit string
  keys; the exact boundary and the first disallowed byte are tested.
- Upstream's original lock lacked a registry checksum required by strict mode.
  A reviewed qualified lock is committed; normal builds use strict lock mode.
- The archive requires CoreFoundation. That requirement is propagated through
  a normal private CMake dependency; unknown residual dependencies fail packaging.
- Independent packaging review closed mismatched C/C++ driver selection and
  missing nested dependency notices, including `utf8_range`. The manifest now
  records actual Bazel compile actions and resolved headers; mismatched driver
  pairs and unpinned optional Bazel configuration files are rejected.

Developers and independent reviewers used the requested 6.1 sol/xHigh team.
Review covered backend/API ownership, arithmetic, lifecycle, limits and packaging
in repeated passes. A static review result is not substituted for the runtime
gates listed above.

## Remaining qualification

The structural limits and post-step AST-event budget are implemented. They are
not a hard cap on every native allocation or a CPU-instruction budget. CEL has
allocations outside Protobuf arenas; no total-memory claim is inferred from
arena settings or process RSS. A failed invocation can execute one AST event
beyond its allowance before the upstream callback aborts it. Allowed operation
sizes are separately bounded, and this profile excludes loops/growing collections.

The native component remains Preview. Full allocation-failure qualification,
hard heap accounting/isolation, dependency ABI co-installation with other
Protobuf/Abseil versions, installed runtime relocation and cross-platform
acceptance remain separate gates. The next contract stage must define portable
versioned program data and a canonical intrinsic ABI, use the existing Forge
testing host, and later validate Spine resource accounting and consensus replay.
