# WAMR Dev Integration Checkpoint

Status: pending changes, NOT ACCEPTED. Date: 2026-10-17.

## Source Identity And Scope

- Integration branch: `vm-wasm-wamr-dev-integration-v1`, based on Forge
  `3a6e388f29c1535d9d9c642877d416ca7e52e83a` (`dev`, PR 187).
- Pinned runtime/compiler fork: `ed5c12ae6bcec88f2863aa7c541b6d829d657a4c`.
- Original `vm-wasm-wamr-experiment-v1` worktree and its staged/unstaged source
  were preserved. Transfer evidence is in workspace artifact
  `wamr-dev-integration-20261017-r4`.
- The approved legacy parser/allocator cleanup preserves `expr_value`, live
  host conversions, `interpreter::context` and caller `wasm_allocator`.
  The signal helper retains `nullptr` calls without a retired code allocator.
- Independent review closed the cleanup slice and the corrected bounded README
  examples. Its source24 diff SHA-256 was
  `1c7e90faba2175b8ba3b0d2597d8d11fae56b2e762bd6853233bbf9c98ccf7a7`.
  Subsequent validation-only fuzz-harness changes are separate and not covered
  by the runtime matrix below.

Exact source breaks and migration are in the
[release note](../releases/unreleased-wamr-bounded-profile.md).

## Fresh Native Qualification

Host: Darwin ARM64, LLVM 22.1.8, Release. Build directory: `build/wamr-dev-r4`.
LLVM JIT and the separate AOT compiler are both enabled. Compilation uses `-j2`
because of the 24 GiB host resource constraint. The AOT helper tests use an
explicit 1 TiB virtual-address diagnostic allowance; this is not a product
memory default or a claim of 1 TiB resident memory.

| Check | Result | Evidence |
|---|---|---|
| Dev JSON/HTTP/secrets targets and API-WASM test | Build PASS; CTest 1/1 PASS | `dev-api-build.log`, `dev-api-r4.xml` |
| Public validation/invocation/execution/consumer checks | 4/4 PASS | `public-wasm-r4-first.xml` |
| Full selected VM matrix | 61/64 CTests PASS; 3 failures | `wasm-matrix-r4.xml`, `wasm-matrix-r4.log` |
| AOT subset of that matrix | 45/45 PASS | Same matrix; `optimizing` label |
| Native JIT/header subset | 7/7 PASS | Same matrix; `jit` label |
| Fresh spec rerun after fixture initialization | 22 failures in 4856 cases | `spec-r4-fixtures.xml`, `spec-r4-fixtures.log` |
| Zero-depth typed-error repair | 2/2 cases, 8 assertions PASS | `zero-depth-r4.xml`, `zero-depth-r4.log` |
| Unit rerun after that repair | 14 historical-profile failures; zero-depth regression closed | `unit-after-zero-depth-r4.xml`, `unit-after-zero-depth-r4.log` |
| Actual spec-generator mapping | 1/1 PASS after building the generator | `generator-r4.xml`, `generator-r4.log` |
| Source layout | PASS, 2770 first-party files | Structure checker |
| Donor source mapping | PASS against local pinned EOS VM donor | `verify_donor_sources.py --check` |

The initial matrix's spec failure was caused by the uninitialized
`eos-vm-test-wasms` submodule; the pinned fixture revision
`ffbbb552e6020623d8ae148d81a152c8ef700325` is now present. The generator-mapping
failure was a missing generator executable. Neither is evidence of runtime
failure, but neither counts as a pass. The subsequent spec failures above are
actual assertion differences and require separate classification.

The historical unit suite reports 16 failures: twelve backend-expanded cases
for the three explicitly retired malformed-input permissions, two historical
extreme-depth cases, and two zero-depth exception-type regressions. The last
cases fail at invocation, not initialization: the caller receives
`vector_out_of_bounds` instead of the expected `interpreter` exception.
The first categories are not green historical parity. Original donor snapshots
and failed logs remain; do not suppress unrelated failures under the approved
profile changes.

The reviewed zero-depth fix restores `interpreter` without changing admission,
initialization, the guard ordering or the depth limit. Focused tests verify its
exact error code/category, calls by name and index, and the still-initialized
context in both backends. The subsequent complete unit run leaves only the
twelve retired-permission and two out-of-profile depth cases. No donor oracle
was rewritten to obtain this result.

Independent review classified the fresh spec failures as follows (both
interpreter and JIT cases are counted):

| Difference | Assertions | Classification |
|---|---:|---|
| Multi-byte zero `memidx` encoding in `memory.size/grow` | 16 | Historical MVP oracle differs from modern grammar; not an established engine defect |
| Different `br_table` target types in unreachable code | 2 | Historical oracle differs from modern polymorphic typing; not an established engine defect |
| Valid export names containing embedded NUL | 2 | Unapproved admission narrowing; source-name compatibility remains open |
| Initial memory of 65,536 pages | 2 | Expected refusal outside the bounded profile |

