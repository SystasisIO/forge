# Optimizing request/artifact owner slice

2026-10-12. Implementation slice after the accepted options/preparation and
SoftFloat TLS prerequisites. The four public declarations have an independent
review; implementation/native execution are separate gates.

## Accepted owner choices

### Separate prospective accounting and pin observation slice

This additive source slice remains within the existing artifact/native-image
owner and preserves serialized identity, immutable entry spans and mapping
lifetime. `prepared_byte_size()` uses the sole `native_image::plan`: rounded
image extents, full extra alignment reservation and exact native-function
storage with checked addition/multiplication. Private function storage becomes
an exact-sized owned array rather than assuming `vector::reserve(n)` yields
capacity `n`. Actual `byte_size()` after prepare uses that same planned storage
contract; before prepare it still reports only counted allocated metadata and
encoding. The getter does not mmap, protect or publish native readiness.
Temporary planner storage is permitted; target/page preparation remains the
existing constructor's separate gate. Allocator/object-management overhead and
process RSS are not this existing accounting contract.

`is_unique()` is only an advisory snapshot under external index exclusion, not
an eviction lease, weak/raw resurrection or mapping synchronization. Shared
artifact pins remain the lifetime authority. Cache accounting must retain and
charge externally pinned entries rather than evict/remap them by sampled count.
Tests cover both profiles, 0/1/3/17 function entries, cold prospective accounting
and unchanged identity/Raw encoding, existing target/alignment/budget rejection,
64 coordinated concurrent pin/drop cycles, and native pre/post size equality
plus shared spans. This is artifact-owner accounting evidence, not compiler,
session/cache, guest/host bridge, fault recovery or runtime-readiness acceptance.
The existing artifact/compiler owning targets built with `-j4` without warnings;
all three CTest targets passed in 2.18 seconds (13 artifact, 18 compiler, four
shared numeric-owner cases). Native accounting equality ran on Darwin ARM64;
Linux native remains NOT_RUN. Raw logs/source/preimages are frozen separately
from the target/compiler slices. Extreme-sized collection arithmetic injection
is NOT_RUN: guards are source-checked, while actual bounded alignment/budget/
target rejection is exercised without manufacturing invalid C++ containers.
No session, root/package, fault-plane, guest/host bridge or performance gate is
promoted by this result.

- `compile_configuration` is one real value shared by compiler request and
  artifact: version, existing execution-options snapshot, canonical imports,
  target profile and compiler budgets, in that one Boost.Describe order.
- The request adds owned WASM bytes. Artifact retains their existing Forge
  SHA-256 digest and the same configuration, not the source or a second option
  record. Identity uses existing Raw/digest packhash of digest+configuration.
- Import order is module/name, not process registry ordinal. Duplicate names
  and unsupported/non-numeric/multi-result signatures are typed failures.
- Persisted output contains sections, section-relative relocations, canonical
  signature IDs, function stack metadata, exports, initial globals/memory/data,
  indirect table and start. Native addresses are never serialized.
- The artifact is an immutable copyable handle. One impl owns checked values,
  encoded bytes and one explicitly prepared native image. Runtime consumes
  checked const views and prepared code/function spans, not private headers
  or a second decode/mapping cache.
- Preparation uses a mutex only for single-flight construction. A local
  uniquely owned image completes relocation and W-to-RX/RO protection before
  release publication of a readiness boolean. Readers acquire before accessing
  fields that never change thereafter. Failure publishes nothing and cleans
  the allocation; retry is explicit. Last-copy lifetime owns unmapping.
- Native mapping is Linux x86-64 only. Non-supported hosts can encode/decode
  values and inspect code-generation output, but preparation throws typed
  `unsupported_target`; no arm64 product execution support is implied.

Donor reference remains Spring `e6a99f68b67abc4d89fe716755b2e1394a4991f7`:
`LLVMJIT.cpp` provides the section/function-offset and `.stack_sizes` gate;
`code_cache.cpp` provides pinned immutable image lifetime. This slice adapts
those ownership patterns, not the donor ORC/GS/chain framework. It does not
copy donor math or add a WASM parser. Full lowering must still establish real
function-stack metadata completeness and reject unknown object relocations.

## ABI source of truth

The standard-layout `invocation_abi` is the only native context declaration.
Compiler access uses its `offsetof`/size/alignment rather than a separately
typed LLVM prefix. Raw callbacks are noexcept; host call indices mean sorted
configuration import indices, numeric opcodes mean existing `interpret::opcodes`
values and explicit operand count. The ABI adds no prefixed/saturating opcode
acceptance. Stop/deadline and exact exception objects remain in the invocation
owner; generated frames only see poll/status/trap callbacks.

The artifact native-function array owns mapped entry addresses plus validated
stack size/type IDs. Invocation memory, globals, table, depth and guarded stack
remain invocation-local. Cancellation must never mprotect shared code.

## Validation and trust

Before mapping, validate all section kinds/alignment/ranges, relocation widths
and overlap, native entry offsets, type/import/export/table/start relationships,
global bit widths and data bounds. Accepted relocation kinds are local
`absolute64` and `relative32`; the upcoming LLVM object normalizer must prove
all emitted references fit that explicit vocabulary.

An encoded version/checksum detects corruption, not adversarial replacement by
an actor controlling the trusted helper/cache. Metadata validation does not
prove arbitrary supplied native bytes implement their named WASM digest. The
isolated compiler, private cache directory and expected identity are mandatory
trust boundaries. Neither this slice nor the factory fixture is a full
optimizing-compiler acceptance test.

Existing Raw container/encoded-size ceilings apply in addition to compiler
budgets. Failures are local compile/cache readiness errors, not a verdict on
historical contract or transaction validity. Input/source bytes are unlogged.

## Reproduction and current evidence

`artifacts/oc-phase0/artifact-consumer` uses the actual installed/relocated Raw,
digest and exception targets plus current source-owning VM/SoftFloat/artifact
targets. It is an auxiliary graph, not a fake package or a root/install gate.
Configure with explicit LLVM 22.1.8 compilers, SDK, `Forge_DIR`, package-specific
dependency prefixes and pinned `FORGE_PHASE0_SOFTFLOAT_ROOT`, then:

```sh
cmake --build artifacts/oc-phase0/artifact-build --target oc_artifact_tests -j 4
ctest --test-dir artifacts/oc-phase0/artifact-build --output-on-failure
```

Current result: owning target build and seven focused cases PASS. Coverage:
all 35 described execution fields, six target fields and both compiler budgets
affect identity; reordered imports preserve it; malformed/duplicate signatures
reject; request/artifact Raw roundtrip, stale identity, checksum corruption,
truncation/trailing bytes, metadata bounds and unknown/overlapping relocations
reject; artifact copies pin one view; unsupported arm64 preparation remains
not-ready and can fail again without publishing partial state. Original logs
are `build/artifact-owner-final-build.log`, `artifact-owner-final-tests.log`
and `artifact-owner-last-test.log`; binaries/BMIs are ignored.

Mandatory native Linux gates remain NOT_RUN: actual relocation application,
RX/RO permissions, concurrent successful prepare single-flight, failed native
mapping/protection injection, stable prepared spans, pinned eviction and
last-copy unmap. Compiler CFG/float/host/stack/executor/session/package/product
acceptance also remains separate. The earlier FP environment transition gap
is not included in this source patch or silently accepted by these tests.
