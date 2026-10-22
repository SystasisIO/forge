# WAMR Public Integration Follow-Up

Date: 2026-10-16. Status: pending experimental changes, not release acceptance.
This record separates the current follow-up from earlier same-day checkpoints.
Raw evidence is under workspace `artifacts/wamr-migration-20261016-r2` and the
existing, explicitly named warm build directories below. Directory names with
old revision suffixes are not binary identities.

## Engine Source

The clean local vendor pin is
`75e8f91e6bb25621a9c8fb4f121f604e5026b89d`, with the unchanged common feature
profile SHA-256
`3047b1666498e5a6b2d5a75d118606c5073fd1f9564c8618c2d6ca159ab8e382`.
The three reviewed commits above `66e6be6` are:

| Commit | Responsibility |
| --- | --- |
| `2395a93` | Correct CLI allocator callback signatures, including usage/user-data prefixes and realloc's extra flag. |
| `066a5d7` | Keep static LLVM allocas in the true function entry and reject non-static or malformed native stack-usage records. |
| `75e8f91` | Explicit upstream-standard, numeric-only and legacy-zero block-type admission in the existing loader. |

The combined eight-file diff was independently reviewed at SHA-256
`4527c2218906654e914396dc0be7a6cc0bdb7fcaea0e05f94181542190e9ce92`.
The coordinator published this head to the existing draft
[fork PR 1](https://github.com/SystasisIO/wasm-micro-runtime/pull/1).
Forge changes remain local/pending. Publication is not product acceptance.
Old compiler identity sidecars must not be rewritten onto old code.

## Block-Type Policy

Forge propagates the existing `allow_zero_blocktype` option into its private
runtime and isolated compiler loaders. Default/false admits only one-byte
numeric/void block types. True additionally treats a literal zero byte as void,
not as type index zero. Overlong zero and other type-index encodings remain
outside this Forge profile. Normalization modifies only the loader's owned
copy, never the caller's WASM or the artifact's original source/digest.

The generic upstream loader/CLI keeps standard typed-block behavior by default.
Both public WAMR header declarations of `LoadArgs` have identical layout and
include-order checks. The optional C API's WASM-only cache explicitly refuses
nonstandard modes before lookup; it cannot reuse a STANDARD entry to bypass a
stricter policy. Forge's shared production profile does not enable that cache.

The exact candidate passed 56 checks each in classic and fast interpreters.
The conditional-cache harness passed 2/2 CTests, including a standard-cache warmup,
strict/legacy refusal and successful subsequent standard reuse. It deliberately
enables an otherwise disabled cache with installed OpenSSL for that conditional
path; it is not a production-profile matrix. These bounded fixtures execute
accepted block/loop/if modules, not merely parse them. Permanent Forge compiler
and public API regression results are recorded separately after rebuilding.

## Allocator Handoff

The baseline public consumer test fails cached A-reset-B-reset-A on one caller
mapping. The repair keeps one mapping and one existing usage dispatcher.
An allocator transition retires the idle instance before changing pages;
active guest execution, host calls and argument conversion retain activity
ownership. The registration callback pins the context during retirement and
synchronizes with destruction. No product-specific retirement call is required.

Implementation passed independent lifetime/concurrency review. The review caught
one error-category race: a busy registration-to-activation transition must retain
`busy`, not become `execution_unavailable`. The narrow rethrow is now included.
Public execution passes **35/35 cases, 996 assertions**, with 100 repetitions.
The coordinator independently repeated the six selected CTests, **6/6 PASS**,
including invocation, execution, engine memory, consumer API and two allocation
failure probes. Parent evidence is `public-parent.xml` under the raw directory.
The unchanged eight donor-sharing cases and six donor block-type cases pass.
Full donor unit is **168/175**: its only seven remaining cases are six approved
malformed-permission differences and the approved extreme-depth rejection.
That historical suite is not reported as green or silently rewritten.

Developer logs are `build/wamr-public-sidecar/borrow-p1-75e8-final-*`.
Cached A-reset-B-reset-A succeeds; 32 allocation-failure/recovery cycles pass.
The allocator's explicit `free()` retains its existing lifetime precondition:
destroy attached backends and release manual borrows first. It is not a new
throwing/mid-invocation mutation API. A max-memory diagnostic warning remains
in the cached probe; it does not fail the scenario or establish an additional
supported capacity. Full product compilation/execution is still separate.

## CLI And Native Stack

The isolated CLI repair passed Release and ASan/function-sanitized 2/2 CTests.
Each recovery test performs 16 successful compiles and 32 controlled failures.
The unchanged callback ABI allocated one byte through `malloc(0)` and then wrote
89/343 bytes; ASan reproduced that defect. An independent LLVM teardown defect
was not reproduced after correcting callbacks.

The heavy-frame diagnostic found that depth instrumentation moved the original
entry block, leaving static allocas outside the true LLVM entry. Actual frames
were roughly three times the reported size. The reviewed fix preserves static
placement and validates the stack-usage token instead of accepting a partial
`sscanf` match. Fresh 32-KiB/1024-depth execution and cancellation/reuse remain
required; no logical cap was lowered or production stack default inflated.

Fresh combined-pin compiler checks now pass **7/7 Release and 7/7 ASan/function**
tests in `tests/vm_wamr/compiler/build-fork-66e6be6-usage1` and
`tests/vm_wamr/compiler/build-fork-75e8f91-asan`. Logs are
`ctest-75e8f91.log`. They cover strict/legacy policy and byte-identical canonical
void compilation, immutable inputs, recovery, page/layout admission and the
generic CLI's separately standard typed-block behavior.

Fresh root AOT checks pass **36/36**, independently repeated by the coordinator
in `build/wamr-oc-sidecar-65f974b`; parent XML is `aot-parent.xml` under the
raw-evidence directory named above. This includes a *fail-closed* session case,
not successful exact reservation. Three live source shapes use 8/16/32 KiB at
logical depth 1024 (1023 guest frames plus one host frame). Debugger sampling of
the 32-KiB shape measures a 32,832-byte native frame against 32,872 metadata bytes,
with 29,292,680 bytes left above the supplied 64-MiB diagnostic boundary.
An 8-MiB negative control traps above its boundary and remains typed
`execution_unavailable`; reuse and floating-point environment checks pass.
Detailed source/binary/IR/log hashes are in workspace
`artifacts/wamr-public-migration-20261016/aot-physical-stack-75e8f91-evidence.json`.
These are Darwin ARM64 results for selected live shapes, not all shapes, a
production stack default, Linux proof or cross-architecture consensus proof.

## Preparation Reservation

The [stage-1 allocation-owner prototype](../iterations/forge-wamr-preparation-reservation/design.md)
is private, isolated and not connected to the loader or session. It exercises
bounded temporary storage, allocation/map-intent accounting and cleanup without
native mappings or executable publication. It does not establish an exact
artifact plan. `artifact::prepared_byte_size()` remains fail-closed.

The final owner passed **16/16 Debug and 16/16 ASan/UBSan**, independently
repeated by reviewer and coordinator. Review caught a cancellation-during-resize
case: returning failure after releasing the old block violated resize semantics.
Before the commit point cancellation now releases only the provisional block;
after commit the operation returns its new pointer. The regression covers both
outcomes. The existing optimizing test graph now registers this isolated owner;
its root Release build passes **16/16**. The prototype is still not linked into
the production runtime or session.

Required follow-up is the same WAMR loader's bounded planning/commit path,
including C++ bridge storage, precharged temporary overlap and the existing
session's resident reservation. Do not replace this with a byte multiplier,
an after-load measurement, a second format parser/cache/scheduler, or an
unbudgeted direct preparation path.

## Remaining Acceptance

The first complete current-profile rerun configured and built all four warm
lanes, then executed every test: **128 passed / 20 failed / 0 not run** in each
of classic, fast, ASan/UBSan classic and ASan/UBSan fast. Baseline totals are
**512 passed / 80 failed / 0 not run**, not a green matrix. Exact paths and
source/compiler/runtime hashes are in
`build/wamr-profile-classic-65f974b-20261016/quad-results-75e8f91.json`.
The six binding-memory cases expect `invalid_argument` for a zero page budget,
but zero is now a valid budget and the positive initial memory must instead
fail with the same `length_error` as any below-minimum cap. The assertion still
requires rejection; its corrected private category passes all six classic
cases. Source order places that check before instantiation; this is not a new
allocation-side-effect assertion. Fourteen exception-info cases failed before
their assertions because their raw instantiation fixture bypassed the
usage-aware memory owner. The test-only repair supplies distinct explicit owners
for the main instance and cluster peer, with scopes covering allocation, calls
and teardown. Its negative control first requires refusal without an owner, then
successful explicit-owner reuse. All exception status/ID/message, priority,
depth and recovery assertions remain. The independently reviewed fixture SHA-256
is `4b6ef967ad4bb1acbe57bf443c93287fdc60d7d3a93b6a399ed27c3a389d191f`.
No production fallback or engine change was needed for these harness failures.

The full postfixture rerun passes **148/148 in each of all four lanes**:
**592 passed / 0 failed / 0 not run / 0 skipped**. Both sanitized lanes retain
ASan/UBSan. The baseline failure report above remains unchanged; the new report,
XMLs, logs and source/binary hashes are linked from
`build/wamr-profile-classic-65f974b-20261016/quad-results-75e8f91-postfixture.json`.
The coordinator checked all four XML case/failure counts and the frozen fixture
hash independently. Standalone compiler/runtime qualification is now green for
this Darwin ARM64 pin/profile, not for other platforms or product deployment.

Exact session/cache success, installed consumers, real downstream execution,
all-shape physical provisioning and cross-architecture determinism remain open.
Current standalone, selected public consumer and selected live AOT tests above
do not replace those gates. Historical malformed-input
permissions and the extreme-depth donor assertion retain their original bodies;
the approved replacement profile requires rejection, not a green historical
parity claim. No additional compatibility break is approved here.

## Completion Block R3 In Progress

The coordinator replaced the old primary backend implementation with selection
of the shared WAMR `runtime_backend`; no EOS code-protection cancellation or
machine-code writer is selected by the public `interpreter`, `null_backend` or
`jit` backend. This is a source checkpoint, not a successful new build or a
claim that all residual donor-derived API/parser/numeric code has been removed.
The separate provenance acceptance gate remains open.

`FORGE_ENABLE_VM_WASM_LLVM_JIT` is explicitly optional and defaults to `OFF`.
The ordinary interpreter/AOT runtime must install and relocate with LLVM
discovery disabled. The opt-in runtime uses one WAMR runtime target and private
LLVM 22.1.8 linkage; the isolated AOT compiler retains JIT-disabled definitions.
Effective JIT features, LLVM version, native target, native CPU selection,
optimization settings and caller-thread preparation enter the runtime identity.
Native JIT selection must be checked before guest initialization; disabled or
unprepared JIT is a typed refusal, never interpreter/AOT fallback. Positive
public JIT execution and its native-map manager are not yet qualified.

A narrow source audit distinguishes the old 1-GiB parser/codegen arena from an
overall process-memory promise. The old executable pool could add segments and
its C++ metadata used ordinary heap allocation. The replacement must bound
Forge/WAMR-owned preparation storage and native mappings, but does not promise
a total cap on LLVM IR/optimizer/STL transient heap or survival of arbitrary OS
out-of-memory termination. A mapper denial must return an operational failure,
not terminate through LLVM `ExitOnErr`. AOT session reservation retains the
separately approved conservative precharge/seal/refund contract.

The corrected private loader tests use the maintained fork checkout, not the
still-pinned production vendor checkout. They now check nonexecuting native
data pages in an allocation larger than 2 MiB, failure after earlier page
protection, and a real module unload with repeated `munmap` denial followed by
reachable cleanup after all external module/budget references have gone. The
coordinator repeated the Debug test ten times and ASan/UBSan test ten times:
both ten-run sequences pass. Evidence is in workspace
`artifacts/wamr-migration-20261016-r3/load-budget-parent-{debug,asan}.xml`.
These are Darwin ARM64 loader/ownership results, not Linux execution, a fresh
compiler-produced artifact identity or successful session/cache acceptance.
The frozen corrected snapshot awaits independent re-review before publication:
`review-load-budget-r2/fork-budget-and-defer.patch`, SHA-256
`e1749cbd5d9304dbbe737a7cbe79159bfd7f8e3ac5c73a7d4370358982981ddd`.

Independent re-review closed the earlier W^X and live-runtime cleanup findings,
then found a late-static-destruction dangling runtime pointer. The coordinator
replaced it with a weak runtime association and an explicit lifetime pin during
retry. An initial integration attempt deadlocked on a nested owner mutex and
hit the finite 60-second test timeout; the captured stack identifies that
failure. The corrected lock order is owner mappings before the runtime list,
with no nested acquisition of the same mutex. New Debug and ASan/UBSan tests
each pass ten repeats, including a real failed unload, successful unregister,
runtime singleton destruction, then a late preparation-owner retry. New XMLs
are `load-budget-parent-lifetime-{debug,asan}.xml`. The immutable narrow fix is
`review-load-budget-r2/late-runtime-weak-owner-v2.tar.gz`, SHA-256
`444c8307229b750dd69f3c55f61d6402e8c2516fa06badfe4001ead2c829ce88`;
its final independent review is scoped clear. The initial failed attempt is not
counted as a pass or suppressed by the successful rerun.

The build/profile contract configures successfully with JIT both disabled and
enabled. The WASM-family structure regression suite passes 12/12, including
rejection of an unguarded LLVM dependency, a dependency in the default `else`
branch, a compiler/SDK reverse dependency inside the JIT guard, and LLVM linkage
from another VM leaf. Full structure lint passes, scanning 2,732 first-party
source files. The opt-in full JIT build graph also configures successfully;
configuration is not positive native execution evidence.

The complete install build is warm but not accepted. Two narrow OpenSSL 4
prerequisites were repaired using public ASN.1 accessors and an owned X.509 name;
the PKI unit target passes. Three test certificate fixtures needed the same
public X.509 name API. The complete P2P test binary builds, and four selected
certificate checks pass with eight assertions. The remaining 1,277 cases were
filtered out, not run or claimed green. Evidence is
`artifacts/wamr-migration-20261016-r3/p2p-openssl4-certificates.xml`.
The reviewed loader/defer slice is now published in fork commit
`156dbd0b6edea3d64b7e21d305913943b60b5f91` and pinned as a clean vendor checkout.
The exact 11-file snapshot is source-reviewed with JIT disabled; the two extra
C-API cache/mode guards are included. The independent header-order ABI test
passes for both public C header orders and zero-default fields. AOT/LLVM native
execution and cache-enabled behavioral qualification are separate gates.

The normal public build now compiles the runtime, execution, preparation and
private load probes. Its first run was 6/9: a moved-from `std::function` was
still callable under the valid libc++ move contract, and two private probes
incorrectly expected a native-stack ID from interpreter logical-depth failure.
The callback transfer now explicitly clears its source, matching the documented
consumed-input promise. The raw probe requires the dedicated depth flag in both
modes and tests their actual distinct internal exception classifications; the
public adapter already gives the same typed logical-depth trap. No depth limit
or cancellation assertion was weakened. The fresh rerun passes **9/9 CTests**:
invocation, execution, engine memory, consumer API, preparation budget, deferred
and legacy bytecode load, disabled-JIT refusal and header-order ABI. The last
four are **not positive native JIT execution**. Raw first/postfix reports are
`public-156dbd0.xml` and `public-156dbd0-postfix.xml` in the R3 artifact directory.
Complete installed/relocated acceptance is still pending; do not label this
install green. Fresh AOT helper/fixtures/session tests are building separately.

Isolated current Spine/Turbine source copies are prepared under workspace
`artifacts/wamr-migration-20261016-r3/consumers`; no live product source/pin was
changed. Their native builds, contracts, processes and installed-package gates
remain NOT_RUN for this completion block. All Forge changes remain pending.

### Installed Runtime And Fresh AOT Checkpoint

The complete runtime-only install and relocated consumer now pass with both
LLVM and Clang package discovery explicitly disabled. That configuration has
tooling/compiler/JIT disabled and optimizing execution enabled. The consumer
executes a real intrinsic (41 to 42), checks pre-cancellation without a host
effect, and reuses the module after cancellation. The private runtime archive
SHA-256 is `b2b6570a1037b3fc5b8b2597cd55183a15d93a142fffc60e8abd5e64bcb7d413`;
the installed smoke binary SHA-256 is
`40d43849ef42084ac3abd8f2c0d538e1d1a5f45fb3367aef74798d10105732aa`.
Evidence is `runtime-only-package-check.tar.gz` in the R3 directory. A separate
tooling-enabled install is in progress for the actual downstream test hosts;
its LLVM dependency does not change the LLVM-free runtime-only result.

Fresh compiler-produced fixtures at pin `156dbd0` pass **42/43 AOT CTests**.
The remaining bounded-session test rejected its second unverified admission
before execution: its synthetic 16-MiB response cap reserved more than half of
the 512-MiB shared budget per record. The test now uses the actual Spine
1-MiB request/response caps, retaining the same shared budget. The largest fresh
artifact is 722,146 bytes and request is 41,274 bytes. Three queued-record
precharges and rejection of a fourth are checked before scheduler progress.
The first failed run remains `aot-156dbd0.xml`; the corrected test is not yet
reported as passed.

An independent default-workspace issue was also repaired: preparation with an
inherited limit now reserves at most the shared headroom under the same account
mutex, after all already-admitted records. The real loader enforces this ceiling;
explicit nonzero workspace limits are not silently shrunk. The bounded loader
still seals actual usage and releases the unused reservation. This narrow
worker/public-comment change is source-reviewed; successful session execution,
post-preparation cancellation and overlapping preparation remain separate
verification gates.

Execution contexts now inherit the prepared bytecode module's fixed JIT mode
when using the existing two-argument constructor. A backend rejects an attached
context from another module or execution mode before replacing its current
pointer. The new regression also checks rejection leaves the original context
usable, explicit detach, and reattachment/reuse. This source slice still needs
the next public-runtime test run; it is not positive native JIT evidence.

### Residual Donor Source Gate

A bounded source audit confirms that the selected backend calls the WAMR
instance, but the interpreter target still includes EOS-derived helpers and
unselected legacy execution code. In particular:

- The live host bridge still uses `host_function`, argument/span conversion,
  guest-pointer validation, stack values and portions of the allocator/types
  modules. These need an implementation/provenance review, not file deletion.
- Public legacy parser/context aliases retain `parser`, writers, old module
  containers, visitors, signals and growable/JIT arenas. No use of
  `binary_parser`, those parser/context aliases, `jit_allocator` or
  `growable_allocator` was found in the isolated Spine/Turbine source copies
  outside vendored code and documentation. Removal of these public surfaces is
  a proposed additional compatibility break, awaiting maintainer approval.
- Old OC-shaped artifact metadata is rejected when nonempty, but removing its
  public fields also changes serialization. That is not an implied cleanup
  permission. Spring/WAVM provenance and Berkeley SoftFloat must be assessed
  separately rather than labelled universally as EOS VM code.

Consequently this checkpoint does not establish an EOS-source-free Forge or
resolution of licensing concerns. Notices/licenses have not been changed.

### Tooling And Consumer Build Follow-Up

The first tooling-enabled install failed because the validator still required
the old `func_type` and direct `module.exports` projection. It now uses existing
`prepared_module::function_signature()` views and `get_exported_function()`;
the public validation request is unchanged. No duplicate export projection or
second parser was added. Six real validator fixtures cover approved imports and
`apply`, nonexecuting validation of a trapping start, parameter count/type and
result mismatch, missing/nonfunction exports, wrong `apply` signature and
nonfunction imports. The new frozen source passes **10/10 public/tooling
CTests**, including the original nine public runtime checks and the tooling
suite. The JIT-disabled tests still do not establish positive native execution.
Evidence: `public-tooling-156dbd0.xml` in the R3 directory.

The new private session observation component initially failed to compile
because this libc++ does not implement `atomic<shared_ptr>`. Its corrected
fixed-point observer uses a mutex-protected shared pin, waiting only after
releasing that mutex. It has two cold test phases, not a new public runtime API,
queue or scheduler. The successful tooling-enabled install is preserved in
`package-install-tooling-156dbd0-r3-postfix.log`; earlier failures remain intact.

An isolated copy of that installed package is held at
`consumers/forge-156dbd0-tooling-prefix`. Its archive SHA-256 values are:

| Archive | SHA-256 |
| --- | --- |
| `libforge_vm_wasm_interpret.a` | `ab59da52be6491e43ee65fe2250771fa014333dda14b173380d105b62bb6b82c` |
| `libforge_vm_wasm_optimizing.a` | `e7c0135fc90f7d6a78e9f40a685679e8783d8b89b507caca8548eed263b61887` |
| `libforge_tooling_validation.a` | `c79913800424e83893c8bd47d06a4cd16c4c80062d8210417b20836bc98141dc` |

Spine now configures against that copy. Its existing supported
`SPINE_FORGE_NOTICES_DIR` option points at the verified installed notices,
including `LICENSE.eos-vm`; no notices were suppressed. Real contract and
process targets are building, not yet accepted.

Current Turbine cannot configure against the experiment's older Forge base
`8975f234`: its clean vendor is `3a6e388f`, and the intervening series supplies
JSON document values, HTTP/SSE/guest streams, secret-version APIs and admission
contracts. Eight direct module imports are absent in the older base. An alias
for the first missing target would not establish compatibility. Matching guest
SDK and AWS prefixes are also unavailable in the bounded local search. The
separate `consumers/turbine-acceptance.md` records exact references and separates
the historical ABI-2/manifest-4 baseline from the current ABI-3/manifest-5 copy.
Moving the WAMR slice to a separate current-dev worktree, without modifying the
existing dirty experiment, awaits maintainer agreement.

### Overnight Source And Execution Checkpoint

The source-reviewed eager JIT slice is published in the maintained fork as
`ed5c12ae6bcec88f2863aa7c541b6d829d657a4c`, with a clean vendor checkout and
matching strict CMake pin. Its exact source manifest is
`tests/vm_wamr/jit/source-freeze-orc-v2.json`, SHA-256
`334af80c0c2c7028f9f6ce5a5f2f2181f32b89d883f3601d7af05b2c0aba72fc`.
All 21 source hashes matched again before publishing the 12 fork files. The
same draft PR remains experimental. The LLVM JIT identity additionally names
Large code model, PIC relocation and Aggressive code generation. Its clean
JIT-enabled configure passes; native execution at this pin remains NOT_RUN.
No Forge commit/push/merge or license-term changes were performed.

The preceding `156dbd0` AOT run is **44/45**, not green. Both new fixed-phase
tests pass: late post-work cancellation reports `actual_resident=681659` and
one actual compute cancellation, with no ready/disk publication, exact drain
and successful reuse of the same compute pool. Two simultaneous preparation
reservations report `291659106 + 122604223 + 122604223 = 536867552` inflight
bytes under the unchanged `536870912`-byte cap, and `67108864` resident bytes.
Both artifacts then complete with distinct mappings and exact final refunds.
These are accounted reservation ceilings, not RSS measurements. Independent
review of the fixed private observer, lock boundaries and tests is scoped clear.

The main session test first expected a concrete local C++ exception for a
runtime enum returned by the isolated helper. The corrected helper-only check
requires `runtime_coded_exception<optimizing::exceptions::code>` and exact enum,
value/category equality; local quota assertions retain their concrete types.
Wrong enum code, unexpected success and a local alias are negative controls.
This matches the existing `FORGE_THROW_CODE` rule, without changing production
throws or parsing error text. The next run reaches eviction, where a real
default resident-ceiling defect was reproduced with an old weak pin. The new
reviewed default cap is bounded by the remaining aggregate account after the
existing eviction path; explicit nonzero caps never shrink. Its five-file
review SHA is `4e2ab41820925a099250c964ea624a32e22800325a5d07201d26b29d0c35d4f5`.
Execution of that last fix is still pending; both failed runs are preserved as
`aot-156dbd0-postfix.xml` and `aot-156dbd0-typed-enum.xml`.

Spine's isolated host target passes **9/9 Boost cases** against the frozen
`156dbd0` tooling prefix, including real execution of all eight system contracts.
The two explicit test-only changes are a generic const byte-container helper
instead of the old parser's `guarded_vector`, and the malformed-header golden
diagnostic. The latter still requires the exact harness prefix and refusal
before invocation; no input/assertion is removed. The initial diagnostic
failure and corrected pass are `consumers/spine-host-r3.xml` and
`consumers/spine-host-r3-diagnostic.xml`. Production/controller/bootstrap process
acceptance is **BLOCKED**, not passed: Spine's separate Forge dependency
`c6ff5ee8`/`f6b617dd` supplies DB allocation-snapshot APIs absent from this base.
Current Turbine remains independently blocked by the newer dev API/SDK gap.