The initial review grouped 20 failures as regressions. Normative follow-up
corrected that classification: [Core 3.0 memory instructions](https://webassembly.github.io/spec/core/binary/instructions.html#memory-instructions)
encode `memory.size/grow` using `memidx`, and [integer encoding](https://webassembly.github.io/spec/core/binary/values.html#integers)
permits redundant zero groups within the `u32` length limit. These eight fixtures
use valid encodings of index zero. WAMR still rejects nonzero indices in this
single-memory configuration. Resolve the explicit profile/oracle policy without
calling these sixteen assertions security defects or silently ignoring them.

The initial `br_table` defect classification was also withdrawn after checking
the [current typing rule](https://webassembly.github.io/spec/core/valid/instructions.html#valid-br-table):
branch operands must match every target, but the target types need not be
identical. Unreachable polymorphic operands satisfy this case. WAMR's existing
`thread_proposal_ignore_cases.patch` explicitly identifies this historical
fixture as valid under the later spec interpreter. A proposed stricter loader
change was not accepted; it must not be shipped merely to turn this old oracle
green.

Thus eighteen assertions reflect language-version differences, two reflect the
bounded memory profile, and two concern unsupported valid NUL-containing export
names. Supporting those names requires length-aware existing module metadata
and AOT handling, not just removing WAMR's UTF-8 check or reparsing WASM in Forge.
The spec log SHA-256 is
`063dd7f79f3963f16b2cd59ec640aded88bf09058f76c29698c25b67d0b221b9`.

## Instruction Profiling Prototype

The unpublished compiler-source prototype passed one native CTest and three
repeat runs on Darwin ARM64. It produced five native intervals for three
original-file WASM instruction offsets. Real timer samples distinguished all
three offsets in every run (299, 275, 240 and 270 samples). Two concurrently live
objects, unknown host/prologue/gap/foreign PCs, mapping refusal and cleanup were
checked. The link map confirms one runtime C provider.

Source freeze: `tests/vm_wamr/jit/source-freeze-instruction-profile-v2.json`,
SHA-256 `f4d09a858324b9336d2bb8436332dc9e1fa0fcdbe7354b6c81531c73361293f7`.
See the [prototype evidence](../../tests/vm_wamr/jit/instruction_profile.md).
Independent review found no P1/P2 source defects in that frozen private proof.
Early owner-heap refusal/cancellation coverage, the public observer's lifetime,
multi-function coverage and public backend integration remain required. The
pinned vendor runtime and normal `jit_profile` behavior have not changed.

## Compiler Isolation

Independent inspection of both newly linked `forge-wasm-compiler` and
`wamr_compile_fixture` confirms one WAMR C runtime provider in each image:
33 compiler-runtime objects and 20 codegen objects, all compiled with
`WASM_ENABLE_JIT=0` and `WASM_ENABLE_WAMR_COMPILER=1`. Runtime-only JIT objects
are absent. Each image has 879 unique WAMR-prefixed global symbols and no
external WAMR imports. Their link maps are emitted under the corresponding
build target directories.

This qualifies the isolated helper/fixture images, not mixing compiler and
execution runtimes in one process. The separate execution runtime uses
`WASM_ENABLE_JIT=1` and `WASM_ENABLE_WAMR_COMPILER=0`.

## Open Gates

- Correct the remaining spec incompatibilities and resolve the explicit
  historical-oracle versus current-profile test policy. The original negative
  tests are not silently skipped; the complete historical suites are not green.
- Extend early-failure/lifetime coverage and integrate public `jit_profile`
  after the scoped private-proof review. The opt-in compiler-source probe does
  not change the vendor pin or qualify normal backend profiling behavior.
- Fresh installed/relocated components and LLVM-free runtime configuration.
- Matching installed SDK and current Turbine process acceptance.
- Spine's separately delivered DB allocation/snapshot dependency closure and
  full-process acceptance.
- Linux/x86-64 and cross-platform numeric/cancellation qualification.

Static production-graph inspection found no selected old EOS interpreter,
x86 code generator or OC compiler. Licensed donor-derived value/conversion
helpers and the legacy signal helper remain. This is neither an EOS-source-free
claim nor a licensing conclusion. No license terms, live product pins, remote
branches or releases were changed by this checkpoint.

## CI Configuration

The pending VM workflow now selects LLVM-free interpreter and LLVM JIT lanes
explicitly and builds the targets covered by its test selection. Isolated AOT
qualification is an explicit dispatch option in the ARM64 LLVM 22.1.8 lane.
Donor assertions remain intact, with no failure whitelist. YAML parsing, 26
shell-block syntax checks and static target coverage passed. Remote CI and x86
JIT qualification are NOT_RUN; this workflow edit is not a green CI result.
